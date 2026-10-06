#include "aibox/snapshot_writer.hpp"

#include <opencv2/imgcodecs.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <ctime>
#include <cstdint>
#include <fcntl.h>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace aibox {
namespace {
constexpr std::size_t kMaxSnapshotQueue = 128;
constexpr std::size_t kMaxEncodedSnapshotBytes = 32 * 1024 * 1024;

bool safe_event_id(const std::string& id) {
  if (id.empty() || id.size() > 64) return false;
  return std::all_of(id.begin(), id.end(), [](unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || c == '-';
  });
}

std::string relative_snapshot_path(const std::string& id) {
  if (!safe_event_id(id)) return {};
  return "snapshots/" + id + ".jpg";
}

std::string utc_cutoff(int retention_days) {
  const auto now = std::chrono::system_clock::now() - std::chrono::hours(24LL * retention_days);
  const std::time_t value = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
  gmtime_r(&value, &tm);
  std::ostringstream out;
  out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S") << ".000Z";
  return out.str();
}

bool write_all(int fd, const std::uint8_t* data, std::size_t size) {
  std::size_t written = 0;
  while (written < size) {
    const ssize_t count = ::write(fd, data + written, size - written);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) return false;
    written += static_cast<std::size_t>(count);
  }
  return true;
}

bool is_safe_regular_file(const std::filesystem::path& path) {
  struct stat info{};
  return ::lstat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

bool restrict_snapshot_file(const std::filesystem::path& path) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) return false;
  struct stat info{};
  const bool valid = ::fstat(fd, &info) == 0 && S_ISREG(info.st_mode) &&
      info.st_uid == ::geteuid() && ::fchmod(fd, S_IRUSR | S_IWUSR) == 0;
  ::close(fd);
  return valid;
}

std::uint64_t safe_file_size(const std::filesystem::path& path) {
  struct stat info{};
  if (::lstat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0)
    return 0;
  return static_cast<std::uint64_t>(info.st_size);
}

}  // namespace

SnapshotWriter::SnapshotWriter(EventStore& events, std::filesystem::path data_directory,
                               StorageConfig config, std::size_t queue_capacity)
    : events_(events), data_directory_(std::move(data_directory)),
      snapshot_directory_(data_directory_ / "snapshots"), config_(config),
      queue_capacity_(queue_capacity) {
  if (data_directory_.empty() || queue_capacity_ == 0 ||
      queue_capacity_ > kMaxSnapshotQueue || config_.retention_days < 1 ||
      config_.max_snapshot_bytes < 1)
    throw std::invalid_argument("Invalid snapshot writer configuration");
  std::filesystem::create_directories(snapshot_directory_);
  if (::chmod(snapshot_directory_.c_str(), S_IRWXU) != 0)
    throw std::runtime_error("Cannot restrict snapshot directory permissions");
  reconcile();
  enforce_retention();
  worker_ = std::thread([this] { run(); });
}

SnapshotWriter::~SnapshotWriter() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
  }
  changed_.notify_all();
  if (worker_.joinable()) worker_.join();
  try {
    events_.flush();
  } catch (...) {
  }
}

bool SnapshotWriter::enqueue(const std::string& event_id, const cv::Mat& bgr) {
  if (!safe_event_id(event_id) || bgr.empty() || bgr.type() != CV_8UC3) {
    set_error("Snapshot enqueue received an invalid event ID or BGR frame");
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || jobs_.size() >= queue_capacity_) {
      error_ = "Snapshot queue is full or stopping";
      return false;
    }
    jobs_.push_back({event_id, bgr});  // OpenCV ref-counted, immutable frame storage.
  }
  changed_.notify_one();
  return true;
}

void SnapshotWriter::configure(StorageConfig config) {
  if (config.retention_days < 1 || config.max_snapshot_bytes < 1)
    throw std::invalid_argument("Invalid snapshot storage configuration");
  {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = config;
    maintenance_requested_ = true;
  }
  changed_.notify_one();
}

bool SnapshotWriter::healthy() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return error_.empty();
}

std::string SnapshotWriter::last_error() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return error_;
}

std::filesystem::path SnapshotWriter::snapshot_directory() const {
  return snapshot_directory_;
}

void SnapshotWriter::set_error(const std::string& error) {
  std::lock_guard<std::mutex> lock(mutex_);
  error_ = error;
}

StorageConfig SnapshotWriter::current_config() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return config_;
}

void SnapshotWriter::run() {
  auto next_retention = std::chrono::steady_clock::now() + std::chrono::minutes(10);
  for (;;) {
    Job job;
    bool has_job = false;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      changed_.wait_until(lock, next_retention,
                          [&] { return stopping_ || maintenance_requested_ || !jobs_.empty(); });
      const bool maintenance = maintenance_requested_;
      maintenance_requested_ = false;
      if (!jobs_.empty()) {
        job = std::move(jobs_.front());
        jobs_.pop_front();
        has_job = true;
      } else if (stopping_) {
        break;
      }
      if (maintenance && !has_job) {
        lock.unlock();
        try { enforce_retention(); }
        catch (const std::exception& error) { set_error(error.what()); }
        continue;
      }
    }
    if (has_job) {
      try {
        write_snapshot(job);
      } catch (const std::exception& error) {
        const auto final_path = snapshot_directory_ / (job.event_id + ".jpg");
        const auto temp_path = snapshot_directory_ / (job.event_id + ".jpg.tmp");
        ::unlink(final_path.c_str());
        ::unlink(temp_path.c_str());
        set_error(error.what());
        const auto path = relative_snapshot_path(job.event_id);
        if (events_.enqueue_snapshot_state(job.event_id, "failed", path))
          events_.flush();
      }
      try {
        enforce_retention();
      } catch (const std::exception& error) {
        set_error(error.what());
      }
    }
    if (std::chrono::steady_clock::now() >= next_retention) {
      try {
        enforce_retention();
      } catch (const std::exception& error) {
        set_error(error.what());
      }
      next_retention = std::chrono::steady_clock::now() + std::chrono::minutes(10);
    }
  }
}

void SnapshotWriter::write_snapshot(const Job& job) {
  const std::string relative = relative_snapshot_path(job.event_id);
  if (relative.empty()) throw std::invalid_argument("Invalid snapshot event ID");
  events_.flush();
  if (!events_.enqueue_snapshot_state(job.event_id, "pending", relative))
    throw std::runtime_error("Could not queue pending snapshot state");
  events_.flush();

  std::vector<std::uint8_t> jpeg;
  if (!cv::imencode(".jpg", job.bgr, jpeg, {cv::IMWRITE_JPEG_QUALITY, 85}) || jpeg.empty())
    throw std::runtime_error("OpenCV failed to encode event snapshot");
  const auto config = current_config();
  if (jpeg.size() > kMaxEncodedSnapshotBytes ||
      jpeg.size() > static_cast<std::uint64_t>(config.max_snapshot_bytes))
    throw std::runtime_error("Event snapshot exceeds configured size limit");

  const auto final_path = snapshot_directory_ / (job.event_id + ".jpg");
  const auto temp_path = snapshot_directory_ / (job.event_id + ".jpg.tmp");
  const int fd = ::open(temp_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                        S_IRUSR | S_IWUSR);
  if (fd < 0) throw std::runtime_error("Cannot create temporary event snapshot");
  const bool written = write_all(fd, jpeg.data(), jpeg.size());
  const int sync_result = written ? ::fsync(fd) : -1;
  const int close_result = ::close(fd);
  if (!written || sync_result != 0 || close_result != 0) {
    ::unlink(temp_path.c_str());
    throw std::runtime_error("Cannot write event snapshot completely");
  }
  if (::rename(temp_path.c_str(), final_path.c_str()) != 0) {
    ::unlink(temp_path.c_str());
    throw std::runtime_error("Cannot atomically publish event snapshot");
  }
  const int directory_fd = ::open(snapshot_directory_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (directory_fd < 0) throw std::runtime_error("Cannot open snapshot directory for sync");
  const int directory_sync = ::fsync(directory_fd);
  ::close(directory_fd);
  if (directory_sync != 0) throw std::runtime_error("Cannot sync snapshot directory");
  if (!events_.enqueue_snapshot_state(job.event_id, "ready", relative))
    throw std::runtime_error("Could not queue ready snapshot state");
  events_.flush();
  const auto stored = events_.get(job.event_id);
  if (!stored || stored->snapshot_state != "ready")
    throw std::runtime_error("Snapshot state did not persist as ready");
  {
    std::lock_guard<std::mutex> lock(mutex_);
    error_.clear();
  }
}

void SnapshotWriter::reconcile() {
  events_.flush();
  std::vector<StoredEvent> rows;
  for (std::size_t offset = 0;; offset += 200) {
    auto page = events_.retention_candidates(offset, 200);
    const auto count = page.size();
    rows.insert(rows.end(), page.begin(), page.end());
    if (count < 200) break;
  }

  std::set<std::string> referenced;
  for (const auto& event : rows) {
    const std::string relative = relative_snapshot_path(event.id);
    if (relative.empty()) continue;
    const auto file = snapshot_directory_ / (event.id + ".jpg");
    const auto temp = snapshot_directory_ / (event.id + ".jpg.tmp");
    if (event.snapshot_state == "pending" || event.snapshot_state == "deleting" ||
        event.snapshot_state == "expiring") {
      ::unlink(file.c_str());
      ::unlink(temp.c_str());
      if (event.snapshot_state == "deleting") {
        if (!events_.enqueue_delete_event(event.id))
          throw std::runtime_error("Could not reconcile retained snapshot deletion");
      } else if (event.snapshot_state == "expiring") {
        if (!events_.enqueue_snapshot_state(event.id, "expired"))
          throw std::runtime_error("Could not reconcile expired active snapshot");
      } else if (!events_.enqueue_snapshot_state(event.id, "failed")) {
        throw std::runtime_error("Could not reconcile interrupted snapshot");
      }
    } else if (event.snapshot_state == "ready") {
      if (event.snapshot_path == relative && is_safe_regular_file(file) &&
          restrict_snapshot_file(file))
        referenced.insert(file.filename().string());
      else if (!events_.enqueue_snapshot_state(event.id, "failed"))
        throw std::runtime_error("Could not mark missing snapshot as failed");
    }
  }

  for (const auto& item : std::filesystem::directory_iterator(snapshot_directory_)) {
    if (!item.is_regular_file()) continue;
    const auto name = item.path().filename().string();
    const bool temporary = name.size() >= 4 && name.compare(name.size() - 4, 4, ".tmp") == 0;
    if (temporary ||
        (item.path().extension() == ".jpg" && referenced.count(name) == 0))
      std::filesystem::remove(item.path());
  }
  events_.flush();
}

void SnapshotWriter::enforce_retention() {
  const auto config = current_config();
  events_.flush();
  std::vector<StoredEvent> rows;
  for (std::size_t offset = 0;; offset += 200) {
    auto page = events_.retention_candidates(offset, 200);
    const auto count = page.size();
    rows.insert(rows.end(), page.begin(), page.end());
    if (count < 200) break;
  }
  const auto cutoff = utc_cutoff(config.retention_days);
  std::uint64_t total_bytes = 0;
  for (const auto& event : rows) {
    if (event.snapshot_state != "ready" || relative_snapshot_path(event.id).empty()) continue;
    const auto file = snapshot_directory_ / (event.id + ".jpg");
    const auto size = safe_file_size(file);
    total_bytes = size > UINT64_MAX - total_bytes ? UINT64_MAX : total_bytes + size;
  }

  for (const auto& event : rows) {
    const bool expired_by_age = !event.active && event.started_utc < cutoff;
    const auto relative = relative_snapshot_path(event.id);
    const auto file = relative.empty() ? std::filesystem::path{}
                                      : snapshot_directory_ / (event.id + ".jpg");
    const auto size = event.snapshot_state == "ready" ? safe_file_size(file) : 0;
    const bool over_capacity = total_bytes > static_cast<std::uint64_t>(config.max_snapshot_bytes);
    if (!expired_by_age && (!over_capacity || size == 0)) continue;

    if (size > 0) {
      if (!events_.enqueue_snapshot_state(event.id,
              event.active ? "expiring" : "deleting", relative))
        throw std::runtime_error("Could not queue snapshot retention state");
      events_.flush();
      if (::unlink(file.c_str()) != 0 && errno != ENOENT)
        throw std::runtime_error("Could not remove expired event snapshot");
      total_bytes -= std::min(total_bytes, size);
      if (event.active) {
        if (!events_.enqueue_snapshot_state(event.id, "expired"))
          throw std::runtime_error("Could not queue active snapshot expiration");
        events_.flush();
        continue;
      }
    }
    if (!events_.enqueue_delete_event(event.id))
      throw std::runtime_error("Could not queue expired event metadata");
    events_.flush();
  }
  for (const auto& item : std::filesystem::directory_iterator(snapshot_directory_)) {
    if (item.is_regular_file() && item.path().extension() == ".tmp")
      std::filesystem::remove(item.path());
  }
}

}  // namespace aibox

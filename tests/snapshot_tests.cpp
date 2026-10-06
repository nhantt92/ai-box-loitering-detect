#include "aibox/snapshot_writer.hpp"

#include <opencv2/imgcodecs.hpp>

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

using namespace aibox;
using namespace std::chrono_literals;

namespace {
const std::string kEventId = "0123456789abcdef0123456789abcdef-1";

std::filesystem::path test_root(const char* label) {
  return std::filesystem::temp_directory_path() /
      (std::string("aibox-snapshot-") + label + "-" + std::to_string(::getpid()));
}

Event make_event(const std::string& event_id,
                 std::chrono::system_clock::time_point started_utc,
                 Mono started = std::chrono::steady_clock::now()) {
  Event event;
  event.id = event_id;
  event.session_id = "0123456789abcdef0123456789abcdef";
  event.track_id = 3;
  event.type = "loitering";
  event.zone_id = "entry";
  event.started = started;
  event.started_utc = started_utc;
  event.ended = started;
  event.ended_utc = started_utc;
  return event;
}

void enqueue_closed_event(EventStore& store,
                          const std::string& event_id,
                          std::chrono::system_clock::time_point started_utc) {
  auto event = make_event(event_id, started_utc);
  assert(store.enqueue({event, true}));
  event.active = false;
  event.reason = "zone_exit";
  event.ended += 1s;
  event.ended_utc += 1s;
  assert(store.enqueue({event, false}));
  store.flush();
}

void writes_jpeg_off_queue_and_reconciles_pending_after_restart() {
  const auto root = test_root("async");
  std::filesystem::remove_all(root);
  {
    EventStore events((root / "events.sqlite3").string());
    enqueue_closed_event(events, kEventId, std::chrono::system_clock::now());
    {
      SnapshotWriter snapshots(events, root, StorageConfig{});
      cv::Mat frame(180, 320, CV_8UC3, cv::Scalar(20, 70, 130));
      assert(snapshots.enqueue(kEventId, frame));
      std::optional<StoredEvent> event;
      const auto deadline = std::chrono::steady_clock::now() + 3s;
      while (std::chrono::steady_clock::now() < deadline) {
        event = events.get(kEventId);
        if (event && event->snapshot_state == "ready") break;
        std::this_thread::sleep_for(5ms);
      }
      assert(event && event->snapshot_state == "ready");
      assert(event->snapshot_path == "snapshots/" + kEventId + ".jpg");
      const auto path = root / event->snapshot_path;
      struct stat info{};
      assert(::stat(path.c_str(), &info) == 0);
      assert((info.st_mode & 077) == 0);
      const cv::Mat decoded = cv::imread(path.string());
      assert(decoded.cols == 320 && decoded.rows == 180);
    }

    assert(events.enqueue_snapshot_state(kEventId, "pending",
                                         "snapshots/" + kEventId + ".jpg"));
    events.flush();
    {
      SnapshotWriter restarted(events, root, StorageConfig{});
      const auto recovered = events.get(kEventId);
      assert(recovered && recovered->snapshot_state == "failed");
      assert(!std::filesystem::exists(root / "snapshots" / (kEventId + ".jpg")));
    }
  }
  std::filesystem::remove_all(root);
}

void removes_expired_snapshot_and_metadata_as_one_retention_action() {
  const auto root = test_root("retention");
  std::filesystem::remove_all(root);
  {
    EventStore events((root / "events.sqlite3").string());
    enqueue_closed_event(events, kEventId, std::chrono::system_clock::now() - 72h);
    const auto directory = root / "snapshots";
    std::filesystem::create_directories(directory);
    cv::Mat frame(80, 100, CV_8UC3, cv::Scalar(4, 5, 6));
    const auto path = directory / (kEventId + ".jpg");
    assert(cv::imwrite(path.string(), frame));
    assert(::chmod(directory.c_str(), 0700) == 0);
    assert(::chmod(path.c_str(), 0600) == 0);
    assert(events.enqueue_snapshot_state(kEventId, "ready",
                                         "snapshots/" + kEventId + ".jpg"));
    events.flush();
    StorageConfig retention;
    retention.retention_days = 1;
    {
      SnapshotWriter snapshots(events, root, retention);
      assert(!events.get(kEventId));
      assert(!std::filesystem::exists(path));
    }
  }
  std::filesystem::remove_all(root);
}

void enforces_total_snapshot_byte_limit_oldest_first() {
  const auto root = test_root("quota");
  std::filesystem::remove_all(root);
  const std::string first_id = "0123456789abcdef0123456789abcdef-10";
  const std::string second_id = "0123456789abcdef0123456789abcdef-11";
  {
    EventStore events((root / "events.sqlite3").string());
    const auto now = std::chrono::system_clock::now();
    enqueue_closed_event(events, first_id, now - 10min);
    enqueue_closed_event(events, second_id, now);
    const auto directory = root / "snapshots";
    std::filesystem::create_directories(directory);
    assert(::chmod(directory.c_str(), 0700) == 0);
    std::vector<char> bytes(700 * 1024, 'x');
    for (const auto& id : {first_id, second_id}) {
      const auto path = directory / (id + ".jpg");
      std::ofstream output(path, std::ios::binary);
      output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
      output.close();
      assert(::chmod(path.c_str(), 0600) == 0);
      assert(events.enqueue_snapshot_state(id, "ready", "snapshots/" + id + ".jpg"));
    }
    events.flush();
    StorageConfig limits;
    limits.retention_days = 30;
    limits.max_snapshot_bytes = 1024 * 1024;
    {
      SnapshotWriter snapshots(events, root, limits);
      assert(!events.get(first_id));
      assert(!std::filesystem::exists(directory / (first_id + ".jpg")));
      assert(events.get(second_id));
      assert(std::filesystem::exists(directory / (second_id + ".jpg")));
    }
  }
  std::filesystem::remove_all(root);
}

void quota_can_expire_active_snapshot_without_dropping_live_event() {
  const auto root = test_root("active-quota");
  std::filesystem::remove_all(root);
  {
    EventStore events((root / "events.sqlite3").string());
    const auto event = make_event(kEventId, std::chrono::system_clock::now());
    assert(events.enqueue({event, true}));
    events.flush();
    const auto directory = root / "snapshots";
    std::filesystem::create_directories(directory);
    assert(::chmod(directory.c_str(), 0700) == 0);
    const auto path = directory / (kEventId + ".jpg");
    std::vector<char> bytes(1200 * 1024, 'x');
    {
      std::ofstream output(path, std::ios::binary);
      output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    assert(::chmod(path.c_str(), 0600) == 0);
    assert(events.enqueue_snapshot_state(kEventId, "ready", "snapshots/" + kEventId + ".jpg"));
    events.flush();
    StorageConfig limits;
    limits.max_snapshot_bytes = 1024 * 1024;
    {
      SnapshotWriter snapshots(events, root, limits);
      const auto retained = events.get(kEventId);
      assert(retained && retained->active && retained->snapshot_state == "expired");
      assert(!std::filesystem::exists(path));
    }
  }
  std::filesystem::remove_all(root);
}

void recovers_interrupted_active_image_expiration_without_deleting_event() {
  const auto root = test_root("active-recovery");
  std::filesystem::remove_all(root);
  {
    EventStore events((root / "events.sqlite3").string());
    const auto event = make_event(kEventId, std::chrono::system_clock::now());
    assert(events.enqueue({event, true}));
    events.flush();
    const auto directory = root / "snapshots";
    std::filesystem::create_directories(directory);
    assert(::chmod(directory.c_str(), 0700) == 0);
    const auto path = directory / (kEventId + ".jpg");
    {
      std::ofstream output(path, std::ios::binary);
      output << "snapshot";
    }
    assert(::chmod(path.c_str(), 0600) == 0);
    assert(events.enqueue_snapshot_state(kEventId, "expiring",
                                         "snapshots/" + kEventId + ".jpg"));
    events.flush();
    {
      SnapshotWriter restarted(events, root, StorageConfig{});
      const auto recovered = events.get(kEventId);
      assert(recovered && recovered->active && recovered->snapshot_state == "expired");
      assert(!std::filesystem::exists(path));
    }
  }
  std::filesystem::remove_all(root);
}
}  // namespace

int main() {
  writes_jpeg_off_queue_and_reconciles_pending_after_restart();
  removes_expired_snapshot_and_metadata_as_one_retention_action();
  enforces_total_snapshot_byte_limit_oldest_first();
  quota_can_expire_active_snapshot_without_dropping_live_event();
  recovers_interrupted_active_image_expiration_without_deleting_event();
}

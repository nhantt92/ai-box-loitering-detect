#include "aibox/preview_publisher.hpp"
#include "aibox/engines.hpp"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace aibox {
namespace {

std::vector<std::uint8_t> encode(const AnalysisFrame& frame,
                                const PreviewConfig& config) {
  if (frame.bgr.empty() || frame.bgr.type() != CV_8UC3)
    throw std::invalid_argument("Preview requires an 8-bit BGR frame");
  const double scale = std::min(1.0,
      static_cast<double>(config.max_width) / frame.bgr.cols);
  const int width = std::max(1, static_cast<int>(std::lround(frame.bgr.cols * scale)));
  const int height = std::max(1, static_cast<int>(std::lround(frame.bgr.rows * scale)));
  cv::Mat annotated;
  cv::resize(frame.bgr, annotated, cv::Size(width, height), 0, 0,
             scale < 1.0 ? cv::INTER_AREA : cv::INTER_LINEAR);

  for (const auto& zone : frame.zones) {
    if (!zone.enabled || !valid_polygon(zone.polygon)) continue;
    std::vector<cv::Point> polygon;
    polygon.reserve(zone.polygon.size());
    for (const auto& point : zone.polygon)
      polygon.emplace_back(static_cast<int>(std::lround(point.x * width)),
                           static_cast<int>(std::lround(point.y * height)));
    cv::polylines(annotated, polygon, true, cv::Scalar(220, 180, 45), 2,
                  cv::LINE_AA);
    if (!polygon.empty())
      cv::putText(annotated, zone.name,
          cv::Point(std::clamp(polygon.front().x + 3, 0, width - 1),
                    std::clamp(polygon.front().y - 5, 14, height - 1)),
          cv::FONT_HERSHEY_SIMPLEX, 0.45, cv::Scalar(220, 180, 45), 1,
          cv::LINE_AA);
  }

  for (const auto& track : frame.tracks) {
    if (!track.observed || track.confidence < 0.25f) continue;
    const int x0 = std::clamp(static_cast<int>(std::lround(track.box.x * scale)), 0, width - 1);
    const int y0 = std::clamp(static_cast<int>(std::lround(track.box.y * scale)), 0, height - 1);
    const int x1 = std::clamp(static_cast<int>(std::lround((track.box.x + track.box.w) * scale)), 0, width - 1);
    const int y1 = std::clamp(static_cast<int>(std::lround((track.box.y + track.box.h) * scale)), 0, height - 1);
    if (x1 <= x0 || y1 <= y0) continue;

    const auto active = std::find_if(frame.signals.begin(), frame.signals.end(),
        [&](const BehaviorSignal& signal) {
          return signal.track_id == track.id && signal.active;
        });
    const auto dwelling = std::find_if(frame.signals.begin(), frame.signals.end(),
        [&](const BehaviorSignal& signal) {
          return signal.track_id == track.id && signal.type == "loitering" &&
                 signal.inside && signal.observed;
        });
    const cv::Scalar color = active != frame.signals.end()
        ? (active->type == "loitering" ? cv::Scalar(40, 40, 235)
                                       : cv::Scalar(20, 145, 255))
        : (dwelling != frame.signals.end() ? cv::Scalar(0, 190, 255)
                                            : cv::Scalar(50, 220, 50));
    cv::rectangle(annotated, cv::Point(x0, y0), cv::Point(x1, y1), color, 2);
    cv::circle(annotated, cv::Point((x0 + x1) / 2, y1), 4,
               cv::Scalar(0, 220, 255), cv::FILLED, cv::LINE_AA);
    std::string label = "Person #" + std::to_string(track.id) + " " +
        std::to_string(static_cast<int>(std::lround(track.confidence * 100.0f))) + "%";
    if (active != frame.signals.end())
      label += active->type == "loitering" ? " LOITERING" : " FAST MOTION";
    else if (dwelling != frame.signals.end())
      label += " DWELL " + std::to_string(static_cast<int>(dwelling->dwell_seconds)) +
          "/" + std::to_string(static_cast<int>(dwelling->target_seconds)) + "s";
    cv::putText(annotated, label,
        cv::Point(x0, std::max(16, y0 - 5)), cv::FONT_HERSHEY_SIMPLEX,
        0.48, color, 2, cv::LINE_AA);
  }

  std::vector<std::uint8_t> jpeg;
  if (!cv::imencode(".jpg", annotated, jpeg,
                    {cv::IMWRITE_JPEG_QUALITY, config.quality}))
    throw std::runtime_error("OpenCV could not encode preview JPEG");
  return jpeg;
}

}  // namespace

PreviewPublisher::PreviewPublisher(PreviewConfig config) : config_(config) {
  if (config_.fps < 1 || config_.fps > 10 || config_.max_width < 160 ||
      config_.max_width > 1920 || config_.quality < 20 || config_.quality > 95 ||
      config_.max_clients < 1 || config_.max_clients > 8)
    throw std::invalid_argument("Invalid preview publisher configuration");
  worker_ = std::thread([this] { run(); });
}

PreviewPublisher::~PreviewPublisher() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
  }
  changed_.notify_all();
  if (worker_.joinable()) worker_.join();
}

void PreviewPublisher::submit(std::shared_ptr<const AnalysisFrame> frame) {
  if (!frame) return;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return;
    pending_ = std::move(frame);
  }
  changed_.notify_all();
}

void PreviewPublisher::configure(PreviewConfig config) {
  if (config.fps < 1 || config.fps > 10 || config.max_width < 160 ||
      config.max_width > 1920 || config.quality < 20 || config.quality > 95 ||
      config.max_clients < 1 || config.max_clients > 8)
    throw std::invalid_argument("Invalid preview publisher configuration");
  {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = config;
  }
  changed_.notify_all();
}

std::shared_ptr<const JpegFrame> PreviewPublisher::latest() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return latest_;
}

std::shared_ptr<const JpegFrame> PreviewPublisher::wait_after(
    std::uint64_t sequence, std::chrono::milliseconds timeout) const {
  std::unique_lock<std::mutex> lock(mutex_);
  changed_.wait_for(lock, timeout, [&] {
    return stopping_ || (latest_ && latest_->sequence > sequence);
  });
  return latest_ && latest_->sequence > sequence ? latest_ : nullptr;
}

bool PreviewPublisher::healthy() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return error_.empty();
}

std::string PreviewPublisher::last_error() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return error_;
}

int PreviewPublisher::max_clients() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return config_.max_clients;
}

void PreviewPublisher::run() {
  auto last_encoded = std::chrono::steady_clock::time_point::min();
  for (;;) {
    std::shared_ptr<const AnalysisFrame> frame;
    PreviewConfig encode_config;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      changed_.wait(lock, [&] { return stopping_ || static_cast<bool>(pending_); });
      if (stopping_) return;
      const auto period = std::chrono::duration<double>(1.0 / config_.fps);
      auto due = last_encoded == std::chrono::steady_clock::time_point::min()
          ? std::chrono::steady_clock::now()
          : last_encoded + std::chrono::duration_cast<std::chrono::steady_clock::duration>(period);
      if (std::chrono::steady_clock::now() < due) {
        changed_.wait_until(lock, due, [&] { return stopping_; });
        if (stopping_) return;
      }
      frame = std::move(pending_);  // Newest pending frame wins.
      pending_.reset();
      encode_config = config_;
    }
    try {
      auto bytes = std::make_shared<const std::vector<std::uint8_t>>(
          encode(*frame, encode_config));
      auto published = std::make_shared<JpegFrame>();
      published->encoded_at = std::chrono::steady_clock::now();
      published->bytes = std::move(bytes);
      {
        std::lock_guard<std::mutex> lock(mutex_);
        published->sequence = ++sequence_;
        latest_ = std::move(published);
        error_.clear();
      }
      last_encoded = std::chrono::steady_clock::now();
      changed_.notify_all();
    } catch (const std::exception& error) {
      std::lock_guard<std::mutex> lock(mutex_);
      error_ = error.what();
      changed_.notify_all();
    }
  }
}

}  // namespace aibox

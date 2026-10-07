#include "aibox/preview_publisher.hpp"
#include "aibox/engines.hpp"
#include "aibox/coco.hpp"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace aibox {
namespace {

void dashed_line(cv::Mat& image, cv::Point a, cv::Point b,
                 const cv::Scalar& color, int thickness) {
  const cv::Point delta = b - a;
  const double length = std::hypot(static_cast<double>(delta.x),
                                   static_cast<double>(delta.y));
  if (length < 1.0) return;
  const cv::Point2d direction(delta.x / length, delta.y / length);
  for (double offset = 0.0; offset < length; offset += 13.0) {
    const double end = std::min(length, offset + 8.0);
    cv::line(image,
        cv::Point(cvRound(a.x + direction.x * offset), cvRound(a.y + direction.y * offset)),
        cv::Point(cvRound(a.x + direction.x * end), cvRound(a.y + direction.y * end)),
        color, thickness, cv::LINE_AA);
  }
}

void dashed_rectangle(cv::Mat& image, int x0, int y0, int x1, int y1,
                      const cv::Scalar& color) {
  dashed_line(image, {x0, y0}, {x1, y0}, color, 2);
  dashed_line(image, {x1, y0}, {x1, y1}, color, 2);
  dashed_line(image, {x1, y1}, {x0, y1}, color, 2);
  dashed_line(image, {x0, y1}, {x0, y0}, color, 2);
}

std::vector<std::uint8_t> encode(const AnalysisFrame& frame,
                                const PreviewConfig& config) {
  if (frame.bgr.empty() || frame.bgr.type() != CV_8UC3)
    throw std::invalid_argument("Preview requires an 8-bit BGR frame");
  const double scale = std::min(1.0,
      static_cast<double>(config.max_width) / frame.bgr.cols);
  const int width = std::max(1, static_cast<int>(std::lround(frame.bgr.cols * scale)));
  const int height = std::max(1, static_cast<int>(std::lround(frame.bgr.rows * scale)));
  const int source_height = frame.source_height > 0 ? frame.source_height : frame.bgr.rows;
  cv::Mat annotated;
  cv::resize(frame.bgr, annotated, cv::Size(width, height), 0, 0,
             scale < 1.0 ? cv::INTER_AREA : cv::INTER_LINEAR);
  if (frame.overlay_stale)
    cv::putText(annotated, "AI OVERLAY DELAYED", {12, 26},
        cv::FONT_HERSHEY_SIMPLEX, 0.62, cv::Scalar(0, 170, 255), 2, cv::LINE_AA);

  for (const auto& zone : frame.zones) {
    if (!zone.enabled || !valid_polygon(zone.polygon)) continue;
    const auto crowd = std::find_if(frame.signals.begin(), frame.signals.end(),
        [&](const BehaviorSignal& signal) {
          return signal.type == "crowd" && signal.zone_id == zone.id;
        });
    std::vector<cv::Point> polygon;
    polygon.reserve(zone.polygon.size());
    for (const auto& point : zone.polygon)
      polygon.emplace_back(static_cast<int>(std::lround(point.x * width)),
                           std::clamp(static_cast<int>(std::lround(
                               point.y * source_height * scale)), 0, height - 1));
    const cv::Scalar zone_color = crowd != frame.signals.end() && crowd->active
        ? cv::Scalar(40, 40, 235) : cv::Scalar(220, 180, 45);
    cv::polylines(annotated, polygon, true, zone_color, 2,
                  cv::LINE_AA);
    if (!polygon.empty())
      cv::putText(annotated, zone.name,
          cv::Point(std::clamp(polygon.front().x + 3, 0, width - 1),
                    std::clamp(polygon.front().y - 5, 14, height - 1)),
          cv::FONT_HERSHEY_SIMPLEX, 0.45, zone_color, 1,
          cv::LINE_AA);
    if (crowd != frame.signals.end() && !polygon.empty())
      cv::putText(annotated,
          "PEOPLE " + std::to_string(crowd->count) + "/" + std::to_string(crowd->threshold),
          cv::Point(std::clamp(polygon.front().x + 3, 0, width - 1),
                    std::clamp(polygon.front().y + 18, 14, height - 1)),
          cv::FONT_HERSHEY_SIMPLEX, 0.43, zone_color, 2, cv::LINE_AA);
  }

  for (const auto& track : frame.tracks) {
    if (track.confidence < 0.25f) continue;
    const int x0 = std::clamp(static_cast<int>(std::lround(track.box.x * scale)), 0, width - 1);
    const int y0 = std::clamp(static_cast<int>(std::lround(track.box.y * scale)), 0, height - 1);
    const int x1 = std::clamp(static_cast<int>(std::lround((track.box.x + track.box.w) * scale)), 0, width - 1);
    const int y1 = std::clamp(static_cast<int>(std::lround((track.box.y + track.box.h) * scale)), 0, height - 1);
    if (x1 <= x0 || y1 <= y0) continue;

    const auto active = std::find_if(frame.signals.begin(), frame.signals.end(),
        [&](const BehaviorSignal& signal) {
          return signal.track_id == track.id && signal.active;
        });
    const auto running = std::find_if(frame.signals.begin(), frame.signals.end(),
        [&](const BehaviorSignal& signal) {
          return signal.track_id == track.id && signal.type == "running" && signal.active;
        });
    const auto dwelling = std::find_if(frame.signals.begin(), frame.signals.end(),
        [&](const BehaviorSignal& signal) {
          return signal.track_id == track.id && signal.type == "loitering" &&
                 signal.inside && signal.observed;
        });
    const auto paused_dwell = std::find_if(frame.signals.begin(), frame.signals.end(),
        [&](const BehaviorSignal& signal) {
          return signal.track_id == track.id && signal.type == "loitering" &&
                 signal.inside && !signal.observed;
        });
    const cv::Scalar color = !track.observed ? cv::Scalar(170, 190, 210)
        : active != frame.signals.end()
        ? (active->type == "loitering" ? cv::Scalar(40, 40, 235)
                                       : (running != frame.signals.end()
                                              ? cv::Scalar(0, 80, 255)
                                              : cv::Scalar(20, 145, 255)))
        : (dwelling != frame.signals.end() ? cv::Scalar(0, 190, 255)
            : (track.class_id == 0 ? cv::Scalar(50, 220, 50)
                                   : cv::Scalar(255, 180, 50)));
    if (track.observed)
      cv::rectangle(annotated, cv::Point(x0, y0), cv::Point(x1, y1), color, 2);
    else
      dashed_rectangle(annotated, x0, y0, x1, y1, color);
    if (track.observed)
      cv::circle(annotated, cv::Point((x0 + x1) / 2, y1), 4,
                 cv::Scalar(0, 220, 255), cv::FILLED, cv::LINE_AA);
    std::string label = coco_class_name(track.class_id) + " #" + std::to_string(track.id) + " " +
        std::to_string(static_cast<int>(std::lround(track.confidence * 100.0f))) + "%";
    if (!track.observed)
      label += active != frame.signals.end() && active->type == "loitering"
          ? " TEMP / DWELL PAUSED"
          : (paused_dwell != frame.signals.end() ? " TEMP / DWELL PAUSED" : " TEMP");
    else if (active != frame.signals.end() && active->type == "loitering" &&
             dwelling != frame.signals.end())
      label += " LOITERING " +
          std::to_string(static_cast<int>(std::floor(dwelling->dwell_seconds))) +
          "/" + std::to_string(static_cast<int>(std::ceil(dwelling->target_seconds))) + "s";
    else if (dwelling != frame.signals.end())
      label += " DWELL " +
          std::to_string(static_cast<int>(std::floor(dwelling->dwell_seconds))) +
          "/" + std::to_string(static_cast<int>(std::ceil(dwelling->target_seconds))) + "s";
    else if (active != frame.signals.end())
      label += active->type == "loitering" ? " LOITERING"
          : (running != frame.signals.end() ? " RUNNING" : " FAST MOTION");
    const bool show_dwell = track.observed && dwelling != frame.signals.end();
    if (show_dwell) {
      int baseline = 0;
      const cv::Size label_size = cv::getTextSize(
          label, cv::FONT_HERSHEY_SIMPLEX, 0.48, 2, &baseline);
      const int label_width = label_size.width + 10;
      const int label_height = label_size.height + 8;
      const int label_left = std::clamp(x0, 0, std::max(0, width - label_width));
      int label_top = y0 - label_height - 3;
      if (label_top < 0)
        label_top = std::min(y1 + 3, std::max(0, height - label_height));
      cv::rectangle(annotated, {label_left, label_top},
          {label_left + label_width, label_top + label_height}, color, cv::FILLED);
      cv::putText(annotated, label,
          {label_left + 5, label_top + label_size.height + 4},
          cv::FONT_HERSHEY_SIMPLEX, 0.48, cv::Scalar(255, 255, 255),
          2, cv::LINE_AA);
    } else {
      cv::putText(annotated, label,
          cv::Point(x0, std::max(16, y0 - 5)), cv::FONT_HERSHEY_SIMPLEX,
          0.48, color, 2, cv::LINE_AA);
    }
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
  auto last_encode_started = std::chrono::steady_clock::time_point::min();
  for (;;) {
    std::shared_ptr<const AnalysisFrame> frame;
    PreviewConfig encode_config;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      changed_.wait(lock, [&] { return stopping_ || static_cast<bool>(pending_); });
      if (stopping_) return;
      const auto period = std::chrono::duration<double>(1.0 / config_.fps);
      auto due = last_encode_started == std::chrono::steady_clock::time_point::min()
          ? std::chrono::steady_clock::now()
          : last_encode_started + std::chrono::duration_cast<std::chrono::steady_clock::duration>(period);
      if (std::chrono::steady_clock::now() < due) {
        changed_.wait_until(lock, due, [&] { return stopping_; });
        if (stopping_) return;
      }
      frame = std::move(pending_);  // Newest pending frame wins.
      pending_.reset();
      encode_config = config_;
      last_encode_started = std::chrono::steady_clock::now();
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
      changed_.notify_all();
    } catch (const std::exception& error) {
      std::lock_guard<std::mutex> lock(mutex_);
      error_ = error.what();
      changed_.notify_all();
    }
  }
}

}  // namespace aibox

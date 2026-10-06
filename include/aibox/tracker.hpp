#pragma once

#include "aibox/yolo.hpp"
#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

namespace aibox {

struct Track {
  int id{};
  int class_id{};
  Box box;
  float confidence{};
  bool observed{true};
  std::chrono::steady_clock::time_point first_seen;
  std::chrono::steady_clock::time_point last_observed;
};

struct ByteTrackConfig {
  float low_confidence{0.10f};
  float high_confidence{0.50f};
  float new_track_confidence{0.60f};
  float first_match_iou{0.20f};
  float second_match_iou{0.10f};
  float unconfirmed_match_iou{0.30f};
  double track_ttl_seconds{2.0};
  double max_predict_seconds{1.0};
  double process_noise{2.0};
  double measurement_noise{1.0};
  int min_hits{2};
};

// CPU ByteTrack-style tracker: timestamp-aware constant-velocity Kalman filter,
// two-stage high/low confidence association, and Hungarian assignment.
class ByteTracker {
 public:
  explicit ByteTracker(ByteTrackConfig config = {});
  ~ByteTracker();
  ByteTracker(ByteTracker&&) noexcept;
  ByteTracker& operator=(ByteTracker&&) noexcept;
  ByteTracker(const ByteTracker&) = delete;
  ByteTracker& operator=(const ByteTracker&) = delete;
  std::vector<Track> update(const std::vector<Detection>& detections,
                           std::chrono::steady_clock::time_point at);
  void reset();

 private:
  struct Impl;
  ByteTrackConfig config_;
  std::unique_ptr<Impl> impl_;
};

}  // namespace aibox

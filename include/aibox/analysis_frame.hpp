#pragma once

#include "aibox/tracker.hpp"
#include "aibox/types.hpp"
#include <opencv2/core.hpp>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace aibox {

struct AnalysisFrame {
  cv::Mat bgr;
  std::vector<Detection> people;
  std::vector<Track> tracks;
  std::vector<Zone> zones;
  std::vector<BehaviorSignal> signals;
  std::uint64_t frame_id{};
  std::string session_id;
  std::chrono::steady_clock::time_point received;
  double inference_ms{};
};

}  // namespace aibox

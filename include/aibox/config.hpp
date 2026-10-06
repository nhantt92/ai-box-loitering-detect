#pragma once

#include "aibox/engines.hpp"
#include <cstdint>
#include <mutex>
#include <string>

namespace aibox {

struct PreviewConfig {
  int fps{5};
  int max_width{640};
  int quality{70};
  int max_clients{3};
};

struct StorageConfig {
  int retention_days{30};
  std::int64_t max_snapshot_bytes{1073741824};
};

struct ModelConfig {
  std::string profile{"yolov5-a733-640"};
  float score_threshold{0.01f};
  float nms_iou{0.45f};
};

struct TrackingSettings {
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

struct RuntimeConfig {
  int revision{};
  std::string camera_url{"rtsp://192.168.1.6:554/profile2"};
  std::string decoder{"omxh264dec"};
  ModelConfig model;
  TrackingSettings tracking;
  std::vector<Zone> zones;
  LoiterConfig loitering;
  MotionConfig motion;
  PreviewConfig preview;
  StorageConfig storage;
};

RuntimeConfig parse_runtime_config(const std::string& json_text);
RuntimeConfig load_runtime_config(const std::string& path);
std::string serialize_runtime_config(const RuntimeConfig& config);

struct RuntimeConfigUpdateResult {
  int status{200};
  RuntimeConfig config;
  std::string error;
};

class RuntimeConfigController {
 public:
  RuntimeConfigController(RuntimeConfig initial, std::string path);
  RuntimeConfig current() const;
  RuntimeConfigUpdateResult update_camera_url(const std::string& url,
                                               int expected_revision);
  RuntimeConfigUpdateResult update_behavior_config(const std::string& json_text,
                                                    int expected_revision);

 private:
  mutable std::mutex mutex_;
  RuntimeConfig config_;
  std::string path_;
};

}  // namespace aibox

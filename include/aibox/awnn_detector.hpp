#pragma once
#include "aibox/yolo.hpp"
#include <memory>
#include <string>
namespace aibox {
class AwnnDetector {
 public:
  explicit AwnnDetector(const std::string& model_path);
  ~AwnnDetector();
  AwnnDetector(const AwnnDetector&)=delete;
  AwnnDetector& operator=(const AwnnDetector&)=delete;
  std::vector<Detection> detect(const cv::Mat& bgr, float score_threshold=0.01f,
                                float nms_iou=0.45f);
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}

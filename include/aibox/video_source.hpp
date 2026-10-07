#pragma once
#include <opencv2/core.hpp>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
namespace aibox {
struct VideoFrame { cv::Mat bgr; std::uint64_t frame_id{}; std::string session_id; std::chrono::steady_clock::time_point received; std::optional<std::uint64_t> pts_ns; };
class GstVideoSource {
 public:
  explicit GstVideoSource(std::string source_type, std::string uri,
      std::string decoder = "omxh264dec", std::string username = {},
      std::string password = {});
  ~GstVideoSource();
  GstVideoSource(const GstVideoSource&)=delete;
  GstVideoSource& operator=(const GstVideoSource&)=delete;
  void start();
  std::optional<VideoFrame> pull(std::chrono::milliseconds timeout);
  void stop();
  std::string last_error() const;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}

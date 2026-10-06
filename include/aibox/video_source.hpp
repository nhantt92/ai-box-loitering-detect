#pragma once
#include <opencv2/core.hpp>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
namespace aibox {
struct VideoFrame { cv::Mat bgr; std::uint64_t frame_id{}; std::string session_id; std::chrono::steady_clock::time_point received; std::optional<std::uint64_t> pts_ns; };
class GstRtspSource {
 public:
  explicit GstRtspSource(std::string uri, std::string decoder = "omxh264dec");
  ~GstRtspSource();
  GstRtspSource(const GstRtspSource&)=delete;
  GstRtspSource& operator=(const GstRtspSource&)=delete;
  void start();
  std::optional<VideoFrame> pull(std::chrono::milliseconds timeout);
  void stop();
  std::string last_error() const;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}

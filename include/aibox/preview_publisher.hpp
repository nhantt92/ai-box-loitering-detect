#pragma once

#include "aibox/analysis_frame.hpp"
#include "aibox/config.hpp"
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace aibox {

struct JpegFrame {
  std::uint64_t sequence{};
  std::chrono::steady_clock::time_point encoded_at;
  std::shared_ptr<const std::vector<std::uint8_t>> bytes;
};

class PreviewPublisher {
 public:
  explicit PreviewPublisher(PreviewConfig config);
  ~PreviewPublisher();
  PreviewPublisher(const PreviewPublisher&) = delete;
  PreviewPublisher& operator=(const PreviewPublisher&) = delete;

  // Constant-time handoff: a slow encoder replaces its pending frame instead
  // of building a video backlog.
  void submit(std::shared_ptr<const AnalysisFrame> frame);
  void configure(PreviewConfig config);
  std::shared_ptr<const JpegFrame> latest() const;
  std::shared_ptr<const JpegFrame> wait_after(
      std::uint64_t sequence, std::chrono::milliseconds timeout) const;
  bool healthy() const;
  std::string last_error() const;
  int max_clients() const;

 private:
  void run();
  PreviewConfig config_;
  mutable std::mutex mutex_;
  mutable std::condition_variable changed_;
  std::shared_ptr<const AnalysisFrame> pending_;
  std::shared_ptr<const JpegFrame> latest_;
  std::thread worker_;
  bool stopping_{};
  std::uint64_t sequence_{};
  std::string error_;
};

}  // namespace aibox

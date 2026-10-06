#pragma once

#include "aibox/config.hpp"
#include "aibox/event_store.hpp"
#include <opencv2/core.hpp>

#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

namespace aibox {

class SnapshotWriter {
 public:
  SnapshotWriter(EventStore& events, std::filesystem::path data_directory,
                 StorageConfig config, std::size_t queue_capacity = 8);
  ~SnapshotWriter();
  SnapshotWriter(const SnapshotWriter&) = delete;
  SnapshotWriter& operator=(const SnapshotWriter&) = delete;

  // Queues a shallow cv::Mat reference. Analysis frames are immutable after
  // publication, so enqueueing does not copy pixels or block inference.
  bool enqueue(const std::string& event_id, const cv::Mat& bgr);
  void configure(StorageConfig config);
  bool healthy() const;
  std::string last_error() const;
  std::filesystem::path snapshot_directory() const;

 private:
  struct Job { std::string event_id; cv::Mat bgr; };
  void run();
  void write_snapshot(const Job& job);
  void enforce_retention();
  void reconcile();
  void set_error(const std::string& error);
  StorageConfig current_config() const;

  EventStore& events_;
  std::filesystem::path data_directory_;
  std::filesystem::path snapshot_directory_;
  StorageConfig config_;
  std::size_t queue_capacity_;
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::deque<Job> jobs_;
  std::thread worker_;
  bool stopping_{};
  bool maintenance_requested_{};
  std::string error_;
};

}  // namespace aibox

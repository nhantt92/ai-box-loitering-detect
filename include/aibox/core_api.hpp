#pragma once

#include "aibox/config.hpp"
#include "aibox/event_store.hpp"
#include "aibox/preview_publisher.hpp"
#include "aibox/types.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace aibox {

struct CoreStatus {
  bool camera_connected{};
  bool overlay_stale{};
  double ai_fps{};
  double inference_ms{};
  std::uint64_t frame_id{};
  std::string session_id;
  std::string message;
  std::string storage_state{"ok"};
  std::size_t person_detections{};
  std::size_t person_tracks{};
  std::size_t enabled_zones{};
  std::vector<BehaviorSignal> loitering;
  std::vector<BehaviorSignal> crowd;
};

class CoreApiServer {
 public:
  CoreApiServer(PreviewPublisher& preview,
                std::shared_ptr<RuntimeConfigController> config,
                std::shared_ptr<EventStore> events,
                std::function<CoreStatus()> status_provider,
                std::string data_directory, std::uint16_t port = 8091);
  ~CoreApiServer();
  CoreApiServer(const CoreApiServer&) = delete;
  CoreApiServer& operator=(const CoreApiServer&) = delete;

  bool available() const;
  std::string last_error() const;
  std::string token_file() const;
  std::uint16_t port() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace aibox

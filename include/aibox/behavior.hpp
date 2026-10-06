#pragma once

#include "aibox/config.hpp"
#include "aibox/tracker.hpp"
#include <memory>
#include <string>
#include <vector>

namespace aibox {

struct BehaviorUpdate {
  std::vector<BehaviorSignal> signals;
  std::vector<EventTransition> events;
};

class BehaviorCoordinator {
 public:
  explicit BehaviorCoordinator(RuntimeConfig config);
  ~BehaviorCoordinator();
  BehaviorCoordinator(BehaviorCoordinator&&) noexcept;
  BehaviorCoordinator& operator=(BehaviorCoordinator&&) noexcept;
  BehaviorCoordinator(const BehaviorCoordinator&) = delete;
  BehaviorCoordinator& operator=(const BehaviorCoordinator&) = delete;

  BehaviorUpdate update(const std::vector<Track>& tracks, Mono at,
                        const std::string& session_id, int frame_width,
                        int frame_height);
  std::vector<EventTransition> reset(const std::string& reason, Mono at);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace aibox

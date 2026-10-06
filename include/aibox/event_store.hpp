#pragma once

#include "aibox/types.hpp"
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace aibox {

struct StoredEvent {
  std::string id, session_id, type, zone_id, started_utc, ended_utc;
  std::string speed_unit, end_reason, snapshot_state, snapshot_path;
  int track_id{};
  double duration_seconds{}, dwell_seconds{}, peak_speed{};
  bool active{};
};

class EventStore {
 public:
  explicit EventStore(std::string database_path, std::size_t queue_capacity = 128);
  ~EventStore();
  EventStore(const EventStore&) = delete;
  EventStore& operator=(const EventStore&) = delete;

  // Waits briefly for bounded queue capacity. False means the caller must
  // report storage degradation; the transition was not accepted.
  bool enqueue(EventTransition transition);
  bool enqueue_snapshot_state(std::string event_id, std::string state,
                              std::string relative_path = {});
  bool enqueue_delete_event(std::string event_id);
  void flush();
  std::vector<StoredEvent> recent(std::size_t offset, std::size_t limit) const;
  std::optional<StoredEvent> get(const std::string& event_id) const;
  std::vector<StoredEvent> retention_candidates(std::size_t offset,
                                                std::size_t limit) const;
  bool healthy() const;
  std::string last_error() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace aibox

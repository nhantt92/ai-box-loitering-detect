#include "aibox/behavior.hpp"

#include <algorithm>
#include <atomic>
#include <map>
#include <stdexcept>
#include <utility>

namespace aibox {
namespace {
using TrackZone = std::pair<int, std::string>;

Event make_event(const std::string& type, int track_id,
                 const std::string& zone_id, const std::string& session_id,
                 Mono at, std::uint64_t sequence) {
  Event event;
  static std::atomic<std::uint64_t> process_event_sequence{0};
  event.id = session_id + "-" + std::to_string(sequence) + "-" +
      std::to_string(process_event_sequence.fetch_add(1, std::memory_order_relaxed));
  event.type = type;
  event.track_id = track_id;
  event.zone_id = zone_id;
  event.session_id = session_id;
  event.started = at;
  event.ended = at;
  event.started_utc = std::chrono::system_clock::now();
  event.ended_utc = event.started_utc;
  event.active = true;
  return event;
}

}  // namespace

struct BehaviorCoordinator::Impl {
  struct LoiterSlot {
    explicit LoiterSlot(const LoiterConfig& config) : engine(config) {}
    LoiteringEngine engine;
    Box last_box;
    std::uint64_t touched{};
    Event event;
    bool event_active{};
    LoiterState state{LoiterState::Outside};
    double dwell{};
    bool observed{};
  };
  struct MotionSlot {
    explicit MotionSlot(const MotionConfig& config) : engine(config) {}
    MotionEngine engine;
    Box last_box;
    std::uint64_t touched{};
    Event event;
    bool event_active{};
    double speed{};
    bool observed{};
  };
  struct CrowdSlot {
    explicit CrowdSlot(const CrowdConfig& config) : engine(config) {}
    CrowdEngine engine;
    Event event;
    bool event_active{};
    int count{};
  };

  explicit Impl(RuntimeConfig value) : config(std::move(value)) {
    for (const auto& zone : config.zones)
      if (zone.enabled && !valid_polygon(zone.polygon))
        throw std::invalid_argument("Behavior coordinator received an invalid zone");
  }

  RuntimeConfig config;
  std::map<TrackZone, LoiterSlot> loiter;
  std::map<int, MotionSlot> motion;
  std::map<int, MotionSlot> running;
  std::map<std::string, CrowdSlot> crowd;
  std::uint64_t frame_sequence{};
  std::uint64_t event_sequence{};
  std::string session_id;
  int frame_width{};
  int frame_height{};
};

BehaviorCoordinator::BehaviorCoordinator(RuntimeConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}
BehaviorCoordinator::~BehaviorCoordinator() = default;
BehaviorCoordinator::BehaviorCoordinator(BehaviorCoordinator&&) noexcept = default;
BehaviorCoordinator& BehaviorCoordinator::operator=(BehaviorCoordinator&&) noexcept = default;

BehaviorUpdate BehaviorCoordinator::update(const std::vector<Track>& tracks,
                                            Mono at,
                                            const std::string& session_id,
                                            int frame_width, int frame_height) {
  auto& p = *impl_;
  if (frame_width <= 0 || frame_height <= 0)
    throw std::invalid_argument("BehaviorCoordinator requires positive frame dimensions");
  BehaviorUpdate output;
  if ((p.frame_width && (p.frame_width != frame_width || p.frame_height != frame_height)) ||
      (!p.session_id.empty() && p.session_id != session_id)) {
    const std::string reason = p.session_id != session_id
        ? "camera_disconnected" : "config_changed";
    output.events = reset(reason, at);
  }
  p.frame_width = frame_width;
  p.frame_height = frame_height;
  if (p.session_id.empty()) p.session_id = session_id;
  ++p.frame_sequence;
  std::map<int, const Track*> current;
  for (const auto& track : tracks)
    if (track.class_id == 0) current[track.id] = &track;

  auto finish_loiter = [&](typename Impl::LoiterSlot& slot,
                           const LoiterUpdate& update) {
    slot.state = update.state;
    slot.dwell = update.dwell;
    if (update.alarm_began && !slot.event_active) {
      slot.event = make_event("loitering", slot.event.track_id,
          slot.event.zone_id, p.session_id, at, ++p.event_sequence);
      slot.event.dwell_seconds = update.dwell;
      slot.event_active = true;
      output.events.push_back({slot.event, true});
    }
    if (slot.event_active) {
      if (!update.ended || update.dwell > 0.0)
        slot.event.dwell_seconds = update.dwell;
      if (update.ended) {
        slot.event.active = false;
        slot.event.reason = update.reason;
        slot.event.ended = at;
        slot.event.ended_utc = std::chrono::system_clock::now();
        output.events.push_back({slot.event, false});
        slot.event_active = false;
      }
    }
  };

  for (const auto& zone : p.config.zones) {
    for (const auto& item : current) {
      const auto& track = *item.second;
      const TrackZone key{track.id, zone.id};
      auto [it, inserted] = p.loiter.try_emplace(key, p.config.loitering);
      auto& slot = it->second;
      if (inserted) {
        slot.event.track_id = track.id;
        slot.event.zone_id = zone.id;
      }
      slot.touched = p.frame_sequence;
      slot.last_box = track.box;
      slot.observed = track.observed;
      const Box normalized{track.box.x / frame_width, track.box.y / frame_height,
          track.box.w / frame_width, track.box.h / frame_height};
      Observation observation{track.id, normalized, at, track.observed};
      const auto update = slot.engine.update(zone, observation);
      finish_loiter(slot, update);
    }
    for (auto it = p.loiter.begin(); it != p.loiter.end();) {
      if (it->first.second != zone.id || it->second.touched == p.frame_sequence) {
        ++it;
        continue;
      }
      auto& slot = it->second;
      slot.observed = false;
      const Box normalized{slot.last_box.x / frame_width,
          slot.last_box.y / frame_height, slot.last_box.w / frame_width,
          slot.last_box.h / frame_height};
      const auto update = slot.engine.update(
          zone, Observation{it->first.first, normalized, at, false});
      finish_loiter(slot, update);
      if (update.ended && update.reason == "track_lost") it = p.loiter.erase(it);
      else ++it;
    }
  }

  const auto update_motion_rule = [&](std::map<int, Impl::MotionSlot>& slots,
                                      const MotionConfig& config,
                                      const char* event_type) {
    for (const auto& item : current) {
      const auto& track = *item.second;
      auto [it, inserted] = slots.try_emplace(track.id, config);
      auto& slot = it->second;
      if (inserted) slot.event.track_id = track.id;
      slot.touched = p.frame_sequence;
      slot.last_box = track.box;
      slot.observed = track.observed;
      const MotionUpdate update = slot.engine.update(
          Observation{track.id, track.box, at, track.observed});
      slot.speed = update.speed;
      if (update.began && !slot.event_active) {
        slot.event = make_event(event_type, track.id, {}, p.session_id,
                                at, ++p.event_sequence);
        slot.event.speed_unit = update.unit;
        slot.event.peak_speed = update.speed;
        slot.event_active = true;
        output.events.push_back({slot.event, true});
      }
      if (slot.event_active) {
        slot.event.peak_speed = std::max(slot.event.peak_speed, update.speed);
        if (update.ended) {
          slot.event.active = false;
          slot.event.reason = update.reason.empty() ? "speed_normalized" : update.reason;
          slot.event.ended = at;
          slot.event.ended_utc = std::chrono::system_clock::now();
          output.events.push_back({slot.event, false});
          slot.event_active = false;
        }
      }
    }
    for (auto it = slots.begin(); it != slots.end();) {
      if (it->second.touched == p.frame_sequence) { ++it; continue; }
      auto& slot = it->second;
      slot.observed = false;
      const MotionUpdate update = slot.engine.update(
          Observation{it->first, slot.last_box, at, false});
      slot.speed = 0.0;
      if (slot.event_active && update.ended) {
        slot.event.active = false;
        slot.event.reason = update.reason;
        slot.event.ended = at;
        slot.event.ended_utc = std::chrono::system_clock::now();
        output.events.push_back({slot.event, false});
        slot.event_active = false;
      }
      if (update.ended && update.reason == "track_lost") it = slots.erase(it);
      else ++it;
    }
  };
  update_motion_rule(p.motion, p.config.motion, "fast_motion");
  update_motion_rule(p.running, p.config.running_detection, "running");

  if (p.config.crowd.enabled) {
    for (const auto& zone : p.config.zones) {
      if (!zone.enabled || !valid_polygon(zone.polygon)) continue;
      int people = 0;
      for (const auto& item : current) {
        const auto& track = *item.second;
        if (!track.observed) continue;
        const Point foot{(track.box.x + track.box.w * 0.5) / frame_width,
                         (track.box.y + track.box.h) / frame_height};
        if (inside_polygon(zone.polygon, foot)) ++people;
      }
      auto [it, inserted] = p.crowd.try_emplace(zone.id, p.config.crowd);
      auto& slot = it->second;
      if (inserted) {
        slot.event.zone_id = zone.id;
      }
      slot.count = people;
      const auto update = slot.engine.update(people, at);
      if (update.began && !slot.event_active) {
        slot.event = make_event("crowd", 0, zone.id, p.session_id, at,
                                ++p.event_sequence);
        slot.event.crowd_count = people;
        slot.event.crowd_threshold = p.config.crowd.trigger_people;
        slot.event_active = true;
        output.events.push_back({slot.event, true});
      }
      if (slot.event_active) {
        slot.event.crowd_count = std::max(slot.event.crowd_count, people);
        if (update.ended) {
          slot.event.active = false;
          slot.event.reason = update.reason;
          slot.event.ended = at;
          slot.event.ended_utc = std::chrono::system_clock::now();
          output.events.push_back({slot.event, false});
          slot.event_active = false;
        }
      }
      output.signals.push_back({0, "crowd", zone.id, zone.name,
          slot.event_active, true, 0.0, 0.0, "people", false, 0.0,
          people, p.config.crowd.trigger_people});
    }
  }

  for (const auto& item : p.loiter) {
    const auto& zone = *std::find_if(p.config.zones.begin(), p.config.zones.end(),
        [&](const Zone& candidate) { return candidate.id == item.first.second; });
    output.signals.push_back({item.first.first, "loitering", zone.id, zone.name,
        item.second.event_active, item.second.observed,
        item.second.dwell, 0.0, "seconds",
        item.second.state == LoiterState::Entering ||
            item.second.state == LoiterState::Inside ||
            item.second.state == LoiterState::Alarm,
        zone.dwell_seconds});
  }
  for (const auto& item : p.motion) {
    output.signals.push_back({item.first, "fast_motion", {}, {},
        item.second.event_active, item.second.observed, 0.0,
        item.second.speed, "body_heights_per_second"});
  }
  for (const auto& item : p.running) {
    output.signals.push_back({item.first, "running", {}, {},
        item.second.event_active, item.second.observed, 0.0,
        item.second.speed, "body_heights_per_second"});
  }
  return output;
}

std::vector<EventTransition> BehaviorCoordinator::reset(const std::string& reason,
                                                        Mono at) {
  auto& p = *impl_;
  std::vector<EventTransition> transitions;
  for (auto& item : p.loiter) {
    const auto result = item.second.engine.reset(reason);
    if (item.second.event_active) {
      item.second.event.active = false;
      item.second.event.reason = result.reason.empty() ? reason : result.reason;
      item.second.event.ended = at;
      item.second.event.ended_utc = std::chrono::system_clock::now();
      transitions.push_back({item.second.event, false});
      item.second.event_active = false;
    }
  }
  for (auto& item : p.motion) {
    const auto result = item.second.engine.reset(reason);
    if (item.second.event_active && result.ended) {
      item.second.event.active = false;
      item.second.event.reason = result.reason.empty() ? reason : result.reason;
      item.second.event.ended = at;
      item.second.event.ended_utc = std::chrono::system_clock::now();
      transitions.push_back({item.second.event, false});
      item.second.event_active = false;
    }
  }
  for (auto& item : p.running) {
    const auto result = item.second.engine.reset(reason);
    if (item.second.event_active && result.ended) {
      item.second.event.active = false;
      item.second.event.reason = result.reason.empty() ? reason : result.reason;
      item.second.event.ended = at;
      item.second.event.ended_utc = std::chrono::system_clock::now();
      transitions.push_back({item.second.event, false});
      item.second.event_active = false;
    }
  }
  for (auto& item : p.crowd) {
    const auto result = item.second.engine.reset(reason);
    if (item.second.event_active && result.ended) {
      item.second.event.active = false;
      item.second.event.reason = result.reason.empty() ? reason : result.reason;
      item.second.event.ended = at;
      item.second.event.ended_utc = std::chrono::system_clock::now();
      transitions.push_back({item.second.event, false});
      item.second.event_active = false;
    }
  }
  p.loiter.clear();
  p.motion.clear();
  p.running.clear();
  p.crowd.clear();
  p.session_id.clear();
  p.frame_width = 0;
  p.frame_height = 0;
  return transitions;
}

}  // namespace aibox

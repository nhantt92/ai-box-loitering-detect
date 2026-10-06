#include "aibox/behavior.hpp"

#include <cassert>
#include <chrono>
#include <stdexcept>

using namespace aibox;
using namespace std::chrono_literals;

namespace {
const char* kConfig = R"json({
  "revision": 3,
  "camera": {"url":"rtsp://192.168.1.6:554/profile2", "decoder":"omxh264dec"},
  "zones": [{"id":"door", "name":"Door", "enabled":true,
    "polygon":[[0.1,0.1],[0.9,0.1],[0.9,0.9],[0.1,0.9]], "dwell_seconds":1}],
  "loitering":{"enter_confirm_seconds":0.5,"exit_confirm_seconds":0.4,
    "lost_grace_seconds":0.6,"max_observation_gap_seconds":1.0},
  "running":{"enabled":false,"mode":"body_heights_per_second",
    "start_threshold":2.5,"end_threshold":1.8,"window_seconds":1.0,
    "min_observations":5},
  "preview":{"fps":5,"max_width":640,"quality":70,"max_clients":3},
  "storage":{"retention_days":30,"max_snapshot_bytes":1073741824}
})json";

Track person_track(int id, double x, Mono at, bool observed = true) {
  return {id, 0, {x, 200, 50, 100}, .9f, observed, at - 1s,
          observed ? at : at - 100ms};
}

void config_parser_validates_operator_input() {
  const auto config = parse_runtime_config(kConfig);
  assert(config.revision == 3 && config.zones.size() == 1);
  assert(config.zones.front().id == "door");
  assert(config.loitering.enter_confirm == .5);
  assert(!config.motion.enabled);

  auto invalid = std::string(kConfig);
  const auto pos = invalid.find("[0.9,0.9]");
  invalid.replace(pos, 9, "[0.1,0.9]");
  bool rejected = false;
  try { parse_runtime_config(invalid); }
  catch (const std::invalid_argument&) { rejected = true; }
  assert(rejected);

  rejected = false;
  try { parse_runtime_config(R"({"camera":{"url":"rtsp://user:secret@camera/live"}})"); }
  catch (const std::invalid_argument&) { rejected = true; }
  assert(rejected);
  rejected = false;
  try { parse_runtime_config(R"({"camera":{"url":"rtsp://:554/profile2"}})"); }
  catch (const std::invalid_argument&) { rejected = true; }
  assert(rejected);
  rejected = false;
  try { parse_runtime_config(R"({"surprise":true})"); }
  catch (const std::invalid_argument&) { rejected = true; }
  assert(rejected);
}

void loiter_alarm_is_emitted_once_and_closed_on_lost() {
  auto config = parse_runtime_config(kConfig);
  BehaviorCoordinator behavior(config);
  const Mono t{};
  auto update = behavior.update({person_track(4, 200, t)}, t, "session-a", 1000, 1000);
  assert(update.events.empty());
  update = behavior.update({person_track(4, 200, t + 500ms)}, t + 500ms, "session-a", 1000, 1000);
  assert(update.events.empty());  // Entry confirmed, dwell has not reached alarm yet.
  update = behavior.update({person_track(4, 200, t + 1s)}, t + 1s, "session-a", 1000, 1000);
  assert(update.events.size() == 1 && update.events.front().started);
  assert(update.events.front().event.type == "loitering");
  const auto event_id = update.events.front().event.id;
  update = behavior.update({person_track(4, 200, t + 1100ms, false)},
                           t + 1100ms, "session-a", 1000, 1000);
  assert(update.events.empty());
  update = behavior.update({person_track(4, 200, t + 1700ms, false)},
                           t + 1700ms, "session-a", 1000, 1000);
  assert(update.events.size() == 1 && !update.events.front().started);
  assert(update.events.front().event.id == event_id);
  assert(update.events.front().event.reason == "track_lost");
  assert(update.events.front().event.dwell_seconds >= 1.0);
  assert(!update.events.front().event.active);
}

void zone_exit_and_session_reset_close_episodes() {
  auto config = parse_runtime_config(kConfig);
  config.loitering.exit_confirm = .2;
  config.zones.front().dwell_seconds = .75;
  BehaviorCoordinator behavior(config);
  const Mono t{};
  behavior.update({person_track(1, 200, t)}, t, "session-b", 1000, 1000);
  behavior.update({person_track(1, 200, t + 500ms)}, t + 500ms, "session-b", 1000, 1000);
  auto update = behavior.update({person_track(1, 200, t + 1s)}, t + 1s, "session-b", 1000, 1000);
  assert(update.events.size() == 1 && update.events.front().started);
  update = behavior.update({person_track(1, 950, t + 1100ms)}, t + 1100ms, "session-b", 1000, 1000);
  assert(update.events.empty());
  update = behavior.update({person_track(1, 950, t + 1400ms)}, t + 1400ms, "session-b", 1000, 1000);
  assert(update.events.size() == 1 && !update.events.front().started);
  assert(update.events.front().event.reason == "zone_exit");

  // A later episode is closed with a precise reconnect reason.
  behavior.update({person_track(1, 200, t + 1500ms)}, t + 1500ms, "session-b", 1000, 1000);
  behavior.update({person_track(1, 200, t + 2s)}, t + 2s, "session-b", 1000, 1000);
  update = behavior.update({person_track(1, 200, t + 2500ms)}, t + 2500ms, "session-b", 1000, 1000);
  assert(update.events.size() == 1 && update.events.front().started);
  const auto closed = behavior.reset("camera_disconnected", t + 2600ms);
  assert(closed.size() == 1 && !closed.front().started);
  assert(closed.front().event.reason == "camera_disconnected");
}

void fast_motion_uses_configured_units_and_hysteresis() {
  std::string text = kConfig;
  const auto begin = text.find("\"running\":");
  const auto end = text.find(",\n  \"preview\"", begin);
  text.replace(begin, end - begin,
      "\"running\":{\"enabled\":true,\"mode\":\"body_heights_per_second\","
      "\"start_threshold\":1.2,\"end_threshold\":0.7,\"window_seconds\":0.5,"
      "\"min_observations\":5,\"min_span_seconds\":0.3,"
      "\"start_confirm_seconds\":0.2,\"end_confirm_seconds\":0.2,"
      "\"lost_grace_seconds\":0.4,\"max_gap_seconds\":0.3}");
  auto config = parse_runtime_config(text);
  assert(config.motion.enabled && config.motion.start == 1.2);
  BehaviorCoordinator behavior(config);
  const Mono t{};
  int starts = 0, ends = 0;
  for (int i = 0; i <= 10; ++i) {
    const auto at = t + i * 100ms;
    auto update = behavior.update({person_track(8, 100 + 25.0 * i, at)}, at,
                                  "motion-session", 1000, 1000);
    for (const auto& event : update.events) if (event.started) ++starts;
  }
  assert(starts == 1);
  for (int i = 11; i <= 28; ++i) {
    const auto at = t + i * 100ms;
    auto update = behavior.update({person_track(8, 350, at)}, at,
                                  "motion-session", 1000, 1000);
    for (const auto& event : update.events) if (!event.started) ++ends;
  }
  assert(ends == 1);
}
}  // namespace

int main() {
  config_parser_validates_operator_input();
  loiter_alarm_is_emitted_once_and_closed_on_lost();
  zone_exit_and_session_reset_close_episodes();
  fast_motion_uses_configured_units_and_hysteresis();
}

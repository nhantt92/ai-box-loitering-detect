#include "aibox/engines.hpp"

#include <cassert>
#include <chrono>
#include <cmath>
#include <stdexcept>

using namespace aibox;
using namespace std::chrono_literals;

namespace {
const std::vector<Point> kSquare{{0.1, 0.1}, {0.9, 0.1}, {0.9, 0.9},
                                 {0.1, 0.9}};

Observation inside_at(Mono at) {
  return {7, {0.45, 0.3, 0.1, 0.2}, at, true};
}

Observation outside_at(Mono at) {
  return {7, {1.1, 0.3, 0.1, 0.2}, at, true};
}

void polygon_geometry() {
  assert(valid_polygon(kSquare));
  assert(inside_polygon(kSquare, {0.5, 0.5}));
  assert(inside_polygon(kSquare, {0.1, 0.5}));  // Boundary counts as inside.
  assert(!inside_polygon(kSquare, {NAN, 0.5}));
  assert(!valid_polygon({{0.1, 0.1}, {0.9, 0.9}, {0.1, 0.9}, {0.9, 0.1}}));
  assert(!valid_polygon({{0.1, 0.1}, {0.9, 0.1}, {0.5, 0.1}, {0.1, 0.9}}));
  assert(!valid_polygon({{0.1, 0.1}, {0.1, 0.1}, {0.9, 0.9}}));
  assert(!valid_polygon({{0.1, 0.1}, {0.5, 0.5}, {0.9, 0.9}}));
}

void loiter_dwell_exit_and_new_episode() {
  LoiteringEngine engine({1.0, 1.0, 2.0, 1.0});
  Zone zone{"loading", "Loading", kSquare, 2.0, true};
  const Mono t{};
  auto update = engine.update(zone, inside_at(t));
  assert(update.state == LoiterState::Entering && !update.began);
  update = engine.update(zone, inside_at(t + 500ms));
  assert(update.state == LoiterState::Entering && std::abs(update.dwell - 0.5) < 1e-9);
  update = engine.update(zone, inside_at(t + 1s));
  assert(update.began && update.state == LoiterState::Inside);
  assert(std::abs(update.dwell - 1.0) < 1e-9);
  update = engine.update(zone, inside_at(t + 2s));
  assert(update.state == LoiterState::Alarm && std::abs(update.dwell - 2.0) < 1e-9);
  update = engine.update(zone, outside_at(t + 2100ms));
  assert(update.state == LoiterState::Exiting && !update.ended);
  update = engine.update(zone, outside_at(t + 3100ms));
  assert(update.ended && update.reason == "zone_exit");
  assert(update.state == LoiterState::Ended);

  update = engine.update(zone, inside_at(t + 3200ms));
  assert(update.state == LoiterState::Entering);
  update = engine.update(zone, inside_at(t + 4200ms));
  assert(update.began);  // Ended is a one-update transition; a new episode works.
}

void loiter_pauses_on_occlusion_and_ends_after_loss() {
  LoiteringEngine engine({0.5, 0.5, 1.0, 0.5});
  Zone zone{"door", "Door", kSquare, 1.0, true};
  const Mono t{};
  auto update = engine.update(zone, inside_at(t));
  update = engine.update(zone, inside_at(t + 500ms));
  assert(update.began && std::abs(update.dwell - 0.5) < 1e-9);

  Observation missing = inside_at(t + 700ms);
  missing.observed = false;
  update = engine.update(zone, missing);
  assert(update.state == LoiterState::Inside && std::abs(update.dwell - 0.5) < 1e-9);
  update = engine.update(zone, inside_at(t + 1s));
  assert(std::abs(update.dwell - 0.5) < 1e-9);  // No dwell credited through occlusion.
  update = engine.update(zone, inside_at(t + 1500ms));
  assert(update.state == LoiterState::Alarm && std::abs(update.dwell - 1.0) < 1e-9);

  missing.at = t + 2600ms;
  update = engine.update(zone, missing);
  assert(update.ended && update.reason == "track_lost");

  update = engine.update(zone, inside_at(t + 2700ms));
  assert(update.state == LoiterState::Entering);
  zone.enabled = false;
  update = engine.update(zone, inside_at(t + 2800ms));
  assert(!update.ended);  // Entry was never confirmed.

  zone.enabled = true;
  engine.update(zone, inside_at(t + 3s));
  update = engine.update(zone, inside_at(t + 3500ms));
  assert(update.began);
  zone.enabled = false;
  update = engine.update(zone, inside_at(t + 3600ms));
  assert(update.ended && update.reason == "config_changed");
}

void loiter_confirmation_timers_pause_during_occlusion() {
  LoiteringEngine entry({1.0, 1.0, 2.0, 1.0});
  Zone zone{"door", "Door", kSquare, 60.0, true};
  const Mono t{};
  entry.update(zone, inside_at(t));
  entry.update(zone, inside_at(t + 400ms));
  Observation missing = inside_at(t + 800ms);
  missing.observed = false;
  entry.update(zone, missing);
  auto update = entry.update(zone, inside_at(t + 1s));
  assert(!update.began && update.state == LoiterState::Entering);
  update = entry.update(zone, inside_at(t + 1600ms));
  assert(update.began);  // 0.4 s before + 0.6 s after occlusion.

  LoiteringEngine exit({0.0, 1.0, 2.0, 1.0});
  exit.update(zone, inside_at(t));
  update = exit.update(zone, outside_at(t + 100ms));
  assert(update.state == LoiterState::Exiting && !update.ended);
  missing = outside_at(t + 600ms);
  missing.observed = false;
  exit.update(zone, missing);
  update = exit.update(zone, outside_at(t + 1100ms));
  assert(!update.ended);  // Occlusion did not count as observed outside time.
  exit.update(zone, outside_at(t + 2s));
  update = exit.update(zone, outside_at(t + 2100ms));
  assert(update.ended && update.reason == "zone_exit");
}

void motion_estimate_is_robust_and_time_based() {
  MotionConfig config;
  config.enabled = true;
  config.start = 2.5;
  config.end = 1.5;
  config.window = 1.0;
  config.min_span = 0.5;
  config.start_confirm = 0.4;
  config.end_confirm = 0.3;
  config.lost_grace = 0.8;
  config.max_gap = 0.3;
  config.max_sample_speed = 8.0;
  config.min_box_height = 12.0;
  config.min_samples = 5;

  MotionEngine standing(config);
  const Mono t{};
  const double jitter[] = {0.0, 1.0, -1.0, 2.0, -1.0, 0.0, 1.0, -2.0};
  MotionUpdate update;
  for (int i = 0; i < 8; ++i) {
    Observation o{1, {100.0 + jitter[i], 100.0, 50.0, 100.0},
                  t + i * 100ms, true};
    update = standing.update(o);
  }
  assert(!update.active && update.speed < 0.2);

  MotionEngine moving(config);
  bool began = false;
  for (int i = 0; i <= 14; ++i) {
    const double seconds = i * 0.1;
    Observation o{2, {100.0 + 250.0 * seconds, 100.0, 50.0, 100.0},
                  t + i * 100ms, true};
    update = moving.update(o);
    began = began || update.began;
  }
  assert(began && update.active);
  assert(update.speed > 2.4 && update.speed < 2.6);

  Observation absent{2, {}, t + 1900ms, false};
  update = moving.update(absent);
  assert(update.active && update.speed == 0.0);
  absent.at = t + 2300ms;
  update = moving.update(absent);
  assert(update.ended && update.reason == "track_lost");
  assert(update.speed == 0.0);
}

void motion_rejects_jump_and_large_gap() {
  MotionConfig config;
  config.enabled = true;
  config.start = 2.0;
  config.end = 1.0;
  config.window = 1.0;
  config.min_span = 0.4;
  config.start_confirm = 0.2;
  config.end_confirm = 0.2;
  config.lost_grace = 1.0;
  config.max_gap = 0.3;
  config.max_sample_speed = 8.0;
  config.min_box_height = 12.0;
  config.min_samples = 5;

  MotionEngine engine(config);
  const Mono t{};
  MotionUpdate update;
  for (int i = 0; i < 5; ++i) {
    Observation o{3, {100.0, 100.0, 50.0, 100.0}, t + i * 100ms, true};
    update = engine.update(o);
  }
  Observation jump{3, {600.0, 100.0, 50.0, 100.0}, t + 500ms, true};
  update = engine.update(jump);
  assert(!update.active && update.speed == 0.0);

  Observation after_gap{3, {600.0, 100.0, 50.0, 100.0}, t + 2s, true};
  update = engine.update(after_gap);
  assert(!update.active && update.speed == 0.0);

  MotionConfig invalid = config;
  invalid.end = invalid.start;
  bool threw = false;
  try {
    MotionEngine bad(invalid);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  assert(threw);
}

void motion_confirmation_pauses_and_trajectory_resets_on_occlusion() {
  MotionConfig config;
  config.enabled = true;
  config.start = 1.0;
  config.end = 0.7;
  config.window = 0.5;
  config.min_span = 0.3;
  config.start_confirm = 0.5;
  config.end_confirm = 0.4;
  config.lost_grace = 1.0;
  config.max_gap = 0.2;
  config.min_samples = 5;
  MotionEngine engine(config);
  const Mono t{};
  MotionUpdate update;
  for (int i = 0; i <= 6; ++i) {
    Observation sample{9, {100.0 + 150.0 * i * 0.1, 100.0, 50.0, 100.0},
                       t + i * 100ms, true};
    update = engine.update(sample);
  }
  assert(!update.active);
  Observation missing{9, {}, t + 700ms, false};
  update = engine.update(missing);
  assert(!update.active);
  Observation resumed{9, {250.0, 100.0, 50.0, 100.0}, t + 1s, true};
  update = engine.update(resumed);
  assert(!update.active && update.speed == 0.0);
  for (int i = 11; i <= 14; ++i) {
    Observation sample{9, {100.0 + 150.0 * i * 0.1, 100.0, 50.0, 100.0},
                       t + i * 100ms, true};
    update = engine.update(sample);
    assert(!update.active);
  }
  for (int i = 15; i <= 19; ++i) {
    Observation sample{9, {100.0 + 150.0 * i * 0.1, 100.0, 50.0, 100.0},
                       t + i * 100ms, true};
    update = engine.update(sample);
  }
  assert(update.active);  // Confirmation restarts after speed is measurable again.
}
}  // namespace

int main() {
  polygon_geometry();
  loiter_dwell_exit_and_new_episode();
  loiter_pauses_on_occlusion_and_ends_after_loss();
  loiter_confirmation_timers_pause_during_occlusion();
  motion_estimate_is_robust_and_time_based();
  motion_rejects_jump_and_large_gap();
  motion_confirmation_pauses_and_trajectory_resets_on_occlusion();
}

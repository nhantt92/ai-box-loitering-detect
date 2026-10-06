#include "aibox/tracker.hpp"

#include <cassert>
#include <chrono>
#include <stdexcept>

using namespace aibox;
using namespace std::chrono_literals;

namespace {
Detection person(double x, float confidence = .9f, double w = 40,
                double h = 100) {
  return {{x, 50, w, h}, 0, confidence};
}

void confirms_and_preserves_ids_through_crossing() {
  ByteTracker tracker;
  const auto t = std::chrono::steady_clock::time_point{};
  auto tracks = tracker.update({person(20), person(180)}, t);
  assert(tracks.size() == 0);  // min_hits=2
  tracks = tracker.update({person(30), person(170)}, t + 100ms);
  assert(tracks.size() == 2);
  const int left_id = tracks[0].id;
  const int right_id = tracks[1].id;
  assert(left_id != right_id);
  // Objects cross; their constant-velocity predictions disambiguate the overlap.
  tracks = tracker.update({person(40), person(160)}, t + 200ms);
  tracks = tracker.update({person(50), person(150)}, t + 300ms);
  tracks = tracker.update({person(60), person(140)}, t + 400ms);
  tracks = tracker.update({person(70), person(130)}, t + 500ms);
  tracks = tracker.update({person(80), person(120)}, t + 600ms);
  tracks = tracker.update({person(90), person(110)}, t + 700ms);
  tracks = tracker.update({person(100), person(100)}, t + 800ms);
  tracks = tracker.update({person(110), person(90)}, t + 900ms);
  assert(tracks.size() == 2);
  double left_track_x = -1.0, right_track_x = -1.0;
  for (const auto& track : tracks) {
    if (track.id == left_id) left_track_x = track.box.x;
    if (track.id == right_id) right_track_x = track.box.x;
  }
  assert(left_track_x > right_track_x);  // IDs follow trajectories through crossing.
}

void second_stage_recovers_low_confidence_and_prediction_is_marked() {
  ByteTrackConfig c;
  c.min_hits = 1;
  ByteTracker tracker(c);
  const auto t = std::chrono::steady_clock::time_point{};
  auto tracks = tracker.update({person(30)}, t);
  assert(tracks.size() == 1 && tracks.front().observed);
  const auto id = tracks.front().id;
  tracks = tracker.update({person(40, .25f)}, t + 100ms);
  assert(tracks.size() == 1 && tracks.front().id == id && tracks.front().observed);
  tracks = tracker.update({}, t + 200ms);
  assert(tracks.size() == 1 && tracks.front().id == id && !tracks.front().observed);
}

void ttl_and_reset_are_time_based() {
  ByteTrackConfig c;
  c.min_hits = 1;
  c.track_ttl_seconds = .5;
  ByteTracker tracker(c);
  const auto t = std::chrono::steady_clock::time_point{};
  auto tracks = tracker.update({person(10)}, t);
  assert(tracks.size() == 1);
  tracks = tracker.update({}, t + 400ms);
  assert(tracks.size() == 1);
  tracks = tracker.update({}, t + 600ms);
  assert(tracks.empty());
  tracker.reset();
  tracks = tracker.update({person(10)}, t + 700ms);
  assert(tracks.size() == 1 && tracks.front().id == 1);
  tracks = tracker.update({person(10)}, t + 2s);
  assert(tracks.size() == 1 && tracks.front().id == 2);  // Expired track cannot reactivate.
  bool rejected_time = false;
  try { tracker.update({}, t + 1500ms); }
  catch (const std::invalid_argument&) { rejected_time = true; }
  assert(rejected_time);
}

void config_and_invalid_detections() {
  ByteTrackConfig invalid;
  invalid.high_confidence = .05f;
  bool threw = false;
  try { ByteTracker bad(invalid); }
  catch (const std::invalid_argument&) { threw = true; }
  assert(threw);
  ByteTrackConfig c;
  c.min_hits = 1;
  ByteTracker tracker(c);
  const auto t = std::chrono::steady_clock::time_point{};
  const auto tracks = tracker.update({person(10, .99f, -1), person(20, .01f)}, t);
  assert(tracks.empty());
}
}  // namespace

int main() {
  confirms_and_preserves_ids_through_crossing();
  second_stage_recovers_low_confidence_and_prediction_is_marked();
  ttl_and_reset_are_time_based();
  config_and_invalid_detections();
}

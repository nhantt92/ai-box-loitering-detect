#include "aibox/engines.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace aibox {
namespace {
constexpr double kGeometryEpsilon = 1e-10;

double cross(Point a, Point b, Point c) {
  return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

bool on_segment(Point a, Point b, Point p) {
  return std::abs(cross(a, b, p)) <= kGeometryEpsilon &&
         p.x >= std::min(a.x, b.x) - kGeometryEpsilon &&
         p.x <= std::max(a.x, b.x) + kGeometryEpsilon &&
         p.y >= std::min(a.y, b.y) - kGeometryEpsilon &&
         p.y <= std::max(a.y, b.y) + kGeometryEpsilon;
}

int orientation(Point a, Point b, Point c) {
  const double value = cross(a, b, c);
  if (value > kGeometryEpsilon) return 1;
  if (value < -kGeometryEpsilon) return -1;
  return 0;
}

bool segments_intersect(Point a, Point b, Point c, Point d) {
  const int o1 = orientation(a, b, c);
  const int o2 = orientation(a, b, d);
  const int o3 = orientation(c, d, a);
  const int o4 = orientation(c, d, b);
  if (o1 != o2 && o3 != o4) return true;
  return (o1 == 0 && on_segment(a, b, c)) ||
         (o2 == 0 && on_segment(a, b, d)) ||
         (o3 == 0 && on_segment(c, d, a)) ||
         (o4 == 0 && on_segment(c, d, b));
}

bool finite_box(const Box& b) {
  return std::isfinite(b.x) && std::isfinite(b.y) && std::isfinite(b.w) &&
         std::isfinite(b.h) && b.w > 0.0 && b.h > 0.0;
}

void require_nonnegative_finite(double value, const char* name) {
  if (!std::isfinite(value) || value < 0.0)
    throw std::invalid_argument(std::string("Invalid ") + name);
}

double median(std::vector<double> values) {
  if (values.empty()) return 0.0;
  const auto middle = values.begin() + values.size() / 2;
  std::nth_element(values.begin(), middle, values.end());
  const double upper = *middle;
  if (values.size() % 2 != 0) return upper;
  const auto lower = std::max_element(values.begin(), middle);
  return (*lower + upper) * 0.5;
}
}  // namespace

bool valid_polygon(const std::vector<Point>& polygon) {
  if (polygon.size() < 3) return false;
  double twice_area = 0.0;
  for (std::size_t i = 0; i < polygon.size(); ++i) {
    const Point a = polygon[i];
    const Point b = polygon[(i + 1) % polygon.size()];
    if (!std::isfinite(a.x) || !std::isfinite(a.y) || a.x < 0.0 ||
        a.x > 1.0 || a.y < 0.0 || a.y > 1.0)
      return false;
    const double dx = a.x - b.x;
    const double dy = a.y - b.y;
    if (dx * dx + dy * dy <= kGeometryEpsilon * kGeometryEpsilon)
      return false;
    twice_area += a.x * b.y - b.x * a.y;
  }
  if (std::abs(twice_area) <= kGeometryEpsilon) return false;

  const std::size_t n = polygon.size();
  for (std::size_t i = 0; i < n; ++i) {
    const std::size_t i_next = (i + 1) % n;
    for (std::size_t j = i + 1; j < n; ++j) {
      const std::size_t j_next = (j + 1) % n;
      if (i == j || i_next == j || j_next == i) continue;
      if (segments_intersect(polygon[i], polygon[i_next], polygon[j],
                             polygon[j_next]))
        return false;
    }
  }
  return true;
}

bool inside_polygon(const std::vector<Point>& polygon, Point point) {
  if (!valid_polygon(polygon) || !std::isfinite(point.x) ||
      !std::isfinite(point.y))
    return false;
  bool inside = false;
  for (std::size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
    const Point a = polygon[j];
    const Point b = polygon[i];
    if (on_segment(a, b, point)) return true;  // Boundary is inside.
    if ((a.y > point.y) != (b.y > point.y)) {
      const double crossing_x =
          (b.x - a.x) * (point.y - a.y) / (b.y - a.y) + a.x;
      if (point.x < crossing_x) inside = !inside;
    }
  }
  return inside;
}

LoiteringEngine::LoiteringEngine(LoiterConfig config) : c_(config) {
  require_nonnegative_finite(c_.enter_confirm, "loiter enter confirmation");
  require_nonnegative_finite(c_.exit_confirm, "loiter exit confirmation");
  require_nonnegative_finite(c_.lost_grace, "loiter lost grace");
  if (!std::isfinite(c_.max_gap) || c_.max_gap <= 0.0)
    throw std::invalid_argument("Loiter max_gap must be positive");
}

bool LoiteringEngine::episode_active() const { return episode_started_; }

void LoiteringEngine::clear() {
  s_ = LoiterState::Outside;
  last_update_.reset();
  last_observed_.reset();
  last_update_observed_ = false;
  last_inside_ = false;
  dwell_ = 0.0;
  enter_elapsed_ = 0.0;
  outside_elapsed_ = 0.0;
  episode_started_ = false;
}

LoiterUpdate LoiteringEngine::reset(const std::string& reason) {
  const bool ended = episode_active();
  clear();
  return {ended ? LoiterState::Ended : LoiterState::Outside, 0.0, false, ended,
          ended ? reason : std::string{}};
}

LoiterUpdate LoiteringEngine::update(const Zone& zone,
                                     const Observation& observation) {
  if (!zone.enabled || !valid_polygon(zone.polygon) ||
      !std::isfinite(zone.dwell_seconds) || zone.dwell_seconds < 0.0)
    return reset("config_changed");

  if (s_ == LoiterState::Ended) clear();
  const bool alarm_before = s_ == LoiterState::Alarm;
  const Mono now = observation.at;
  if (last_update_ && now <= *last_update_) return reset("track_lost");

  const double dt = last_update_
                        ? std::chrono::duration<double>(now - *last_update_).count()
                        : 0.0;
  const double observed_gap = last_observed_
                                  ? std::chrono::duration<double>(now - *last_observed_)
                                        .count()
                                  : 0.0;

  if (!observation.observed || !finite_box(observation.box)) {
    last_update_ = now;
    last_update_observed_ = false;
    // Confirmation timers count observed intervals only. The candidate is
    // retained during the lost grace period, but occlusion adds no time.
    if (last_observed_ && observed_gap > c_.lost_grace) {
      if (episode_active()) return reset("track_lost");
      clear();
      return {LoiterState::Ended, 0.0, false, true, "track_lost"};
    }
    return {s_, dwell_, false, false, {}};
  }

  if (episode_active() && last_observed_ && observed_gap > c_.lost_grace) {
    LoiterUpdate ended = reset("track_lost");
    // The current observed point starts a fresh entry candidate.
    last_update_ = now;
    last_observed_ = now;
    last_update_observed_ = true;
    const Point foot{observation.box.x + observation.box.w * 0.5,
                     observation.box.y + observation.box.h};
    last_inside_ = inside_polygon(zone.polygon, foot);
    if (last_inside_) {
      s_ = LoiterState::Entering;
      enter_elapsed_ = 0.0;
    }
    return ended;
  }

  const bool gap = last_update_ && dt > c_.max_gap;
  const Point foot{observation.box.x + observation.box.w * 0.5,
                   observation.box.y + observation.box.h};
  const bool inside = inside_polygon(zone.polygon, foot);
  LoiterUpdate update;

  if (s_ == LoiterState::Entering && gap) {
    s_ = LoiterState::Outside;
    enter_elapsed_ = 0.0;
    dwell_ = 0.0;
  }

  if (inside) {
    outside_elapsed_ = 0.0;
    if (s_ == LoiterState::Outside) {
      s_ = LoiterState::Entering;
      enter_elapsed_ = 0.0;
      dwell_ = 0.0;
    }

    const bool consecutive_inside = last_update_ && last_update_observed_ &&
                                   last_inside_ && !gap;
    if (consecutive_inside &&
        (s_ == LoiterState::Entering || s_ == LoiterState::Inside ||
         s_ == LoiterState::Alarm || s_ == LoiterState::Exiting))
      dwell_ += dt;

    if (s_ == LoiterState::Entering && consecutive_inside)
      enter_elapsed_ += dt;
    if (s_ == LoiterState::Entering && enter_elapsed_ >= c_.enter_confirm) {
      s_ = LoiterState::Inside;
      episode_started_ = true;
      update.began = true;
    } else if (s_ == LoiterState::Exiting) {
      s_ = dwell_ >= zone.dwell_seconds ? LoiterState::Alarm
                                        : LoiterState::Inside;
    }
    if (episode_active() && dwell_ >= zone.dwell_seconds)
      s_ = LoiterState::Alarm;
  } else {
    if (s_ == LoiterState::Entering) {
      s_ = LoiterState::Outside;
      enter_elapsed_ = 0.0;
      dwell_ = 0.0;
    } else if (s_ == LoiterState::Inside || s_ == LoiterState::Alarm) {
      s_ = LoiterState::Exiting;
      outside_elapsed_ = 0.0;
    } else if (s_ == LoiterState::Exiting) {
      // Both endpoints were observed outside. Count the elapsed wall time
      // between them even when it exceeds max_gap; otherwise slow inference
      // can leave an active alarm stuck in Exiting forever.
      if (last_update_ && last_update_observed_ && !last_inside_)
        outside_elapsed_ += dt;
      if (outside_elapsed_ < c_.exit_confirm) {
        // Keep the episode alive until observed outside time is sufficient.
      } else {
      update = reset("zone_exit");
      return update;
      }
    }
  }

  last_update_ = now;
  last_observed_ = now;
  last_update_observed_ = true;
  last_inside_ = inside;
  update.state = s_;
  update.dwell = dwell_;
  update.alarm_began = !alarm_before && s_ == LoiterState::Alarm;
  return update;
}

MotionEngine::MotionEngine(MotionConfig config) : c_(config) {
  require_nonnegative_finite(c_.start, "motion start threshold");
  require_nonnegative_finite(c_.end, "motion end threshold");
  require_nonnegative_finite(c_.window, "motion window");
  require_nonnegative_finite(c_.min_span, "motion minimum span");
  require_nonnegative_finite(c_.start_confirm, "motion start confirmation");
  require_nonnegative_finite(c_.end_confirm, "motion end confirmation");
  require_nonnegative_finite(c_.lost_grace, "motion lost grace");
  require_nonnegative_finite(c_.max_sample_speed, "motion outlier speed");
  require_nonnegative_finite(c_.min_box_height, "motion minimum box height");
  if (c_.end >= c_.start || c_.window <= 0.0 || c_.min_span <= 0.0 ||
      c_.max_gap <= 0.0 || c_.max_sample_speed <= 0.0 || c_.min_samples < 2)
    throw std::invalid_argument("Invalid motion engine thresholds or window");
  if (!std::isfinite(c_.max_gap))
    throw std::invalid_argument("Motion max_gap must be finite");
}

void MotionEngine::clear_window() {
  pts_.clear();
  speed_ = 0.0;
}

MotionUpdate MotionEngine::reset(const std::string& reason) {
  MotionUpdate result;
  result.active = false;
  result.ended = active_;
  result.reason = result.ended ? reason : std::string{};
  pts_.clear();
  last_.reset();
  speed_ = 0.0;
  active_ = false;
  above_elapsed_ = 0.0;
  below_elapsed_ = 0.0;
  last_observation_valid_ = false;
  return result;
}

double MotionEngine::estimate_speed() const {
  if (pts_.size() < 2) return 0.0;
  std::vector<double> vx;
  std::vector<double> vy;
  std::vector<double> heights;
  heights.reserve(pts_.size());
  for (const auto& point : pts_) heights.push_back(point.height);
  for (std::size_t i = 0; i < pts_.size(); ++i) {
    for (std::size_t j = i + 1; j < pts_.size(); ++j) {
      const double seconds =
          std::chrono::duration<double>(pts_[j].at - pts_[i].at).count();
      if (seconds <= 0.0) continue;
      vx.push_back((pts_[j].foot.x - pts_[i].foot.x) / seconds);
      vy.push_back((pts_[j].foot.y - pts_[i].foot.y) / seconds);
    }
  }
  if (vx.empty()) return 0.0;
  const double reference_height = median(std::move(heights));
  if (reference_height <= 0.0) return 0.0;
  return std::hypot(median(std::move(vx)), median(std::move(vy))) /
         reference_height;
}

MotionUpdate MotionEngine::update(const Observation& observation) {
  if (!c_.enabled) return reset("config_changed");
  if (!observation.observed || !finite_box(observation.box) ||
      observation.box.h < c_.min_box_height) {
    if (last_ && observation.at > *last_ &&
        std::chrono::duration<double>(observation.at - *last_).count() >
            c_.lost_grace)
      return reset("track_lost");
    // Do not estimate a trajectory through an occlusion. Keep the active
    // state and already-observed confirmation duration, but pause both.
    pts_.clear();
    speed_ = 0.0;
    last_observation_valid_ = false;
    MotionUpdate result;
    result.active = active_;
    result.speed = 0.0;  // Missing observations do not extend speed or hysteresis.
    return result;
  }

  bool gap = false;
  double dt = 0.0;
  if (last_) {
    dt = std::chrono::duration<double>(observation.at - *last_).count();
    if (dt <= 0.0) return reset("track_lost");
    if (dt > c_.lost_grace) return reset("track_lost");
    gap = !last_observation_valid_ || dt > c_.max_gap;
    if (gap) clear_window();
  }

  const Point foot{observation.box.x + observation.box.w * 0.5,
                   observation.box.y + observation.box.h};
  if (!pts_.empty()) {
    const auto& previous = pts_.back();
    const double dt =
        std::chrono::duration<double>(observation.at - previous.at).count();
    const double scale = (previous.height + observation.box.h) * 0.5;
    const double sample_speed =
        dt > 0.0 ? std::hypot(foot.x - previous.foot.x,
                              foot.y - previous.foot.y) /
                      (dt * scale)
                  : std::numeric_limits<double>::infinity();
    if (sample_speed > c_.max_sample_speed) {
      clear_window();
      above_elapsed_ = 0.0;
      below_elapsed_ = 0.0;
      gap = true;
    }
  }

  pts_.push_back({observation.at, foot, observation.box.h});
  const auto window = std::chrono::duration<double>(c_.window);
  while (!pts_.empty() && observation.at - pts_.front().at > window)
    pts_.erase(pts_.begin());
  last_ = observation.at;
  last_observation_valid_ = true;

  speed_ = 0.0;
  if (pts_.size() >= static_cast<std::size_t>(c_.min_samples)) {
    const double span =
        std::chrono::duration<double>(pts_.back().at - pts_.front().at).count();
    if (span >= c_.min_span) speed_ = estimate_speed();
  }

  MotionUpdate result;
  result.speed = speed_;
  if (gap) {
    result.active = active_;
    return result;
  }
  if (!active_ && speed_ >= c_.start) {
    above_elapsed_ += dt;
    below_elapsed_ = 0.0;
    if (above_elapsed_ >= c_.start_confirm) {
      active_ = true;
      result.began = true;
      above_elapsed_ = 0.0;
    }
  } else if (active_ && speed_ <= c_.end) {
    below_elapsed_ += dt;
    above_elapsed_ = 0.0;
    if (below_elapsed_ >= c_.end_confirm) {
      active_ = false;
      result.ended = true;
      below_elapsed_ = 0.0;
    }
  } else {
    above_elapsed_ = 0.0;
    below_elapsed_ = 0.0;
  }
  result.active = active_;
  return result;
}

CrowdEngine::CrowdEngine(CrowdConfig config) : c_(config) {
  require_nonnegative_finite(c_.enter_confirm, "crowd enter confirmation");
  require_nonnegative_finite(c_.exit_confirm, "crowd exit confirmation");
  if (c_.trigger_people < 1 || c_.clear_people < 0 ||
      c_.clear_people >= c_.trigger_people || !std::isfinite(c_.max_gap) ||
      c_.max_gap <= 0.0)
    throw std::invalid_argument("Invalid crowd thresholds or observation gap");
}

CrowdUpdate CrowdEngine::reset(const std::string& reason) {
  CrowdUpdate result;
  result.ended = active_;
  result.active = false;
  result.reason = active_ ? reason : std::string{};
  active_ = false;
  last_.reset();
  above_elapsed_ = below_elapsed_ = 0.0;
  return result;
}

CrowdUpdate CrowdEngine::update(int people, Mono at) {
  if (people < 0) throw std::invalid_argument("Crowd count cannot be negative");
  if (!c_.enabled) return reset("config_changed");
  CrowdUpdate result;
  result.people = people;
  result.active = active_;
  if (last_ && at <= *last_) return reset("invalid_timestamp");
  if (!last_) {
    last_ = at;
    return result;
  }
  const double dt = std::chrono::duration<double>(at - *last_).count();
  last_ = at;
  if (dt > c_.max_gap) {
    above_elapsed_ = below_elapsed_ = 0.0;
    return result;
  }
  if (!active_ && people >= c_.trigger_people) {
    above_elapsed_ += dt;
    below_elapsed_ = 0.0;
    if (above_elapsed_ >= c_.enter_confirm) {
      active_ = true;
      result.began = true;
      above_elapsed_ = 0.0;
    }
  } else if (active_ && people <= c_.clear_people) {
    below_elapsed_ += dt;
    above_elapsed_ = 0.0;
    if (below_elapsed_ >= c_.exit_confirm) {
      active_ = false;
      result.ended = true;
      result.reason = "crowd_cleared";
      below_elapsed_ = 0.0;
    }
  } else {
    above_elapsed_ = below_elapsed_ = 0.0;
  }
  result.active = active_;
  return result;
}
}  // namespace aibox

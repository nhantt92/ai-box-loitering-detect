#include "aibox/tracker.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>
#include <optional>
#include <stdexcept>

namespace aibox {
namespace {
using Clock = std::chrono::steady_clock;
constexpr int kN = 8;
constexpr int kM = 4;
using State = std::array<double, kN>;
using Covariance = std::array<std::array<double, kN>, kN>;
using Measurement = std::array<double, kM>;

bool finite_box(const Box& b) {
  return std::isfinite(b.x) && std::isfinite(b.y) && std::isfinite(b.w) &&
         std::isfinite(b.h) && b.w > 0 && b.h > 0;
}

Measurement measure(const Box& b) {
  return {b.x + b.w * 0.5, b.y + b.h * 0.5, b.w / b.h, b.h};
}

Box box_from(const State& x) {
  const double h = std::max(1e-3, x[3]);
  const double w = std::max(1e-3, x[2] * h);
  return {x[0] - w * .5, x[1] - h * .5, w, h};
}

double iou(const Box& a, const Box& b) {
  const double x0 = std::max(a.x, b.x), y0 = std::max(a.y, b.y);
  const double x1 = std::min(a.x + a.w, b.x + b.w);
  const double y1 = std::min(a.y + a.h, b.y + b.h);
  const double inter = std::max(0.0, x1 - x0) * std::max(0.0, y1 - y0);
  const double total = a.w * a.h + b.w * b.h - inter;
  return total > 0 ? inter / total : 0;
}

bool invert4(const std::array<std::array<double, kM>, kM>& input,
             std::array<std::array<double, kM>, kM>& inverse) {
  std::array<std::array<double, 2 * kM>, kM> a{};
  for (int r = 0; r < kM; ++r) {
    for (int c = 0; c < kM; ++c) a[r][c] = input[r][c];
    a[r][kM + r] = 1;
  }
  for (int c = 0; c < kM; ++c) {
    int pivot = c;
    for (int r = c + 1; r < kM; ++r)
      if (std::abs(a[r][c]) > std::abs(a[pivot][c])) pivot = r;
    if (std::abs(a[pivot][c]) < 1e-12) return false;
    std::swap(a[pivot], a[c]);
    const double d = a[c][c];
    for (double& value : a[c]) value /= d;
    for (int r = 0; r < kM; ++r) {
      if (r == c) continue;
      const double factor = a[r][c];
      for (int j = 0; j < 2 * kM; ++j) a[r][j] -= factor * a[c][j];
    }
  }
  for (int r = 0; r < kM; ++r)
    for (int c = 0; c < kM; ++c) inverse[r][c] = a[r][kM + c];
  return true;
}

// Rectangular Hungarian assignment. A match is returned only when its cost is
// below max_cost; dummy columns let every row remain unmatched.
std::vector<std::pair<int, int>> assign(const std::vector<std::vector<double>>& cost,
                                         double max_cost) {
  const int rows = static_cast<int>(cost.size());
  if (!rows) return {};
  const int cols = static_cast<int>(cost.front().size());
  if (!cols) return {};
  const int n = rows, m = cols + rows;
  std::vector<double> u(n + 1), v(m + 1);
  std::vector<int> p(m + 1), way(m + 1);
  for (int i = 1; i <= n; ++i) {
    p[0] = i;
    int j0 = 0;
    std::vector<double> minv(m + 1, std::numeric_limits<double>::infinity());
    std::vector<char> used(m + 1, false);
    do {
      used[j0] = true;
      const int i0 = p[j0];
      double delta = std::numeric_limits<double>::infinity();
      int j1 = 0;
      for (int j = 1; j <= m; ++j) {
        if (used[j]) continue;
        const double c = j <= cols ? cost[i0 - 1][j - 1] : max_cost;
        const double cur = c - u[i0] - v[j];
        if (cur < minv[j]) { minv[j] = cur; way[j] = j0; }
        if (minv[j] < delta) { delta = minv[j]; j1 = j; }
      }
      for (int j = 0; j <= m; ++j) {
        if (used[j]) { u[p[j]] += delta; v[j] -= delta; }
        else minv[j] -= delta;
      }
      j0 = j1;
    } while (p[j0] != 0);
    do { const int j1 = way[j0]; p[j0] = p[j1]; j0 = j1; } while (j0);
  }
  std::vector<std::pair<int, int>> matches;
  for (int j = 1; j <= cols; ++j)
    if (p[j] && cost[p[j] - 1][j - 1] < max_cost)
      matches.emplace_back(p[j] - 1, j - 1);
  return matches;
}

struct InternalTrack {
  int id{};
  int class_id{};
  float confidence{};
  State x{};
  Covariance p{};
  Clock::time_point born{}, last_seen{}, state_at{};
  int hits{};
  bool confirmed{};
  bool lost{};
  bool matched_this_update{};
};

void predict(InternalTrack& t, double dt, const ByteTrackConfig& c) {
  dt = std::clamp(dt, 0.0, c.max_predict_seconds);
  for (int i = 0; i < 4; ++i) t.x[i] += t.x[i + 4] * dt;
  double f[kN][kN]{};
  for (int i = 0; i < kN; ++i) f[i][i] = 1;
  for (int i = 0; i < 4; ++i) f[i][i + 4] = dt;
  Covariance fp{}, next{};
  for (int i = 0; i < kN; ++i)
    for (int j = 0; j < kN; ++j)
      for (int k = 0; k < kN; ++k) fp[i][j] += f[i][k] * t.p[k][j];
  for (int i = 0; i < kN; ++i)
    for (int j = 0; j < kN; ++j) {
      for (int k = 0; k < kN; ++k) next[i][j] += fp[i][k] * f[j][k];
      const double q = c.process_noise * (i < 4 ? 1.0 : 4.0) *
                       (i == 2 || i == 6 ? .01 : 1.0);
      next[i][j] += i == j ? q * std::max(dt, 1e-3) : 0;
    }
  t.p = next;
}

void correct(InternalTrack& t, const Box& b, float confidence,
             int class_id, Clock::time_point at, const ByteTrackConfig& c) {
  const auto z = measure(b);
  std::array<std::array<double, kM>, kM> s{}, inv{};
  for (int i = 0; i < kM; ++i)
    for (int j = 0; j < kM; ++j)
      s[i][j] = t.p[i][j] + (i == j ? c.measurement_noise *
            (i == 2 ? .01 : (i == 3 ? std::max(1.0, z[3] * .01) : 1.0)) : 0);
  if (invert4(s, inv)) {
    double k[kN][kM]{};
    for (int i = 0; i < kN; ++i)
      for (int j = 0; j < kM; ++j)
        for (int q = 0; q < kM; ++q) k[i][j] += t.p[i][q] * inv[q][j];
    double residual[kM]{};
    for (int i = 0; i < kM; ++i) residual[i] = z[i] - t.x[i];
    for (int i = 0; i < kN; ++i)
      for (int j = 0; j < kM; ++j) t.x[i] += k[i][j] * residual[j];
    Covariance old = t.p;
    for (int i = 0; i < kN; ++i)
      for (int j = 0; j < kN; ++j) {
        double value = old[i][j];
        for (int q = 0; q < kM; ++q) value -= k[i][q] * old[q][j];
        t.p[i][j] = value;
      }
  } else {
    for (int i = 0; i < kM; ++i) t.x[i] = z[i];
  }
  t.last_seen = t.state_at = at;
  t.confidence = confidence;
  t.class_id = class_id;
  ++t.hits;
  if (t.hits >= c.min_hits) t.confirmed = true;
  t.lost = false;
  t.matched_this_update = true;
}

InternalTrack make_track(const Detection& d, Clock::time_point at, int id,
                         const ByteTrackConfig& c) {
  InternalTrack t;
  t.id = id; t.class_id = d.class_id; t.confidence = d.confidence;
  const auto z = measure(d.box);
  std::copy(z.begin(), z.end(), t.x.begin());
  for (int i = 0; i < kN; ++i) t.p[i][i] = i < 4 ? 10.0 : 100.0;
  t.born = t.last_seen = t.state_at = at;
  t.hits = 1;
  t.confirmed = c.min_hits <= 1;
  t.matched_this_update = true;
  return t;
}

}  // namespace

struct ByteTracker::Impl {
  int next_id{1};
  std::optional<Clock::time_point> last_update;
  std::vector<InternalTrack> tracks;
};

ByteTracker::ByteTracker(ByteTrackConfig config)
    : config_(config), impl_(std::make_unique<Impl>()) {
  auto unit = [](float x) { return std::isfinite(x) && x >= 0 && x <= 1; };
  auto positive = [](double x) { return std::isfinite(x) && x > 0; };
  if (!unit(config_.low_confidence) || !unit(config_.high_confidence) ||
      !unit(config_.new_track_confidence) ||
      config_.low_confidence > config_.high_confidence ||
      config_.high_confidence > config_.new_track_confidence ||
      !unit(config_.first_match_iou) || !unit(config_.second_match_iou) ||
      !unit(config_.unconfirmed_match_iou) ||
      !positive(config_.track_ttl_seconds) ||
      !positive(config_.max_predict_seconds) ||
      !positive(config_.process_noise) || !positive(config_.measurement_noise) ||
      config_.min_hits < 1)
    throw std::invalid_argument("Invalid ByteTrack configuration");
}

ByteTracker::~ByteTracker() = default;
ByteTracker::ByteTracker(ByteTracker&&) noexcept = default;
ByteTracker& ByteTracker::operator=(ByteTracker&&) noexcept = default;

void ByteTracker::reset() { impl_ = std::make_unique<Impl>(); }

std::vector<Track> ByteTracker::update(const std::vector<Detection>& input,
                                      Clock::time_point at) {
  if (impl_->last_update && at <= *impl_->last_update)
    throw std::invalid_argument("ByteTracker timestamps must increase");
  impl_->last_update = at;
  impl_->tracks.erase(std::remove_if(impl_->tracks.begin(), impl_->tracks.end(),
      [&](const InternalTrack& t) {
        return std::chrono::duration<double>(at - t.last_seen).count() >
               config_.track_ttl_seconds;
      }), impl_->tracks.end());
  std::vector<Detection> valid;
  valid.reserve(input.size());
  for (const auto& d : input)
    if (finite_box(d.box) && std::isfinite(d.confidence) &&
        d.confidence >= config_.low_confidence && d.confidence <= 1.0f)
      valid.push_back(d);

  for (auto& t : impl_->tracks) {
    t.matched_this_update = false;
    const double dt = std::chrono::duration<double>(at - t.state_at).count();
    if (dt >= 0) predict(t, dt, config_);
    t.state_at = at;
  }

  std::vector<int> high, low;
  for (int i = 0; i < static_cast<int>(valid.size()); ++i)
    (valid[i].confidence >= config_.high_confidence ? high : low).push_back(i);
  std::vector<int> pool;
  for (int i = 0; i < static_cast<int>(impl_->tracks.size()); ++i)
    if (impl_->tracks[i].confirmed) pool.push_back(i);

  auto associate = [&](const std::vector<int>& track_ids,
                       const std::vector<int>& det_ids, double threshold,
                       std::vector<int>& unmatched_tracks,
                       std::vector<int>& unmatched_dets) {
    std::vector<std::vector<double>> costs(track_ids.size(),
        std::vector<double>(det_ids.size(), 1.0));
    for (std::size_t r = 0; r < track_ids.size(); ++r)
      for (std::size_t c = 0; c < det_ids.size(); ++c) {
        const auto& t = impl_->tracks[track_ids[r]];
        const auto& d = valid[det_ids[c]];
        if (t.class_id == d.class_id)
          costs[r][c] = 1.0 - iou(box_from(t.x), d.box);
      }
    auto matches = assign(costs, 1.0 - threshold);
    std::vector<char> used_tracks(track_ids.size()), used_dets(det_ids.size());
    for (const auto& [r, c] : matches) {
      correct(impl_->tracks[track_ids[r]], valid[det_ids[c]].box,
              valid[det_ids[c]].confidence, valid[det_ids[c]].class_id, at,
              config_);
      used_tracks[r] = used_dets[c] = true;
    }
    unmatched_tracks.clear(); unmatched_dets.clear();
    for (std::size_t i = 0; i < track_ids.size(); ++i)
      if (!used_tracks[i]) unmatched_tracks.push_back(track_ids[i]);
    for (std::size_t i = 0; i < det_ids.size(); ++i)
      if (!used_dets[i]) unmatched_dets.push_back(det_ids[i]);
  };

  std::vector<int> unmatched_pool, unmatched_high;
  associate(pool, high, config_.first_match_iou, unmatched_pool, unmatched_high);
  std::vector<int> active_unmatched;
  for (int i : unmatched_pool)
    if (!impl_->tracks[i].lost) active_unmatched.push_back(i);
  std::vector<int> unmatched_after_low, ignored_low;
  associate(active_unmatched, low, config_.second_match_iou,
            unmatched_after_low, ignored_low);
  for (int i : unmatched_pool)
    if (!impl_->tracks[i].matched_this_update) impl_->tracks[i].lost = true;

  // Tentative tracks get one high-confidence association opportunity.
  std::vector<int> tentative;
  for (int i = 0; i < static_cast<int>(impl_->tracks.size()); ++i)
    if (!impl_->tracks[i].confirmed && !impl_->tracks[i].lost) tentative.push_back(i);
  std::vector<int> unmatched_tentative, remaining_high;
  associate(tentative, unmatched_high, config_.unconfirmed_match_iou,
            unmatched_tentative, remaining_high);
  for (int i : unmatched_tentative) impl_->tracks[i].lost = true;
  impl_->tracks.erase(std::remove_if(impl_->tracks.begin(), impl_->tracks.end(),
      [](const InternalTrack& t) { return !t.confirmed && t.lost; }),
      impl_->tracks.end());

  for (int i : remaining_high)
    if (valid[i].confidence >= config_.new_track_confidence)
      impl_->tracks.push_back(make_track(valid[i], at, impl_->next_id++, config_));

  impl_->tracks.erase(std::remove_if(impl_->tracks.begin(), impl_->tracks.end(),
      [&](const InternalTrack& t) {
        return std::chrono::duration<double>(at - t.last_seen).count() >
               config_.track_ttl_seconds;
      }), impl_->tracks.end());

  std::vector<Track> output;
  for (const auto& t : impl_->tracks) {
    if (!t.confirmed) continue;
    output.push_back({t.id, t.class_id, box_from(t.x), t.confidence,
                      t.matched_this_update, t.born, t.last_seen});
  }
  return output;
}

}  // namespace aibox

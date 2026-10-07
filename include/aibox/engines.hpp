#pragma once
#include "aibox/types.hpp"
#include <optional>
#include <stdexcept>
namespace aibox {
bool valid_polygon(const std::vector<Point>& p);
bool inside_polygon(const std::vector<Point>& p, Point q);
enum class LoiterState { Outside, Entering, Inside, Alarm, Exiting, Ended };
struct LoiterConfig { double enter_confirm{1}, exit_confirm{1}, lost_grace{2}, max_gap{1}; };
struct LoiterUpdate { LoiterState state{LoiterState::Outside}; double dwell{}; bool began{}, ended{}; std::string reason; bool alarm_began{}; };
class LoiteringEngine {
 public: explicit LoiteringEngine(LoiterConfig c={});
 LoiterUpdate update(const Zone& z, const Observation& o);
 LoiterUpdate reset(const std::string& reason);
 private:
  void clear();
  bool episode_active() const;
  LoiterConfig c_;
  LoiterState s_{LoiterState::Outside};
  std::optional<Mono> last_update_, last_observed_;
  bool last_update_observed_{}, last_inside_{};
  double dwell_{}, enter_elapsed_{}, outside_elapsed_{};
  bool episode_started_{};
};
struct MotionConfig { bool enabled{false}; double start{2.5}, end{1.8}, window{1}, min_span{.5}, start_confirm{.8}, end_confirm{1}, lost_grace{2}, max_gap{.5}, max_sample_speed{8.0}, min_box_height{12.0}; int min_samples{5}; };
struct MotionUpdate { bool active{}; double speed{}; std::string unit{"body_heights_per_second"}; bool began{}, ended{}; std::string reason; };
class MotionEngine {
 public: explicit MotionEngine(MotionConfig c={}); MotionUpdate update(const Observation& o); MotionUpdate reset(const std::string& reason);
 private:
  struct Sample { Mono at; Point foot; double height; };
  void clear_window();
  double estimate_speed() const;
  MotionConfig c_;
  std::vector<Sample> pts_;
  bool active_{};
  std::optional<Mono> last_;
  double above_elapsed_{}, below_elapsed_{}, speed_{};
  bool last_observation_valid_{};
};
struct CrowdConfig {
  bool enabled{false};
  int trigger_people{5};
  int clear_people{3};
  double enter_confirm{1.0};
  double exit_confirm{2.0};
  double max_gap{1.0};
};
struct CrowdUpdate {
  int people{};
  bool active{};
  bool began{}, ended{};
  std::string reason;
};
class CrowdEngine {
 public:
  explicit CrowdEngine(CrowdConfig config = {});
  CrowdUpdate update(int people, Mono at);
  CrowdUpdate reset(const std::string& reason);
 private:
  CrowdConfig c_;
  bool active_{};
  std::optional<Mono> last_;
  double above_elapsed_{}, below_elapsed_{};
};
}

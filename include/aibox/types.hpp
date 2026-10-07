#pragma once
#include <string>
#include <vector>
#include <chrono>
namespace aibox {
using Mono = std::chrono::steady_clock::time_point;
struct Point { double x{}, y{}; };
struct Box { double x{}, y{}, w{}, h{}; };
struct Observation { int track_id{}; Box box; Mono at; bool observed{true}; };
struct Zone { std::string id, name; std::vector<Point> polygon; double dwell_seconds{60}; bool enabled{true}; };
struct Event { std::string id, type, zone_id, reason, session_id; int track_id{}; Mono started, ended; std::chrono::system_clock::time_point started_utc, ended_utc; double dwell_seconds{}, peak_speed{}; std::string speed_unit; bool active{true}; int crowd_count{}, crowd_threshold{}; };
struct EventTransition { Event event; bool started{}; };
struct BehaviorSignal { int track_id{}; std::string type, zone_id, zone_name; bool active{}, observed{}; double dwell_seconds{}, speed{}; std::string speed_unit; bool inside{}; double target_seconds{}; int count{}, threshold{}; };
}

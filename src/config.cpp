#include "aibox/config.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <algorithm>
#include <charconv>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <fcntl.h>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unistd.h>

namespace aibox {
namespace {
using Json = nlohmann::json;

void keys_only(const Json& value, std::initializer_list<const char*> allowed,
               const char* context) {
  if (!value.is_object()) throw std::invalid_argument(std::string(context) + " must be an object");
  std::set<std::string> known;
  for (const auto* key : allowed) known.emplace(key);
  for (auto it = value.begin(); it != value.end(); ++it)
    if (!known.count(it.key()))
      throw std::invalid_argument(std::string("Unknown ") + context + " key: " + it.key());
}

double number(const Json& value, const char* name) {
  if (!value.is_number()) throw std::invalid_argument(std::string(name) + " must be numeric");
  const double result = value.get<double>();
  if (!std::isfinite(result)) throw std::invalid_argument(std::string(name) + " must be finite");
  return result;
}

bool optional_bool(const Json& object, const char* name, bool fallback) {
  const auto it = object.find(name);
  if (it == object.end()) return fallback;
  if (!it->is_boolean()) throw std::invalid_argument(std::string(name) + " must be boolean");
  return it->get<bool>();
}

std::string string_value(const Json& object, const char* name,
                         const std::string& fallback = {}) {
  const auto it = object.find(name);
  if (it == object.end()) return fallback;
  if (!it->is_string()) throw std::invalid_argument(std::string(name) + " must be a string");
  return it->get<std::string>();
}

void validate_rtsp_url(const std::string& url) {
  const std::size_t authority_begin = url.rfind("rtsp://", 0) == 0 ? 7 :
      url.rfind("rtsps://", 0) == 0 ? 8 : std::string::npos;
  if (authority_begin == std::string::npos)
    throw std::invalid_argument("camera.url must use RTSP or RTSPS");
  if (url.empty() || std::any_of(url.begin(), url.end(), [](unsigned char c) {
        return std::isspace(c) || std::iscntrl(c);
      }))
    throw std::invalid_argument("camera.url must not contain whitespace");
  const auto authority_end = url.find_first_of("/?#", authority_begin);
  const std::string authority = url.substr(authority_begin,
      authority_end == std::string::npos ? std::string::npos : authority_end - authority_begin);
  if (authority.empty()) throw std::invalid_argument("camera.url must contain a host");
  if (authority.find('@') != std::string::npos)
    throw std::invalid_argument("camera.url must not contain embedded credentials");

  std::string host;
  std::string port;
  if (authority.front() == '[') {
    const auto close = authority.find(']');
    if (close == std::string::npos || close == 1)
      throw std::invalid_argument("camera.url has an invalid IPv6 host");
    host = authority.substr(1, close - 1);
    const auto suffix = authority.substr(close + 1);
    if (!suffix.empty()) {
      if (suffix.front() != ':') throw std::invalid_argument("camera.url has an invalid host suffix");
      port = suffix.substr(1);
    }
    if (!std::all_of(host.begin(), host.end(), [](unsigned char c) {
          return std::isxdigit(c) || c == ':' || c == '.';
        }))
      throw std::invalid_argument("camera.url has an invalid IPv6 host");
  } else {
    const auto colon = authority.find(':');
    if (colon != std::string::npos) {
      if (authority.find(':', colon + 1) != std::string::npos)
        throw std::invalid_argument("IPv6 camera addresses must be enclosed in brackets");
      host = authority.substr(0, colon);
      port = authority.substr(colon + 1);
    } else {
      host = authority;
    }
    if (host.empty() || !std::all_of(host.begin(), host.end(), [](unsigned char c) {
          return std::isalnum(c) || c == '.' || c == '-' || c == '_';
        }))
      throw std::invalid_argument("camera.url has an invalid host");
  }
  if (!port.empty()) {
    unsigned int value{};
    const auto parsed = std::from_chars(port.data(), port.data() + port.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != port.data() + port.size() ||
        value == 0 || value > 65535)
      throw std::invalid_argument("camera.url has an invalid port");
  } else if (authority.back() == ':') {
    throw std::invalid_argument("camera.url has an empty port");
  }
}

}  // namespace

RuntimeConfig parse_runtime_config(const std::string& json_text) {
  Json root;
  try { root = Json::parse(json_text); }
  catch (const Json::exception& error) {
    throw std::invalid_argument(std::string("Invalid runtime config JSON: ") + error.what());
  }
  keys_only(root, {"revision", "camera", "model", "tracking", "zones", "running", "preview", "storage", "loitering"}, "config");
  RuntimeConfig result;
  if (root.contains("revision")) {
    if (!root["revision"].is_number_integer() || root["revision"].get<int>() < 0)
      throw std::invalid_argument("revision must be a non-negative integer");
    result.revision = root["revision"].get<int>();
  }
  if (root.contains("camera")) {
    const auto& camera = root["camera"];
    keys_only(camera, {"url", "decoder"}, "camera");
    result.camera_url = string_value(camera, "url", result.camera_url);
    result.decoder = string_value(camera, "decoder", result.decoder);
    validate_rtsp_url(result.camera_url);
    if (result.decoder.empty()) throw std::invalid_argument("camera.decoder must not be empty");
    if (result.decoder != "omxh264dec" && result.decoder != "avdec_h264")
      throw std::invalid_argument("camera.decoder must be omxh264dec or avdec_h264");
  }

  if (root.contains("model")) {
    const auto& value = root["model"];
    keys_only(value, {"profile", "score_threshold", "nms_iou", "input_width",
        "input_height", "runtime"}, "model");
    result.model.profile = string_value(value, "profile", result.model.profile);
    if (result.model.profile != "yolov5-a733-640")
      throw std::invalid_argument("model.profile is not available in this runtime");
    if ((value.contains("input_width") && value["input_width"] != 640) ||
        (value.contains("input_height") && value["input_height"] != 640) ||
        (value.contains("runtime") && value["runtime"] != "AWNN/VIPLite"))
      throw std::invalid_argument("Model runtime and input dimensions are fixed by the selected profile");
    if (value.contains("score_threshold"))
      result.model.score_threshold = static_cast<float>(number(value["score_threshold"], "model.score_threshold"));
    if (value.contains("nms_iou"))
      result.model.nms_iou = static_cast<float>(number(value["nms_iou"], "model.nms_iou"));
    if (result.model.score_threshold < 0.0f || result.model.score_threshold > 1.0f ||
        result.model.nms_iou < 0.0f || result.model.nms_iou > 1.0f)
      throw std::invalid_argument("model thresholds must be between 0 and 1");
  }

  if (root.contains("tracking")) {
    const auto& value = root["tracking"];
    keys_only(value, {"low_confidence", "high_confidence", "new_track_confidence",
        "first_match_iou", "second_match_iou", "unconfirmed_match_iou",
        "track_ttl_seconds", "max_predict_seconds", "process_noise",
        "measurement_noise", "min_hits"}, "tracking");
    auto& tracking = result.tracking;
    if (value.contains("low_confidence")) tracking.low_confidence = static_cast<float>(number(value["low_confidence"], "tracking.low_confidence"));
    if (value.contains("high_confidence")) tracking.high_confidence = static_cast<float>(number(value["high_confidence"], "tracking.high_confidence"));
    if (value.contains("new_track_confidence")) tracking.new_track_confidence = static_cast<float>(number(value["new_track_confidence"], "tracking.new_track_confidence"));
    if (value.contains("first_match_iou")) tracking.first_match_iou = static_cast<float>(number(value["first_match_iou"], "tracking.first_match_iou"));
    if (value.contains("second_match_iou")) tracking.second_match_iou = static_cast<float>(number(value["second_match_iou"], "tracking.second_match_iou"));
    if (value.contains("unconfirmed_match_iou")) tracking.unconfirmed_match_iou = static_cast<float>(number(value["unconfirmed_match_iou"], "tracking.unconfirmed_match_iou"));
    if (value.contains("track_ttl_seconds")) tracking.track_ttl_seconds = number(value["track_ttl_seconds"], "tracking.track_ttl_seconds");
    if (value.contains("max_predict_seconds")) tracking.max_predict_seconds = number(value["max_predict_seconds"], "tracking.max_predict_seconds");
    if (value.contains("process_noise")) tracking.process_noise = number(value["process_noise"], "tracking.process_noise");
    if (value.contains("measurement_noise")) tracking.measurement_noise = number(value["measurement_noise"], "tracking.measurement_noise");
    if (value.contains("min_hits")) {
      if (!value["min_hits"].is_number_integer()) throw std::invalid_argument("tracking.min_hits must be an integer");
      tracking.min_hits = value["min_hits"].get<int>();
    }
    const auto unit = [](float x) { return std::isfinite(x) && x >= 0.0f && x <= 1.0f; };
    const auto positive = [](double x) { return std::isfinite(x) && x > 0.0; };
    if (!unit(tracking.low_confidence) || !unit(tracking.high_confidence) ||
        !unit(tracking.new_track_confidence) ||
        tracking.low_confidence > tracking.high_confidence ||
        tracking.high_confidence > tracking.new_track_confidence ||
        !unit(tracking.first_match_iou) || !unit(tracking.second_match_iou) ||
        !unit(tracking.unconfirmed_match_iou) ||
        !positive(tracking.track_ttl_seconds) ||
        !positive(tracking.max_predict_seconds) ||
        !positive(tracking.process_noise) || !positive(tracking.measurement_noise) ||
        tracking.min_hits < 1)
      throw std::invalid_argument("Invalid tracking configuration");
  }

  if (root.contains("zones")) {
    if (!root["zones"].is_array()) throw std::invalid_argument("zones must be an array");
    std::set<std::string> ids;
    for (const auto& item : root["zones"]) {
      keys_only(item, {"id", "name", "enabled", "polygon", "dwell_seconds"}, "zone");
      Zone zone;
      zone.id = string_value(item, "id");
      zone.name = string_value(item, "name");
      zone.enabled = optional_bool(item, "enabled", true);
      if (zone.id.empty() || zone.name.empty() || !ids.insert(zone.id).second)
        throw std::invalid_argument("zone id/name must be non-empty and IDs unique");
      if (!item.contains("polygon") || !item["polygon"].is_array())
        throw std::invalid_argument("zone polygon must be an array");
      for (const auto& point : item["polygon"]) {
        if (!point.is_array() || point.size() != 2)
          throw std::invalid_argument("zone polygon points must be [x,y]");
        zone.polygon.push_back({number(point[0], "polygon x"), number(point[1], "polygon y")});
      }
      zone.dwell_seconds = item.contains("dwell_seconds")
          ? number(item["dwell_seconds"], "dwell_seconds") : zone.dwell_seconds;
      if (zone.enabled && (!valid_polygon(zone.polygon) || zone.dwell_seconds < 0))
        throw std::invalid_argument("enabled zone has invalid normalized polygon or dwell_seconds");
      if (!zone.enabled && !zone.polygon.empty() && !valid_polygon(zone.polygon))
        throw std::invalid_argument("disabled zone polygon is malformed");
      result.zones.push_back(std::move(zone));
    }
  }

  if (root.contains("loitering")) {
    const auto& value = root["loitering"];
    keys_only(value, {"enter_confirm_seconds", "exit_confirm_seconds", "lost_grace_seconds", "max_observation_gap_seconds"}, "loitering");
    if (value.contains("enter_confirm_seconds")) result.loitering.enter_confirm = number(value["enter_confirm_seconds"], "enter_confirm_seconds");
    if (value.contains("exit_confirm_seconds")) result.loitering.exit_confirm = number(value["exit_confirm_seconds"], "exit_confirm_seconds");
    if (value.contains("lost_grace_seconds")) result.loitering.lost_grace = number(value["lost_grace_seconds"], "lost_grace_seconds");
    if (value.contains("max_observation_gap_seconds")) result.loitering.max_gap = number(value["max_observation_gap_seconds"], "max_observation_gap_seconds");
  }

  if (root.contains("running")) {
    const auto& value = root["running"];
    keys_only(value, {"enabled", "mode", "start_threshold", "end_threshold", "window_seconds", "min_observations", "min_span_seconds", "start_confirm_seconds", "end_confirm_seconds", "lost_grace_seconds", "max_gap_seconds"}, "running");
    result.motion.enabled = optional_bool(value, "enabled", false);
    const std::string mode = string_value(value, "mode", "body_heights_per_second");
    if (mode != "body_heights_per_second")
      throw std::invalid_argument("running.mode must be body_heights_per_second until calibrated speed is implemented");
    if (value.contains("start_threshold")) result.motion.start = number(value["start_threshold"], "start_threshold");
    if (value.contains("end_threshold")) result.motion.end = number(value["end_threshold"], "end_threshold");
    if (value.contains("window_seconds")) result.motion.window = number(value["window_seconds"], "window_seconds");
    if (value.contains("min_observations")) {
      const auto& count = value["min_observations"];
      if (!count.is_number_integer()) throw std::invalid_argument("min_observations must be an integer");
      result.motion.min_samples = count.get<int>();
    }
    if (value.contains("min_span_seconds")) result.motion.min_span = number(value["min_span_seconds"], "min_span_seconds");
    if (value.contains("start_confirm_seconds")) result.motion.start_confirm = number(value["start_confirm_seconds"], "start_confirm_seconds");
    if (value.contains("end_confirm_seconds")) result.motion.end_confirm = number(value["end_confirm_seconds"], "end_confirm_seconds");
    if (value.contains("lost_grace_seconds")) result.motion.lost_grace = number(value["lost_grace_seconds"], "lost_grace_seconds");
    if (value.contains("max_gap_seconds")) result.motion.max_gap = number(value["max_gap_seconds"], "max_gap_seconds");
  }
  if (root.contains("preview")) {
    const auto& value = root["preview"];
    keys_only(value, {"fps", "max_width", "quality", "max_clients"}, "preview");
    const double fps = value.contains("fps") ? number(value["fps"], "preview.fps") : result.preview.fps;
    const double width = value.contains("max_width") ? number(value["max_width"], "preview.max_width") : result.preview.max_width;
    const double quality = value.contains("quality") ? number(value["quality"], "preview.quality") : result.preview.quality;
    const double clients = value.contains("max_clients") ? number(value["max_clients"], "preview.max_clients") : result.preview.max_clients;
    if (fps < 1 || fps > 10 || width < 160 || width > 1920 || quality < 20 || quality > 95 || clients < 1 || clients > 8 || std::floor(clients) != clients)
      throw std::invalid_argument("preview settings are outside supported limits");
    if (std::floor(fps) != fps || std::floor(width) != width || std::floor(quality) != quality)
      throw std::invalid_argument("preview fps, max_width and quality must be integers");
    result.preview = {static_cast<int>(fps), static_cast<int>(width),
                      static_cast<int>(quality), static_cast<int>(clients)};
  }
  if (root.contains("storage")) {
    const auto& value = root["storage"];
    keys_only(value, {"retention_days", "max_snapshot_bytes"}, "storage");
    const double days = value.contains("retention_days") ? number(value["retention_days"], "retention_days") : result.storage.retention_days;
    const double bytes = value.contains("max_snapshot_bytes") ? number(value["max_snapshot_bytes"], "max_snapshot_bytes") : result.storage.max_snapshot_bytes;
    if (days < 1 || days > 3650 || bytes < 1048576 || bytes > 1099511627776.0 || std::floor(bytes) != bytes)
      throw std::invalid_argument("storage settings are outside supported limits");
    result.storage = {static_cast<int>(days), static_cast<std::int64_t>(bytes)};
  }
  // Constructors own the canonical threshold validation.
  LoiteringEngine checked_loiter(result.loitering);
  MotionEngine checked_motion(result.motion);
  (void)checked_loiter; (void)checked_motion;
  return result;
}

RuntimeConfig load_runtime_config(const std::string& path) {
  std::ifstream input(path);
  if (!input) throw std::runtime_error("Cannot open runtime config: " + path);
  std::ostringstream contents;
  contents << input.rdbuf();
  return parse_runtime_config(contents.str());
}

std::string serialize_runtime_config(const RuntimeConfig& config) {
  Json zones = Json::array();
  for (const auto& zone : config.zones) {
    Json points = Json::array();
    for (const auto& point : zone.polygon) points.push_back({point.x, point.y});
    zones.push_back({{"id", zone.id}, {"name", zone.name},
        {"enabled", zone.enabled}, {"polygon", points},
        {"dwell_seconds", zone.dwell_seconds}});
  }
  Json root = {
    {"revision", config.revision},
    {"camera", {{"url", config.camera_url}, {"decoder", config.decoder}}},
    {"model", {{"profile", config.model.profile},
      {"score_threshold", config.model.score_threshold},
      {"nms_iou", config.model.nms_iou}}},
    {"tracking", {{"low_confidence", config.tracking.low_confidence},
      {"high_confidence", config.tracking.high_confidence},
      {"new_track_confidence", config.tracking.new_track_confidence},
      {"first_match_iou", config.tracking.first_match_iou},
      {"second_match_iou", config.tracking.second_match_iou},
      {"unconfirmed_match_iou", config.tracking.unconfirmed_match_iou},
      {"track_ttl_seconds", config.tracking.track_ttl_seconds},
      {"max_predict_seconds", config.tracking.max_predict_seconds},
      {"process_noise", config.tracking.process_noise},
      {"measurement_noise", config.tracking.measurement_noise},
      {"min_hits", config.tracking.min_hits}}},
    {"zones", zones},
    {"loitering", {
      {"enter_confirm_seconds", config.loitering.enter_confirm},
      {"exit_confirm_seconds", config.loitering.exit_confirm},
      {"lost_grace_seconds", config.loitering.lost_grace},
      {"max_observation_gap_seconds", config.loitering.max_gap}}},
    {"running", {
      {"enabled", config.motion.enabled},
      {"mode", "body_heights_per_second"},
      {"start_threshold", config.motion.start},
      {"end_threshold", config.motion.end},
      {"window_seconds", config.motion.window},
      {"min_observations", config.motion.min_samples},
      {"min_span_seconds", config.motion.min_span},
      {"start_confirm_seconds", config.motion.start_confirm},
      {"end_confirm_seconds", config.motion.end_confirm},
      {"lost_grace_seconds", config.motion.lost_grace},
      {"max_gap_seconds", config.motion.max_gap}}},
    {"preview", {{"fps", config.preview.fps},
      {"max_width", config.preview.max_width}, {"quality", config.preview.quality},
      {"max_clients", config.preview.max_clients}}},
    {"storage", {{"retention_days", config.storage.retention_days},
      {"max_snapshot_bytes", config.storage.max_snapshot_bytes}}}
  };
  return root.dump(2) + "\n";
}

namespace {
bool write_config_atomically(const std::string& path, const std::string& contents,
                             std::string& error) {
  namespace fs = std::filesystem;
  const fs::path destination(path);
  const fs::path parent = destination.has_parent_path() ? destination.parent_path() : fs::path(".");
  const std::string temporary = path + ".tmp." + std::to_string(::getpid());
  const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0) {
    error = "Cannot create temporary config file: " + std::string(std::strerror(errno));
    return false;
  }
  std::size_t written = 0;
  while (written < contents.size()) {
    const ssize_t count = ::write(fd, contents.data() + written, contents.size() - written);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) {
      error = "Cannot write temporary config file: " + std::string(std::strerror(errno));
      ::close(fd); ::unlink(temporary.c_str()); return false;
    }
    written += static_cast<std::size_t>(count);
  }
  const int sync_result = ::fsync(fd);
  const int close_result = ::close(fd);
  if (sync_result != 0 || close_result != 0) {
    error = "Cannot sync temporary config file";
    ::unlink(temporary.c_str()); return false;
  }
  if (::rename(temporary.c_str(), path.c_str()) != 0) {
    error = "Cannot replace config file: " + std::string(std::strerror(errno));
    ::unlink(temporary.c_str()); return false;
  }
  const int directory_fd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (directory_fd >= 0) { ::fsync(directory_fd); ::close(directory_fd); }
  return true;
}
}  // namespace

RuntimeConfigController::RuntimeConfigController(RuntimeConfig initial, std::string path)
    : config_(std::move(initial)), path_(std::move(path)) {}

RuntimeConfig RuntimeConfigController::current() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return config_;
}

RuntimeConfigUpdateResult RuntimeConfigController::update_camera_url(
    const std::string& url, int expected_revision) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (expected_revision != config_.revision)
    return {409, config_, "Configuration revision conflict; reload the current settings"};
  if (config_.revision == std::numeric_limits<int>::max())
    return {409, config_, "Configuration revision limit reached; restart the core"};
  RuntimeConfig candidate = config_;
  candidate.camera_url = url;
  ++candidate.revision;
  try {
    candidate = parse_runtime_config(serialize_runtime_config(candidate));
  } catch (const std::exception& error) {
    return {422, config_, error.what()};
  }
  std::string error;
  if (!write_config_atomically(path_, serialize_runtime_config(candidate), error))
    return {503, config_, error};
  config_ = candidate;
  return {202, config_, {}};
}

RuntimeConfigUpdateResult RuntimeConfigController::update_behavior_config(
    const std::string& json_text, int expected_revision) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (expected_revision != config_.revision)
    return {409, config_, "Configuration revision conflict; reload the current settings"};
  if (config_.revision == std::numeric_limits<int>::max())
    return {409, config_, "Configuration revision limit reached; restart the core"};
  try {
    Json root = Json::parse(json_text);
    if (!root.is_object() || !root.contains("revision") ||
        !root["revision"].is_number_integer() ||
        root["revision"].get<int>() != expected_revision)
      return {409, config_, "Configuration revision conflict; reload the current settings"};
    root["revision"] = config_.revision + 1;
    RuntimeConfig candidate = parse_runtime_config(root.dump());
    std::string error;
    if (!write_config_atomically(path_, serialize_runtime_config(candidate), error))
      return {503, config_, error};
    config_ = std::move(candidate);
    return {202, config_, {}};
  } catch (const std::exception& error) {
    return {422, config_, error.what()};
  }
}

}  // namespace aibox

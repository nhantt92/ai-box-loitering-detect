#include "aibox/core_api.hpp"

#include <nlohmann/json.hpp>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <random>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace aibox {
namespace {
using Json = nlohmann::json;

std::string trim(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

std::string lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

bool constant_time_equal(const std::string& a, const std::string& b) {
  std::size_t diff = a.size() ^ b.size();
  const std::size_t length = std::max(a.size(), b.size());
  for (std::size_t i = 0; i < length; ++i) {
    const unsigned char x = i < a.size() ? static_cast<unsigned char>(a[i]) : 0;
    const unsigned char y = i < b.size() ? static_cast<unsigned char>(b[i]) : 0;
    diff |= static_cast<std::size_t>(x ^ y);
  }
  return diff == 0;
}

std::string token_for(const std::string& directory) {
  std::filesystem::create_directories(directory);
  const std::string path = (std::filesystem::path(directory) / "core-api.token").string();
  auto read_existing = [&] {
    std::ifstream input(path);
    std::string value;
    std::getline(input, value);
    if (!input && value.empty()) throw std::runtime_error("Cannot read core API token file");
    if (chmod(path.c_str(), S_IRUSR | S_IWUSR) != 0)
      throw std::runtime_error("Cannot restrict core API token permissions");
    return trim(value);
  };
  if (std::filesystem::exists(path)) {
    auto value = read_existing();
    if (value.size() < 32) throw std::runtime_error("Core API token is invalid");
    return value;
  }
  std::random_device random;
  std::array<unsigned char, 32> bytes{};
  for (auto& byte : bytes) byte = static_cast<unsigned char>(random());
  std::ostringstream encoded;
  for (auto byte : bytes) encoded << std::hex << std::setw(2) << std::setfill('0')
                                 << static_cast<int>(byte);
  const std::string value = encoded.str() + '\n';
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
  if (fd < 0) {
    if (errno == EEXIST) return read_existing();
    throw std::runtime_error("Cannot create core API token file");
  }
  std::size_t written = 0;
  while (written < value.size()) {
    const auto count = ::write(fd, value.data() + written, value.size() - written);
    if (count <= 0) {
      ::close(fd);
      throw std::runtime_error("Cannot write core API token file");
    }
    written += static_cast<std::size_t>(count);
  }
  const int sync_result = ::fsync(fd);
  const int close_result = ::close(fd);
  if (sync_result != 0 || close_result != 0)
    throw std::runtime_error("Cannot sync core API token file");
  return trim(value);
}

bool send_all(int fd, const void* raw_data, std::size_t size) {
  const auto* data = static_cast<const char*>(raw_data);
  std::size_t sent = 0;
  while (sent < size) {
    const ssize_t count = ::send(fd, data + sent, size - sent, MSG_NOSIGNAL);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) return false;
    sent += static_cast<std::size_t>(count);
  }
  return true;
}

bool send_all(int fd, const std::string& value) {
  return send_all(fd, value.data(), value.size());
}

void response(int fd, int code, const std::string& reason,
              const std::string& content_type, const std::string& body,
              const std::string& extra_headers = {}) {
  std::ostringstream header;
  header << "HTTP/1.1 " << code << ' ' << reason << "\r\n"
         << "Content-Type: " << content_type << "\r\n"
         << "Content-Length: " << body.size() << "\r\n"
         << "Cache-Control: no-store\r\nConnection: close\r\n"
         << extra_headers << "\r\n";
  send_all(fd, header.str());
  send_all(fd, body);
}

bool safe_event_id(const std::string& id) {
  if (id.empty() || id.size() > 64) return false;
  return std::all_of(id.begin(), id.end(), [](unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || c == '-';
  });
}

std::string query_value(const std::string& path, const std::string& key,
                        const std::string& fallback) {
  const auto q = path.find('?');
  if (q == std::string::npos) return fallback;
  std::size_t begin = q + 1;
  while (begin < path.size()) {
    const auto end = path.find('&', begin);
    const std::string item = path.substr(begin,
        end == std::string::npos ? std::string::npos : end - begin);
    const auto equals = item.find('=');
    if (item.substr(0, equals) == key)
      return equals == std::string::npos ? std::string{} : item.substr(equals + 1);
    if (end == std::string::npos) break;
    begin = end + 1;
  }
  return fallback;
}

std::size_t bounded_int(const std::string& value, std::size_t fallback,
                        std::size_t maximum) {
  if (value.empty()) return fallback;
  std::size_t result{};
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
  if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
      result > maximum)
    throw std::invalid_argument("Invalid pagination query");
  return result;
}

Json event_json(const StoredEvent& event) {
  return {{"id", event.id}, {"session_id", event.session_id},
      {"track_id", event.track_id}, {"type", event.type},
      {"zone_id", event.zone_id}, {"started_utc", event.started_utc},
      {"ended_utc", event.ended_utc.empty() ? Json(nullptr) : Json(event.ended_utc)},
      {"duration_seconds", event.duration_seconds},
      {"dwell_seconds", event.dwell_seconds}, {"peak_speed", event.peak_speed},
      {"crowd_count", event.crowd_count},
      {"crowd_threshold", event.crowd_threshold},
      {"speed_unit", event.speed_unit}, {"end_reason", event.end_reason},
      {"active", event.active}, {"snapshot_state", event.snapshot_state},
      {"snapshot_url", event.snapshot_state == "ready"
          ? Json("/api/events/" + event.id + "/snapshot") : Json(nullptr)}};
}

std::string csv_escape(const std::string& value) {
  std::string out = "\"";
  for (char c : value) { if (c == '"') out += '"'; out += c; }
  out += '"';
  return out;
}

}  // namespace

struct CoreApiServer::Impl {
  struct Client {
    int fd{-1};
    bool done{};
    std::thread thread;
  };

  Impl(PreviewPublisher& publisher,
       std::shared_ptr<RuntimeConfigController> runtime_config,
       std::shared_ptr<EventStore> event_store,
       std::function<CoreStatus()> status, std::string directory,
       std::uint16_t requested_port)
      : preview(publisher), config(std::move(runtime_config)),
        events(std::move(event_store)), status_provider(std::move(status)),
        data_directory(std::move(directory)), token(token_for(data_directory)),
        token_path((std::filesystem::path(data_directory) / "core-api.token").string()) {
    listener = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) throw std::runtime_error("Cannot create core API socket");
    int reuse = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(requested_port);
    if (::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        ::listen(listener, 16) != 0) {
      const std::string error = std::strerror(errno);
      ::close(listener); listener = -1;
      throw std::runtime_error("Cannot bind core API to loopback: " + error);
    }
    socklen_t address_size = sizeof(address);
    if (getsockname(listener, reinterpret_cast<sockaddr*>(&address), &address_size) == 0)
      bound_port = ntohs(address.sin_port);
    accept_thread = std::thread([this] { accept_loop(); });
  }

  ~Impl() {
    stopping = true;
    if (listener >= 0) ::shutdown(listener, SHUT_RDWR);
    if (accept_thread.joinable()) accept_thread.join();
    {
      std::lock_guard<std::mutex> lock(clients_mutex);
      for (const auto& client : clients)
        if (client->fd >= 0) ::shutdown(client->fd, SHUT_RDWR);
    }
    for (auto& client : clients)
      if (client->thread.joinable()) client->thread.join();
    if (listener >= 0) ::close(listener);
  }

  Json config_json() const {
    const RuntimeConfig current = config->current();
    Json zones = Json::array();
    for (const auto& zone : current.zones) {
      Json polygon = Json::array();
      for (const auto& point : zone.polygon) polygon.push_back({point.x, point.y});
      zones.push_back({{"id", zone.id}, {"name", zone.name},
          {"enabled", zone.enabled}, {"polygon", polygon},
          {"dwell_seconds", zone.dwell_seconds}});
    }
    return {{"revision", current.revision},
      {"camera", {{"source_type", current.camera_source_type},
          {"url", current.camera_url}, {"decoder", current.decoder},
          {"username", current.camera_username},
          {"password_configured", !current.camera_password.empty()}}},
      {"model", {{"profile", current.model.profile},
          {"score_threshold", current.model.score_threshold},
          {"nms_iou", current.model.nms_iou}, {"input_width", 640},
          {"input_height", 640}, {"runtime", "AWNN/VIPLite"},
          {"classes", current.model.classes}}},
      {"tracking", {{"low_confidence", current.tracking.low_confidence},
          {"high_confidence", current.tracking.high_confidence},
          {"new_track_confidence", current.tracking.new_track_confidence},
          {"first_match_iou", current.tracking.first_match_iou},
          {"second_match_iou", current.tracking.second_match_iou},
          {"unconfirmed_match_iou", current.tracking.unconfirmed_match_iou},
          {"track_ttl_seconds", current.tracking.track_ttl_seconds},
          {"max_predict_seconds", current.tracking.max_predict_seconds},
          {"process_noise", current.tracking.process_noise},
          {"measurement_noise", current.tracking.measurement_noise},
          {"min_hits", current.tracking.min_hits}}},
      {"zones", zones},
      {"loitering", {{"enter_confirm_seconds", current.loitering.enter_confirm},
          {"exit_confirm_seconds", current.loitering.exit_confirm},
          {"lost_grace_seconds", current.loitering.lost_grace},
          {"max_observation_gap_seconds", current.loitering.max_gap}}},
      {"running", {{"enabled", current.motion.enabled},
          {"mode", "body_heights_per_second"},
          {"start_threshold", current.motion.start}, {"end_threshold", current.motion.end},
          {"window_seconds", current.motion.window},
          {"min_observations", current.motion.min_samples},
          {"min_span_seconds", current.motion.min_span},
          {"start_confirm_seconds", current.motion.start_confirm},
          {"end_confirm_seconds", current.motion.end_confirm},
          {"lost_grace_seconds", current.motion.lost_grace},
          {"max_gap_seconds", current.motion.max_gap}}},
      {"running_detection", {{"enabled", current.running_detection.enabled},
          {"mode", "body_heights_per_second"},
          {"start_threshold", current.running_detection.start},
          {"end_threshold", current.running_detection.end},
          {"window_seconds", current.running_detection.window},
          {"min_observations", current.running_detection.min_samples},
          {"min_span_seconds", current.running_detection.min_span},
          {"start_confirm_seconds", current.running_detection.start_confirm},
          {"end_confirm_seconds", current.running_detection.end_confirm},
          {"lost_grace_seconds", current.running_detection.lost_grace},
          {"max_gap_seconds", current.running_detection.max_gap}}},
      {"crowd", {{"enabled", current.crowd.enabled},
          {"trigger_people", current.crowd.trigger_people},
          {"clear_people", current.crowd.clear_people},
          {"enter_confirm_seconds", current.crowd.enter_confirm},
          {"exit_confirm_seconds", current.crowd.exit_confirm},
          {"max_observation_gap_seconds", current.crowd.max_gap}}},
      {"preview", {{"fps", current.preview.fps}, {"max_width", current.preview.max_width},
          {"quality", current.preview.quality}, {"max_clients", current.preview.max_clients}}},
      {"storage", {{"retention_days", current.storage.retention_days},
          {"max_snapshot_bytes", current.storage.max_snapshot_bytes}}}};
  }

  bool authorize(const std::string& header) const {
    constexpr const char* prefix = "Bearer ";
    return header.rfind(prefix, 0) == 0 &&
           constant_time_equal(header.substr(7), token);
  }

  bool read_request(int fd, std::string& method, std::string& path,
                    std::string& authorization, std::string& body) {
    std::string input;
    std::array<char, 1024> buffer{};
    while (input.size() < 8192 && input.find("\r\n\r\n") == std::string::npos) {
      const ssize_t count = ::recv(fd, buffer.data(), buffer.size(), 0);
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) return false;
      input.append(buffer.data(), static_cast<std::size_t>(count));
    }
    const auto end = input.find("\r\n");
    if (end == std::string::npos) return false;
    std::istringstream first(input.substr(0, end));
    std::string version;
    first >> method >> path >> version;
    if (method.empty() || path.empty() || version.rfind("HTTP/1.", 0) != 0) return false;
    std::size_t content_length = 0;
    bool has_content_length = false;
    std::size_t cursor = end + 2;
    while (cursor < input.size()) {
      const auto next = input.find("\r\n", cursor);
      if (next == std::string::npos || next == cursor) break;
      const auto colon = input.find(':', cursor);
      if (colon != std::string::npos && colon < next) {
        const auto name = lower(trim(input.substr(cursor, colon - cursor)));
        const auto value = trim(input.substr(colon + 1, next - colon - 1));
        if (name == "authorization") authorization = value;
        else if (name == "content-length") {
          const auto parsed = std::from_chars(value.data(), value.data() + value.size(), content_length);
          if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || content_length > 8192)
            return false;
          has_content_length = true;
        } else if (name == "transfer-encoding") return false;
      }
      cursor = next + 2;
    }
    const auto header_end = input.find("\r\n\r\n");
    if (header_end == std::string::npos) return false;
    if (!has_content_length) content_length = 0;
    body = input.substr(header_end + 4);
    if (body.size() > content_length) body.resize(content_length);
    while (body.size() < content_length) {
      const ssize_t count = ::recv(fd, buffer.data(),
          std::min(buffer.size(), content_length - body.size()), 0);
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) return false;
      body.append(buffer.data(), static_cast<std::size_t>(count));
    }
    return true;
  }

  void send_json(int fd, int code, const Json& value) {
    const char* reason = code == 200 ? "OK" : code == 202 ? "Accepted" :
        code == 400 ? "Bad Request" : code == 401 ? "Unauthorized" :
        code == 404 ? "Not Found" : code == 405 ? "Method Not Allowed" :
        code == 409 ? "Conflict" : code == 422 ? "Unprocessable Entity" :
        code == 503 ? "Service Unavailable" : "Error";
    response(fd, code, reason, "application/json",
             value.dump());
  }

  void handle_stream(int fd) {
    int current = stream_clients.load();
    do {
      if (current >= preview.max_clients()) {
        send_json(fd, 503, {{"error", "preview client limit reached"}});
        return;
      }
    } while (!stream_clients.compare_exchange_weak(current, current + 1));
    struct Release { std::atomic<int>& count; ~Release() { --count; } } release{stream_clients};
    if (!send_all(fd, "HTTP/1.1 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary=frame\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n")) return;
    std::uint64_t sequence = 0;
    while (!stopping) {
      auto jpeg = preview.wait_after(sequence, std::chrono::seconds(1));
      if (!jpeg) continue;
      sequence = jpeg->sequence;
      const auto status = status_provider();
      const auto age = std::chrono::duration<double>(
          std::chrono::steady_clock::now() - jpeg->encoded_at).count();
      if (!status.camera_connected || age > 2.0) continue;
      std::ostringstream part;
      part << "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: "
           << jpeg->bytes->size() << "\r\nX-Sequence: " << jpeg->sequence << "\r\n\r\n";
      const auto header = part.str();
      if (!send_all(fd, header) || !send_all(fd, jpeg->bytes->data(), jpeg->bytes->size()) ||
          !send_all(fd, "\r\n")) return;
    }
  }

  void handle(int fd) {
    std::string method, path, authorization, body;
    if (!read_request(fd, method, path, authorization, body)) {
      response(fd, 400, "Bad Request", "application/json", "{\"error\":\"invalid request\"}");
      return;
    }
    if (method != "GET" && method != "PUT") {
      send_json(fd, 405, {{"error", "method not allowed"}});
      return;
    }
    if (!authorize(authorization)) {
      response(fd, 401, "Unauthorized", "application/json", "{\"error\":\"unauthorized\"}",
               "WWW-Authenticate: Bearer\r\n");
      return;
    }
    try {
      const auto query = path.find('?');
      const std::string route = path.substr(0, query);
      if (method == "PUT" && route == "/api/config") {
        Json request_json;
        try { request_json = Json::parse(body); }
        catch (const Json::exception&) { throw std::invalid_argument("Invalid JSON body"); }
        if (!request_json.is_object() || !request_json.contains("revision") ||
            !request_json["revision"].is_number_integer())
          throw std::invalid_argument("Expected a full config object with revision");
        const auto result = config->update_behavior_config(
            body, request_json["revision"].get<int>());
        if (result.status != 202) {
          send_json(fd, result.status, {{"error", result.error},
              {"revision", result.config.revision}});
          return;
        }
        send_json(fd, 202, {{"revision", result.config.revision},
            {"state", "applying"}});
        return;
      }
      if (method == "PUT" && route == "/api/config/camera") {
        Json request_json;
        try { request_json = Json::parse(body); }
        catch (const Json::exception&) { throw std::invalid_argument("Invalid JSON body"); }
        if (!request_json.is_object() ||
            !request_json.contains("revision") || !request_json["revision"].is_number_integer() ||
            !request_json.contains("camera") || !request_json["camera"].is_object())
          throw std::invalid_argument("Expected revision and camera settings");
        const auto& camera = request_json["camera"];
        const auto get_string = [&](const char* key, const std::string& fallback) {
          if (!camera.contains(key)) return fallback;
          if (!camera[key].is_string()) throw std::invalid_argument(std::string("camera.") + key + " must be a string");
          return camera[key].get<std::string>();
        };
        const auto current = config->current();
        std::optional<std::string> password;
        if (camera.contains("password")) {
          if (camera["password"].is_null()) password = std::string{};
          else if (camera["password"].is_string()) password = camera["password"].get<std::string>();
          else throw std::invalid_argument("camera.password must be a string or null");
        }
        const auto result = config->update_camera(
            get_string("source_type", current.camera_source_type),
            get_string("url", current.camera_url),
            get_string("decoder", current.decoder),
            get_string("username", current.camera_username), password,
            request_json["revision"].get<int>());
        if (result.status != 202) {
          send_json(fd, result.status, {{"error", result.error},
              {"revision", result.config.revision}});
          return;
        }
        send_json(fd, 202, {{"revision", result.config.revision},
            {"camera", {{"source_type", result.config.camera_source_type},
                {"url", result.config.camera_url}, {"decoder", result.config.decoder},
                {"username", result.config.camera_username},
                {"password_configured", !result.config.camera_password.empty()}}},
            {"state", "reconnecting"}});
        return;
      }
      if (method != "GET") {
        send_json(fd, 405, {{"error", "method not allowed"}});
        return;
      }
      if (route == "/api/status") {
        const auto status = status_provider();
        const auto current = config->current();
        Json loitering = Json::array();
        for (const auto& signal : status.loitering) {
          loitering.push_back({{"track_id", signal.track_id},
              {"zone_id", signal.zone_id}, {"zone_name", signal.zone_name},
              {"observed", signal.observed}, {"inside", signal.inside},
              {"active", signal.active}, {"dwell_seconds", signal.dwell_seconds},
              {"target_seconds", signal.target_seconds}});
        }
        Json crowd = Json::array();
        for (const auto& signal : status.crowd) {
          crowd.push_back({{"zone_id", signal.zone_id},
              {"zone_name", signal.zone_name}, {"active", signal.active},
              {"count", signal.count}, {"threshold", signal.threshold}});
        }
        send_json(fd, 200, {{"core", "online"},
            {"camera", status.camera_connected ? "connected" : "disconnected"},
            {"overlay_stale", status.overlay_stale},
            {"revision", current.revision}, {"ai_fps", status.ai_fps},
            {"inference_ms", status.inference_ms}, {"frame_id", status.frame_id},
            {"session_id", status.session_id}, {"message", status.message},
            {"storage", status.storage_state},
            {"person_detections", status.person_detections},
            {"person_tracks", status.person_tracks},
            {"enabled_zones", status.enabled_zones}, {"loitering", loitering},
            {"crowd", crowd},
            {"preview_sequence", preview.latest() ? preview.latest()->sequence : 0}});
        return;
      }
      if (route == "/api/config") { send_json(fd, 200, config_json()); return; }
      if (route == "/api/events") {
        const auto offset = bounded_int(query_value(path, "offset", "0"), 0, 1000000);
        const auto limit = bounded_int(query_value(path, "limit", "50"), 50, 200);
        const auto rows = events ? events->recent(offset, limit) : std::vector<StoredEvent>{};
        Json items = Json::array();
        for (const auto& row : rows) items.push_back(event_json(row));
        send_json(fd, 200, {{"items", items}, {"offset", offset}, {"limit", limit}});
        return;
      }
      if (route == "/api/events.csv") {
        const auto limit = bounded_int(query_value(path, "limit", "200"), 200, 1000);
        std::string csv = "id,type,zone_id,started_utc,ended_utc,duration_seconds,end_reason,crowd_count,crowd_threshold\r\n";
        for (std::size_t offset = 0; events && offset < limit; offset += 200) {
          const auto rows = events->recent(offset, std::min<std::size_t>(200, limit - offset));
          for (const auto& row : rows) {
            csv += csv_escape(row.id) + ',' + csv_escape(row.type) + ',' +
                csv_escape(row.zone_id) + ',' + csv_escape(row.started_utc) + ',' +
                csv_escape(row.ended_utc) + ',' + std::to_string(row.duration_seconds) + ',' +
                csv_escape(row.end_reason) + ',' + std::to_string(row.crowd_count) + ',' +
                std::to_string(row.crowd_threshold) + "\r\n";
          }
          if (rows.size() < std::min<std::size_t>(200, limit - offset)) break;
        }
        response(fd, 200, "OK", "text/csv; charset=utf-8", csv);
        return;
      }
      if (route == "/api/frame.jpg") {
        const auto status = status_provider();
        const auto jpeg = preview.latest();
        if (!jpeg || !jpeg->bytes || !status.camera_connected ||
            std::chrono::steady_clock::now() - jpeg->encoded_at > std::chrono::seconds(2)) {
          send_json(fd, 503, {{"error", "fresh preview unavailable"}});
          return;
        }
        std::ostringstream header;
        header << "HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\n"
               << "Content-Length: " << jpeg->bytes->size() << "\r\n"
               << "Cache-Control: no-store\r\nConnection: close\r\n"
               << "X-Sequence: " << jpeg->sequence << "\r\n\r\n";
        send_all(fd, header.str());
        send_all(fd, jpeg->bytes->data(), jpeg->bytes->size());
        return;
      }
      if (route == "/api/stream.mjpg") { handle_stream(fd); return; }
      if (route.rfind("/api/events/", 0) == 0 && route.size() > 12 &&
          route.compare(route.size() - 9, 9, "/snapshot") == 0) {
        const std::string event_id = route.substr(12, route.size() - 12 - 9);
        if (!safe_event_id(event_id) || !events) {
          send_json(fd, 404, {{"error", "event snapshot not found"}});
          return;
        }
        const auto event = events->get(event_id);
        if (!event) {
          send_json(fd, 404, {{"error", "event snapshot not found"}});
          return;
        }
        if (event->snapshot_state != "ready" ||
            event->snapshot_path != "snapshots/" + event_id + ".jpg") {
          send_json(fd, 404, {{"error", "event snapshot is not available"}});
          return;
        }
        const auto directory = (std::filesystem::path(data_directory) / "snapshots").string();
        const int directory_fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (directory_fd < 0) {
          send_json(fd, 404, {{"error", "event snapshot not found"}});
          return;
        }
        struct stat directory_info{};
        if (::fstat(directory_fd, &directory_info) != 0 ||
            !S_ISDIR(directory_info.st_mode) || directory_info.st_uid != ::geteuid() ||
            (directory_info.st_mode & 077) != 0) {
          ::close(directory_fd);
          send_json(fd, 404, {{"error", "event snapshot not found"}});
          return;
        }
        const std::string filename = event_id + ".jpg";
        const int snapshot_fd = ::openat(directory_fd, filename.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        ::close(directory_fd);
        if (snapshot_fd < 0) {
          send_json(fd, 404, {{"error", "event snapshot not found"}});
          return;
        }
        struct stat snapshot_info{};
        if (::fstat(snapshot_fd, &snapshot_info) != 0 || !S_ISREG(snapshot_info.st_mode) ||
            snapshot_info.st_uid != ::geteuid() || (snapshot_info.st_mode & 077) != 0 ||
            snapshot_info.st_size <= 0 || snapshot_info.st_size > 32 * 1024 * 1024) {
          ::close(snapshot_fd);
          send_json(fd, 404, {{"error", "event snapshot is invalid"}});
          return;
        }
        std::ostringstream header;
        header << "HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\n"
               << "Content-Length: " << snapshot_info.st_size << "\r\n"
               << "Cache-Control: private, no-store\r\nConnection: close\r\n\r\n";
        bool ok = send_all(fd, header.str());
        std::array<char, 16384> buffer{};
        while (ok) {
          const auto count = ::read(snapshot_fd, buffer.data(), buffer.size());
          if (count < 0 && errno == EINTR) continue;
          if (count <= 0) break;
          ok = send_all(fd, buffer.data(), static_cast<std::size_t>(count));
        }
        ::close(snapshot_fd);
        return;
      }
      send_json(fd, 404, {{"error", "not found"}});
    } catch (const std::invalid_argument& error) {
      send_json(fd, 422, {{"error", error.what()}});
    } catch (const std::exception& error) {
      send_json(fd, 503, {{"error", error.what()}});
    }
  }

  void accept_loop() {
    while (!stopping) {
      const int fd = ::accept(listener, nullptr, nullptr);
      if (fd < 0) {
        if (errno == EINTR) continue;
        if (stopping) break;
        std::lock_guard<std::mutex> lock(error_mutex);
        error = std::strerror(errno);
        continue;
      }
      timeval timeout{2, 0};
      setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
      setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
      std::lock_guard<std::mutex> lock(clients_mutex);
      for (auto it = clients.begin(); it != clients.end();) {
        if ((*it)->done) {
          if ((*it)->thread.joinable()) (*it)->thread.join();
          it = clients.erase(it);
        } else ++it;
      }
      if (clients.size() >= 16) {
        send_json(fd, 503, {{"error", "core API connection limit reached"}});
        ::close(fd);
        continue;
      }
      auto client = std::make_unique<Client>();
      client->fd = fd;
      Client* raw = client.get();
      client->thread = std::thread([this, raw] {
        handle(raw->fd);
        std::lock_guard<std::mutex> client_lock(clients_mutex);
        if (raw->fd >= 0) ::close(raw->fd);
        raw->fd = -1;
        raw->done = true;
      });
      clients.push_back(std::move(client));
    }
  }

  PreviewPublisher& preview;
  std::shared_ptr<RuntimeConfigController> config;
  std::shared_ptr<EventStore> events;
  std::function<CoreStatus()> status_provider;
  std::string data_directory;
  std::string token, token_path;
  int listener{-1};
  std::uint16_t bound_port{};
  std::atomic<bool> stopping{false};
  std::atomic<int> stream_clients{0};
  std::thread accept_thread;
  std::mutex clients_mutex, error_mutex;
  std::vector<std::unique_ptr<Client>> clients;
  std::string error;
};

CoreApiServer::CoreApiServer(PreviewPublisher& preview,
    std::shared_ptr<RuntimeConfigController> config,
    std::shared_ptr<EventStore> events, std::function<CoreStatus()> status_provider,
    std::string data_directory, std::uint16_t port)
    : impl_(std::make_unique<Impl>(preview, std::move(config), std::move(events),
          std::move(status_provider), std::move(data_directory), port)) {}
CoreApiServer::~CoreApiServer() = default;
bool CoreApiServer::available() const { return impl_ && impl_->listener >= 0; }
std::string CoreApiServer::last_error() const {
  std::lock_guard<std::mutex> lock(impl_->error_mutex);
  return impl_->error;
}
std::string CoreApiServer::token_file() const { return impl_->token_path; }
std::uint16_t CoreApiServer::port() const { return impl_->bound_port; }

}  // namespace aibox

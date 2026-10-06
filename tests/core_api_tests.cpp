#include "aibox/core_api.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/stat.h>

#include <opencv2/imgcodecs.hpp>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace aibox;

namespace {
std::string request(std::uint16_t port, const std::string& path,
                    const std::string& token = {}, const std::string& method = "GET",
                    const std::string& body = {}) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  assert(fd >= 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(port);
  assert(::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
  std::string raw = method + " " + path + " HTTP/1.1\r\nHost: localhost\r\n";
  if (!token.empty()) raw += "Authorization: Bearer " + token + "\r\n";
  if (!body.empty()) raw += "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
  raw += "Connection: close\r\n\r\n";
  raw += body;
  assert(::send(fd, raw.data(), raw.size(), 0) == static_cast<ssize_t>(raw.size()));
  std::string output;
  char buffer[4096];
  for (;;) {
    const auto count = ::recv(fd, buffer, sizeof(buffer), 0);
    if (count <= 0) break;
    output.append(buffer, static_cast<std::size_t>(count));
  }
  ::close(fd);
  return output;
}

void core_api_authenticates_and_serves_cached_preview_and_events() {
  const auto root = std::filesystem::temp_directory_path() /
      ("aibox-core-api-" + std::to_string(::getpid()));
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  {
  auto events = std::make_shared<EventStore>((root / "events.sqlite3").string());
  PreviewConfig preview_config;
  PreviewPublisher preview(preview_config);
  RuntimeConfig config;
  config.revision = 4;
  const auto config_path = (root / "config.json").string();
  { std::ofstream output(config_path); output << serialize_runtime_config(config); }
  auto config_controller = std::make_shared<RuntimeConfigController>(config, config_path);
  CoreStatus status;
  status.camera_connected = true;
  status.ai_fps = 5.0;
  status.frame_id = 23;
  status.session_id = "test-session";
  CoreApiServer api(preview, config_controller, events, [&] { return status; }, root.string(), 0);
  std::ifstream token_input(api.token_file());
  std::string token;
  std::getline(token_input, token);
  assert(token.size() >= 32);
  assert((std::filesystem::status(api.token_file()).permissions() &
          std::filesystem::perms::group_all) == std::filesystem::perms::none);

  const auto unauthorized = request(api.port(), "/api/status");
  assert(unauthorized.find("401 Unauthorized") != std::string::npos);
  const auto status_response = request(api.port(), "/api/status", token);
  assert(status_response.find("200 OK") != std::string::npos);
  assert(status_response.find("\"camera\":\"connected\"") != std::string::npos);
  assert(status_response.find("test-session") != std::string::npos);
  const auto config_response = request(api.port(), "/api/config", token);
  assert(config_response.find("\"revision\":4") != std::string::npos);
  assert(config_response.find("rtsp://192.168.1.6:554/profile2") != std::string::npos);
  const std::string update_body = R"({"revision":4,"camera":{"url":"rtsp://192.168.1.77:554/profile2"}})";
  const auto update_response = request(api.port(), "/api/config/camera", token, "PUT", update_body);
  assert(update_response.find("202 Accepted") != std::string::npos);
  assert(config_controller->current().revision == 5);
  assert(config_controller->current().camera_url == "rtsp://192.168.1.77:554/profile2");
  const auto persisted = load_runtime_config(config_path);
  assert(persisted.revision == 5 && persisted.camera_url == "rtsp://192.168.1.77:554/profile2");
  const std::string behavior_body = R"({"revision":5,"camera":{"url":"rtsp://192.168.1.77:554/profile2","decoder":"omxh264dec"},"zones":[{"id":"entry","name":"Entry","enabled":true,"polygon":[[0.1,0.1],[0.9,0.1],[0.9,0.9],[0.1,0.9]],"dwell_seconds":30}],"running":{"enabled":true,"mode":"body_heights_per_second","start_threshold":2.5,"end_threshold":1.8}})";
  const auto behavior_response = request(api.port(), "/api/config", token, "PUT", behavior_body);
  assert(behavior_response.find("202 Accepted") != std::string::npos);
  assert(config_controller->current().revision == 6);
  assert(config_controller->current().zones.size() == 1);
  assert(config_controller->current().motion.enabled);
  const auto behavior_persisted = load_runtime_config(config_path);
  assert(behavior_persisted.zones.size() == 1 && behavior_persisted.zones[0].id == "entry");
  const auto behavior_conflict = request(api.port(), "/api/config", token, "PUT", behavior_body);
  assert(behavior_conflict.find("409 Conflict") != std::string::npos);
  const auto conflict = request(api.port(), "/api/config/camera", token, "PUT", update_body);
  assert(conflict.find("409 Conflict") != std::string::npos);
  const std::string invalid_body = R"({"revision":6,"camera":{"url":"https://192.168.1.77/live"}})";
  const auto invalid = request(api.port(), "/api/config/camera", token, "PUT", invalid_body);
  assert(invalid.find("422 Unprocessable Entity") != std::string::npos);
  assert(config_controller->current().revision == 6);
  const auto events_response = request(api.port(), "/api/events?offset=0&limit=20", token);
  assert(events_response.find("\"items\":[]") != std::string::npos);
  assert(request(api.port(), "/api/events?limit=999", token)
             .find("422 Unprocessable Entity") != std::string::npos);

  auto frame = std::make_shared<AnalysisFrame>();
  frame->bgr = cv::Mat(100, 200, CV_8UC3, cv::Scalar(12, 34, 56));
  preview.submit(frame);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  std::shared_ptr<const JpegFrame> jpeg;
  while (!(jpeg = preview.latest()) && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  assert(jpeg);
  const auto jpg_response = request(api.port(), "/api/frame.jpg", token);
  assert(jpg_response.find("200 OK") != std::string::npos);
  assert(jpg_response.find("Content-Type: image/jpeg") != std::string::npos);
  assert(jpg_response.size() > jpeg->bytes->size());

  const std::string event_id = "0123456789abcdef0123456789abcdef-1";
  Event event;
  event.id = event_id;
  event.session_id = "0123456789abcdef0123456789abcdef";
  event.track_id = 9;
  event.type = "loitering";
  event.zone_id = "door";
  event.started = std::chrono::steady_clock::now();
  event.ended = event.started;
  event.started_utc = std::chrono::system_clock::now();
  event.ended_utc = event.started_utc;
  assert(events->enqueue({event, true}));
  events->flush();
  std::filesystem::create_directories(root / "snapshots");
  assert(::chmod((root / "snapshots").c_str(), 0700) == 0);
  std::vector<std::uint8_t> snapshot_bytes;
  assert(cv::imencode(".jpg", frame->bgr, snapshot_bytes));
  {
    std::ofstream output(root / "snapshots" / (event_id + ".jpg"), std::ios::binary);
    output.write(reinterpret_cast<const char*>(snapshot_bytes.data()),
                  static_cast<std::streamsize>(snapshot_bytes.size()));
  }
  assert(::chmod((root / "snapshots" / (event_id + ".jpg")).c_str(), 0600) == 0);
  assert(events->enqueue_snapshot_state(event_id, "ready", "snapshots/" + event_id + ".jpg"));
  events->flush();
  const auto snapshot_response = request(api.port(),
      "/api/events/" + event_id + "/snapshot", token);
  const auto body_start = snapshot_response.find("\r\n\r\n");
  assert(snapshot_response.find("200 OK") != std::string::npos);
  assert(snapshot_response.find("Content-Type: image/jpeg") != std::string::npos);
  assert(body_start != std::string::npos &&
         static_cast<unsigned char>(snapshot_response[body_start + 4]) == 0xff &&
         static_cast<unsigned char>(snapshot_response[body_start + 5]) == 0xd8);
  assert(request(api.port(), "/api/events/../../etc/passwd/snapshot", token)
             .find("404 Not Found") != std::string::npos);
  }
  std::filesystem::remove_all(root);
}
}  // namespace

int main() { core_api_authenticates_and_serves_cached_preview_and_events(); }

#include "aibox/renderer.hpp"
#include "aibox/engines.hpp"

#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

namespace aibox {
struct HdmiRenderer::Impl {
  GstElement* pipeline{};
  GstElement* source{};
  GstBus* bus{};
  bool ok{};
};

HdmiRenderer::HdmiRenderer() : impl_(std::make_unique<Impl>()) {
  gst_init(nullptr, nullptr);
  auto* p = impl_.get();
  const char* display = std::getenv("DISPLAY");
  const char* wayland = std::getenv("WAYLAND_DISPLAY");
  const char* requested_sink = std::getenv("AIBOX_DISPLAY_SINK");
  const bool has_desktop = (display && *display) || (wayland && *wayland);
  const auto has_factory = [](const char* name) {
    GstElementFactory* factory = gst_element_factory_find(name);
    if (!factory) return false;
    gst_object_unref(factory);
    return true;
  };
  std::vector<std::string> candidates;
  if (requested_sink && *requested_sink) {
    candidates.emplace_back(requested_sink);
  } else if (!has_desktop) {
    if (has_factory("kmssink")) candidates.emplace_back("kmssink");
    if (has_factory("fbdevsink")) candidates.emplace_back("fbdevsink");
    candidates.emplace_back("autovideosink");
  } else {
    candidates.emplace_back("autovideosink");
  }
  for (const auto& sink_factory : candidates) {
    p->pipeline = gst_pipeline_new("ai-box-hdmi-output");
    p->source = gst_element_factory_make("appsrc", "annotated-frames");
    GstElement* convert = gst_element_factory_make("videoconvert", "display-convert");
    GstElement* sink = gst_element_factory_make(sink_factory.c_str(), "hdmi-sink");
    if (!p->pipeline || !p->source || !convert || !sink) {
      std::cerr << "HDMI sink unavailable: could not create " << sink_factory << '\n';
      if (sink) gst_object_unref(sink);
      if (convert) gst_object_unref(convert);
      if (p->source) gst_object_unref(p->source);
      if (p->pipeline) gst_object_unref(p->pipeline);
      p->source = p->pipeline = nullptr;
      continue;
    }

    GstCaps* caps = gst_caps_new_simple(
        "video/x-raw", "format", G_TYPE_STRING, "BGR", "width", G_TYPE_INT,
        1024, "height", G_TYPE_INT, 600, "framerate", GST_TYPE_FRACTION, 20,
        1, nullptr);
    g_object_set(p->source, "caps", caps, "format", GST_FORMAT_TIME,
                 "is-live", TRUE, "block", FALSE, "do-timestamp", TRUE,
                 nullptr);
    gst_caps_unref(caps);
    if (g_object_class_find_property(G_OBJECT_GET_CLASS(sink), "sync"))
      g_object_set(sink, "sync", FALSE, nullptr);
    if (sink_factory == "kmssink") {
      const char* kms_driver = std::getenv("AIBOX_KMS_DRIVER");
      if (!kms_driver || !*kms_driver) kms_driver = "sunxi-drm";
      if (g_object_class_find_property(G_OBJECT_GET_CLASS(sink), "driver-name")) {
        g_object_set(sink, "driver-name", kms_driver, nullptr);
        std::cerr << "Trying kmssink DRM driver: " << kms_driver << '\n';
      } else {
        std::cerr << "kmssink has no driver-name property; using its default DRM lookup\n";
      }
    }
    gst_bin_add_many(GST_BIN(p->pipeline), p->source, convert, sink, nullptr);
    if (!gst_element_link_many(p->source, convert, sink, nullptr)) {
      std::cerr << "HDMI sink unavailable: cannot link " << sink_factory << '\n';
      gst_object_unref(p->pipeline);
      p->pipeline = p->source = nullptr;
      continue;
    }
    GstBus* bus = gst_element_get_bus(p->pipeline);
    const GstStateChangeReturn state =
        gst_element_set_state(p->pipeline, GST_STATE_PLAYING);
    if (state == GST_STATE_CHANGE_FAILURE) {
      std::cerr << "HDMI sink cannot start: " << sink_factory << '\n';
      GstMessage* error_message = gst_bus_timed_pop_filtered(
          bus, 2 * GST_SECOND, GST_MESSAGE_ERROR);
      if (error_message) {
        GError* error = nullptr;
        gchar* debug = nullptr;
        gst_message_parse_error(error_message, &error, &debug);
        std::cerr << "HDMI sink error: "
                  << (error ? error->message : "unknown error") << '\n';
        if (debug && *debug) std::cerr << "HDMI sink details: " << debug << '\n';
        if (error) g_error_free(error);
        g_free(debug);
        gst_message_unref(error_message);
      } else {
        std::cerr << "HDMI sink reported no GStreamer ERROR message\n";
      }
      gst_object_unref(bus);
      gst_element_set_state(p->pipeline, GST_STATE_NULL);
      gst_object_unref(p->pipeline);
      p->pipeline = p->source = nullptr;
      continue;
    }
    p->bus = bus;
    p->ok = true;
    std::cerr << "HDMI output uses GStreamer " << sink_factory << '\n';
    return;
  }
  std::cerr << "HDMI output unavailable: all video sinks failed\n";
}

HdmiRenderer::~HdmiRenderer() { close_display(); }

bool HdmiRenderer::available() const { return impl_->ok; }

void HdmiRenderer::close_display() {
  auto* p = impl_.get();
  p->ok = false;
  if (p->pipeline) {
    gst_element_set_state(p->pipeline, GST_STATE_NULL);
    gst_object_unref(p->pipeline);
  }
  if (p->bus) gst_object_unref(p->bus);
  p->pipeline = nullptr;
  p->source = nullptr;
  p->bus = nullptr;
}

bool HdmiRenderer::pump_events() {
  auto* p = impl_.get();
  if (!p->ok || !p->bus) return true;
  GstMessage* message = gst_bus_pop_filtered(
      p->bus, static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_WARNING));
  if (!message) return true;
  GError* error = nullptr;
  gchar* debug = nullptr;
  if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
    gst_message_parse_error(message, &error, &debug);
    std::cerr << "HDMI GStreamer sink error: "
              << (error ? error->message : "unknown error") << '\n';
    if (debug && *debug) std::cerr << "HDMI sink details: " << debug << '\n';
    if (error) g_error_free(error);
    g_free(debug);
    gst_message_unref(message);
    close_display();
    return false;
  }
  gst_message_parse_warning(message, &error, &debug);
  std::cerr << "HDMI GStreamer sink warning: "
            << (error ? error->message : "unknown warning") << '\n';
  if (error) g_error_free(error);
  g_free(debug);
  gst_message_unref(message);
  return true;
}

void HdmiRenderer::present(const std::shared_ptr<const AnalysisFrame>& frame,
                           bool camera_connected, double ai_fps,
                           const std::string& message) {
  auto* p = impl_.get();
  if (!p->ok) return;

  constexpr int width = 1024;
  constexpr int height = 600;
  constexpr int sidebar = 256;
  constexpr int video_width = width - sidebar;
  cv::Mat canvas(height, width, CV_8UC3, cv::Scalar(19, 28, 35));
  cv::rectangle(canvas, cv::Rect(video_width, 0, sidebar, height),
                cv::Scalar(35, 47, 56), cv::FILLED);

  std::size_t count = 0;
  double inference_ms = 0.0;
  double frame_age = -1.0;
  if (frame && !frame->bgr.empty()) {
    const cv::Mat& image = frame->bgr;
    const double scale = std::min(static_cast<double>(video_width) / image.cols,
                                  static_cast<double>(height) / image.rows);
    const int rw = std::max(1, static_cast<int>(std::lround(image.cols * scale)));
    const int rh = std::max(1, static_cast<int>(std::lround(image.rows * scale)));
    const int ox = (video_width - rw) / 2;
    const int oy = (height - rh) / 2;
    cv::Mat resized;
    cv::resize(image, resized, cv::Size(rw, rh), 0, 0,
               scale < 1.0 ? cv::INTER_AREA : cv::INTER_LINEAR);
    for (const auto& zone : frame->zones) {
      if (!zone.enabled || !valid_polygon(zone.polygon)) continue;
      std::vector<cv::Point> points;
      points.reserve(zone.polygon.size());
      for (const auto& point : zone.polygon)
        points.emplace_back(static_cast<int>(std::lround(point.x * image.cols * scale)),
                            static_cast<int>(std::lround(point.y * image.rows * scale)));
      cv::polylines(resized, points, true, cv::Scalar(220, 180, 45), 2, cv::LINE_AA);
      if (!points.empty())
        cv::putText(resized, zone.name, points.front() + cv::Point(3, -5),
                    cv::FONT_HERSHEY_SIMPLEX, 0.45, cv::Scalar(220, 180, 45),
                    1, cv::LINE_AA);
    }
    for (const auto& track : frame->tracks) {
      if (!track.observed || track.confidence < 0.25f) continue;
      const int x1 = std::clamp(static_cast<int>(std::lround(track.box.x * scale)), 0, rw - 1);
      const int y1 = std::clamp(static_cast<int>(std::lround(track.box.y * scale)), 0, rh - 1);
      const int x2 = std::clamp(static_cast<int>(std::lround((track.box.x + track.box.w) * scale)), 0, rw - 1);
      const int y2 = std::clamp(static_cast<int>(std::lround((track.box.y + track.box.h) * scale)), 0, rh - 1);
      if (x2 <= x1 || y2 <= y1) continue;
      const auto active = std::find_if(frame->signals.begin(), frame->signals.end(),
          [&](const BehaviorSignal& signal) {
            return signal.track_id == track.id && signal.active;
          });
      const auto dwelling = std::find_if(frame->signals.begin(), frame->signals.end(),
          [&](const BehaviorSignal& signal) {
            return signal.track_id == track.id && signal.type == "loitering" &&
                   signal.inside && signal.observed;
          });
      const cv::Scalar color = active != frame->signals.end()
          ? (active->type == "loitering" ? cv::Scalar(40, 40, 235)
                                         : cv::Scalar(20, 145, 255))
          : (dwelling != frame->signals.end() ? cv::Scalar(0, 190, 255)
                                              : cv::Scalar(50, 220, 50));
      cv::rectangle(resized, cv::Point(x1, y1), cv::Point(x2, y2), color, 2);
      cv::circle(resized, cv::Point((x1 + x2) / 2, y2), 4,
                 cv::Scalar(0, 220, 255), cv::FILLED, cv::LINE_AA);
      std::string label = "Person #" + std::to_string(track.id) + " " +
          std::to_string(static_cast<int>(std::lround(track.confidence * 100.0f))) + "%";
      if (active != frame->signals.end())
        label += active->type == "loitering" ? " LOITERING" : " FAST MOTION";
      else if (dwelling != frame->signals.end())
        label += " DWELL " + std::to_string(static_cast<int>(dwelling->dwell_seconds)) +
            "/" + std::to_string(static_cast<int>(dwelling->target_seconds)) + "s";
      cv::putText(resized, label, cv::Point(x1, std::max(18, y1 - 6)),
                  cv::FONT_HERSHEY_SIMPLEX, 0.48, color, 2,
                  cv::LINE_AA);
      ++count;
    }
    resized.copyTo(canvas(cv::Rect(ox, oy, rw, rh)));
    inference_ms = frame->inference_ms;
    frame_age = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - frame->received)
                    .count();
  }

  const cv::Scalar white(235, 242, 246), muted(180, 197, 207);
  const cv::Scalar green(70, 220, 110), orange(30, 170, 255);
  int x = video_width + 24;
  int y = 48;
  const auto line = [&](const std::string& s, const cv::Scalar& color,
                        double size = 0.55) {
    cv::putText(canvas, s, cv::Point(x, y), cv::FONT_HERSHEY_SIMPLEX, size,
                color, 1, cv::LINE_AA);
    y += 38;
  };
  line("AI BOX", white, 0.9);
  line("CAMERA", muted, 0.48);
  line(camera_connected ? "CONNECTED" : "RECONNECTING",
       camera_connected ? green : orange, 0.68);
  line(frame_age >= 0 ? "Frame age: " + std::to_string(frame_age).substr(0, 4) + " s"
                      : "Waiting for video", muted, 0.5);
  y += 10;
  line("PEOPLE", muted, 0.48);
  line(std::to_string(count), count ? green : white, 1.0);
  const auto alarms = frame ? std::count_if(frame->signals.begin(), frame->signals.end(),
      [](const BehaviorSignal& signal) { return signal.active; }) : 0;
  line("ACTIVE ALERTS", muted, 0.46);
  line(std::to_string(alarms), alarms ? orange : white, 0.85);
  line("AI rate: " + std::to_string(ai_fps).substr(0, 4) + " FPS", white);
  line("NPU: " + std::to_string(inference_ms).substr(0, 5) + " ms", white);
  if (frame) line("Frame: " + std::to_string(frame->frame_id), muted, 0.48);
  if (frame && !frame->session_id.empty())
    line("Session: " + frame->session_id.substr(0, 8), muted, 0.48);
  if (!message.empty()) line(message, cv::Scalar(70, 80, 240), 0.42);

  GstBuffer* buffer = gst_buffer_new_allocate(nullptr, canvas.total() * canvas.elemSize(), nullptr);
  if (!buffer) return;
  GstMapInfo map;
  if (!gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
    gst_buffer_unref(buffer);
    return;
  }
  std::memcpy(map.data, canvas.data, map.size);
  gst_buffer_unmap(buffer, &map);
  const GstFlowReturn flow = gst_app_src_push_buffer(GST_APP_SRC(p->source), buffer);
  if (flow != GST_FLOW_OK) {
    std::cerr << "HDMI GStreamer output stopped, flow=" << flow << '\n';
    close_display();
  }
}
}  // namespace aibox

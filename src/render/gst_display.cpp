#include "aibox/renderer.hpp"
#include "aibox/coco.hpp"
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
namespace {
void dashed_line(cv::Mat& image, cv::Point a, cv::Point b,
                 const cv::Scalar& color, int thickness) {
  const cv::Point delta = b - a;
  const double length = std::hypot(static_cast<double>(delta.x),
                                   static_cast<double>(delta.y));
  if (length < 1.0) return;
  const cv::Point2d direction(delta.x / length, delta.y / length);
  for (double offset = 0.0; offset < length; offset += 13.0) {
    const double end = std::min(length, offset + 8.0);
    cv::line(image,
        cv::Point(cvRound(a.x + direction.x * offset), cvRound(a.y + direction.y * offset)),
        cv::Point(cvRound(a.x + direction.x * end), cvRound(a.y + direction.y * end)),
        color, thickness, cv::LINE_AA);
  }
}

void dashed_rectangle(cv::Mat& image, int x0, int y0, int x1, int y1,
                      const cv::Scalar& color) {
  dashed_line(image, {x0, y0}, {x1, y0}, color, 2);
  dashed_line(image, {x1, y0}, {x1, y1}, color, 2);
  dashed_line(image, {x1, y1}, {x0, y1}, color, 2);
  dashed_line(image, {x0, y1}, {x0, y0}, color, 2);
}

void panel(cv::Mat& image, const cv::Rect& bounds, const cv::Scalar& fill,
           const cv::Scalar& border) {
  cv::rectangle(image, bounds, fill, cv::FILLED);
  cv::rectangle(image, bounds, border, 1, cv::LINE_AA);
}

void text(cv::Mat& image, const std::string& value, cv::Point origin,
          double scale, const cv::Scalar& color, int thickness = 1) {
  cv::putText(image, value, origin, cv::FONT_HERSHEY_SIMPLEX, scale, color,
              thickness, cv::LINE_AA);
}

std::string alert_name(const std::string& type) {
  if (type == "loitering") return "LOITERING";
  if (type == "running") return "RUNNING";
  if (type == "fast_motion") return "FAST MOTION";
  if (type == "crowd") return "CROWD";
  return type;
}
}  // namespace

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
  const cv::Scalar background(16, 22, 30), panel_fill(25, 34, 44);
  const cv::Scalar border(48, 62, 74), white(238, 243, 247);
  const cv::Scalar muted(157, 174, 187), cyan(220, 190, 75);
  const cv::Scalar green(85, 220, 125), red(70, 75, 235);
  constexpr int video_x = 16, video_y = 76, video_w = 720, video_h = 460;
  constexpr int sidebar_x = 748, sidebar_w = 260;
  constexpr int view_x = video_x + 8, view_y = video_y + 8;
  constexpr int view_w = video_w - 16, view_h = video_h - 16;

  cv::Mat canvas(height, width, CV_8UC3, background);
  cv::rectangle(canvas, cv::Rect(0, 0, width, 62), cv::Scalar(23, 31, 40), cv::FILLED);
  cv::line(canvas, {0, 61}, {width, 61}, border, 1, cv::LINE_AA);
  cv::rectangle(canvas, cv::Rect(16, 15, 4, 32), cyan, cv::FILLED);
  text(canvas, "AI BOX", {31, 31}, 0.78, white, 2);
  text(canvas, "EDGE VIDEO ANALYTICS", {32, 48}, 0.34, muted);
  text(canvas, "LIVE MONITOR", {sidebar_x + 4, 37}, 0.48, cyan, 1);

  panel(canvas, {video_x, video_y, video_w, video_h}, cv::Scalar(8, 12, 17), border);
  panel(canvas, {sidebar_x, video_y, sidebar_w, 78}, panel_fill, border);
  panel(canvas, {sidebar_x, 164, sidebar_w, 108}, panel_fill, border);
  panel(canvas, {sidebar_x, 282, sidebar_w, 100}, panel_fill, border);
  panel(canvas, {sidebar_x, 392, sidebar_w, 144}, panel_fill, border);

  std::size_t tracked = 0;
  std::size_t detections = 0;
  double inference_ms = 0.0;
  double frame_age = -1.0;
  int image_x = view_x, image_y = view_y, image_w = view_w, image_h = view_h;
  if (frame && !frame->bgr.empty()) {
    const cv::Mat& image = frame->bgr;
    const int source_height = frame->source_height > 0
        ? frame->source_height : image.rows;
    const double scale = std::min(static_cast<double>(view_w) / image.cols,
                                  static_cast<double>(view_h) / image.rows);
    image_w = std::max(1, static_cast<int>(std::lround(image.cols * scale)));
    image_h = std::max(1, static_cast<int>(std::lround(image.rows * scale)));
    image_x = view_x + (view_w - image_w) / 2;
    image_y = view_y + (view_h - image_h) / 2;
    cv::Mat resized;
    cv::resize(image, resized, {image_w, image_h}, 0, 0,
               scale < 1.0 ? cv::INTER_AREA : cv::INTER_LINEAR);

    for (const auto& zone : frame->zones) {
      if (!zone.enabled || !valid_polygon(zone.polygon)) continue;
      const auto crowd = std::find_if(frame->signals.begin(), frame->signals.end(),
          [&](const BehaviorSignal& signal) {
            return signal.type == "crowd" && signal.zone_id == zone.id;
          });
      std::vector<cv::Point> points;
      points.reserve(zone.polygon.size());
      for (const auto& point : zone.polygon)
        points.emplace_back(cvRound(point.x * image_w),
            std::clamp(cvRound(point.y * source_height * scale), 0, image_h - 1));
      const cv::Scalar color = crowd != frame->signals.end() && crowd->active
          ? red : cyan;
      cv::polylines(resized, points, true, color, 2, cv::LINE_AA);
      if (!points.empty()) {
        const std::string zone_label = crowd != frame->signals.end()
            ? zone.name + "  " + std::to_string(crowd->count) + "/" +
                  std::to_string(crowd->threshold)
            : zone.name;
        const int label_y = std::clamp(points.front().y + 22, 20, image_h - 4);
        text(resized, zone_label, {std::clamp(points.front().x + 5, 4, image_w - 120),
                                  label_y}, 0.42, color, 2);
      }
    }

    for (const auto& track : frame->tracks) {
      if (track.confidence < 0.25f) continue;
      const int x1 = std::clamp(cvRound(track.box.x * scale), 0, image_w - 1);
      const int y1 = std::clamp(cvRound(track.box.y * scale), 0, image_h - 1);
      const int x2 = std::clamp(cvRound((track.box.x + track.box.w) * scale), 0, image_w - 1);
      const int y2 = std::clamp(cvRound((track.box.y + track.box.h) * scale), 0, image_h - 1);
      if (x2 <= x1 || y2 <= y1) continue;
      const auto active = std::find_if(frame->signals.begin(), frame->signals.end(),
          [&](const BehaviorSignal& signal) {
            return signal.track_id == track.id && signal.active;
          });
      const auto running = std::find_if(frame->signals.begin(), frame->signals.end(),
          [&](const BehaviorSignal& signal) {
            return signal.track_id == track.id && signal.type == "running" && signal.active;
          });
      const auto dwelling = std::find_if(frame->signals.begin(), frame->signals.end(),
          [&](const BehaviorSignal& signal) {
            return signal.track_id == track.id && signal.type == "loitering" &&
                   signal.inside && signal.observed;
          });
      const cv::Scalar color = !track.observed ? muted
          : active != frame->signals.end()
              ? (active->type == "loitering" ? red : cv::Scalar(0, 150, 255))
              : (dwelling != frame->signals.end() ? cv::Scalar(0, 190, 255)
                 : (track.class_id == 0 ? green : cyan));
      if (track.observed)
        cv::rectangle(resized, {x1, y1}, {x2, y2}, color, 2, cv::LINE_AA);
      else
        dashed_rectangle(resized, x1, y1, x2, y2, color);

      std::string label = coco_class_name(track.class_id) + "  #" + std::to_string(track.id);
      if (!track.observed)
        label += "  TEMP";
      else if (active != frame->signals.end() && active->type == "loitering" &&
               dwelling != frame->signals.end())
        label += "  LOITERING " +
                 std::to_string(static_cast<int>(std::floor(dwelling->dwell_seconds))) +
                 "/" + std::to_string(static_cast<int>(std::ceil(dwelling->target_seconds))) + "s";
      else if (dwelling != frame->signals.end())
        label += "  DWELL " + std::to_string(static_cast<int>(std::floor(dwelling->dwell_seconds))) +
                 "/" + std::to_string(static_cast<int>(std::ceil(dwelling->target_seconds))) + "s";
      else if (active != frame->signals.end())
        label += "  " + alert_name(active->type);
      else if (running != frame->signals.end())
        label += "  RUNNING";
      int baseline = y1 - 8;
      const cv::Size label_size = cv::getTextSize(label, cv::FONT_HERSHEY_SIMPLEX,
                                                  0.40, 1, nullptr);
      int label_top = baseline - label_size.height - 8;
      if (label_top < 2) label_top = std::min(y1 + 3, image_h - label_size.height - 12);
      const int label_left = std::clamp(x1, 2,
          std::max(2, image_w - label_size.width - 12));
      cv::rectangle(resized,
          {label_left, label_top},
          {label_left + label_size.width + 12, label_top + label_size.height + 8},
          color, cv::FILLED);
      text(resized, label, {label_left + 6, label_top + label_size.height + 3},
           0.40, cv::Scalar(255, 255, 255), 1);
      if (track.observed) ++tracked;
    }

    if (frame->overlay_stale)
      text(resized, "AI OVERLAY DELAYED", {12, 28}, 0.55,
           cv::Scalar(0, 170, 255), 2);
    resized.copyTo(canvas(cv::Rect(image_x, image_y, image_w, image_h)));
    detections = frame->people.size();
    inference_ms = frame->inference_ms;
    frame_age = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - frame->received).count();
  }

  const std::string live_status = camera_connected ? "LIVE" : "RECONNECTING";
  const cv::Scalar status_color = camera_connected ? green : red;
  cv::circle(canvas, {sidebar_x + 22, 101}, 5, status_color, cv::FILLED, cv::LINE_AA);
  text(canvas, "CAMERA INPUT", {sidebar_x + 36, 96}, 0.38, muted, 1);
  text(canvas, live_status, {sidebar_x + 36, 122}, 0.62, status_color, 2);
  const std::string age_text = frame_age >= 0
      ? "Frame age  " + std::to_string(frame_age).substr(0, 4) + " s"
      : "Waiting for video";
  text(canvas, age_text, {sidebar_x + 15, 145}, 0.37, muted);

  text(canvas, "TRACKED OBJECTS", {sidebar_x + 15, 187}, 0.38, muted, 1);
  text(canvas, std::to_string(tracked), {sidebar_x + 15, 232}, 1.25,
       tracked ? green : white, 2);
  text(canvas, std::to_string(detections) + " detections  /  live tracks",
       {sidebar_x + 82, 232}, 0.34, muted);

  text(canvas, "ANALYTICS PERFORMANCE", {sidebar_x + 15, 305}, 0.36, muted, 1);
  text(canvas, std::to_string(ai_fps).substr(0, 4), {sidebar_x + 15, 350},
       0.95, white, 2);
  text(canvas, "AI FPS", {sidebar_x + 15, 370}, 0.34, muted);
  cv::line(canvas, {sidebar_x + 132, 318}, {sidebar_x + 132, 366}, border, 1);
  text(canvas, std::to_string(inference_ms).substr(0, 5),
       {sidebar_x + 148, 350}, 0.72, cyan, 2);
  text(canvas, "NPU ms", {sidebar_x + 148, 370}, 0.34, muted);

  const int alert_count = frame ? static_cast<int>(std::count_if(
      frame->signals.begin(), frame->signals.end(),
      [](const BehaviorSignal& signal) { return signal.active; })) : 0;
  text(canvas, "ACTIVE ALERTS", {sidebar_x + 15, 416}, 0.38, muted, 1);
  text(canvas, std::to_string(alert_count), {sidebar_x + 15, 461}, 1.0,
       alert_count ? red : white, 2);
  int alert_y = 430;
  if (frame) {
    for (const auto& signal : frame->signals) {
      if (!signal.active || alert_y > 510) continue;
      const std::string detail = alert_name(signal.type) +
          (signal.track_id > 0 ? "  #" + std::to_string(signal.track_id) : "") +
          (!signal.zone_id.empty() ? "  " + signal.zone_id : "");
      text(canvas, detail, {sidebar_x + 84, alert_y}, 0.34, red, 1);
      alert_y += 22;
    }
  }
  if (alert_count == 0)
    text(canvas, "No active alerts", {sidebar_x + 84, 452}, 0.37, muted);
  if (!message.empty())
    text(canvas, message.substr(0, 31), {sidebar_x + 15, 522}, 0.32, red);

  cv::rectangle(canvas, {16, 550, 992, 34}, cv::Scalar(23, 31, 40), cv::FILLED);
  cv::line(canvas, {16, 550}, {1008, 550}, border, 1, cv::LINE_AA);
  const std::string session = frame && !frame->session_id.empty()
      ? frame->session_id.substr(0, 8) : "--";
  text(canvas, "VIPLite  |  H.264  |  Session " + session,
       {28, 572}, 0.34, muted);
  const std::string frame_text = frame
      ? "Frame " + std::to_string(frame->frame_id) : "Frame --";
  text(canvas, frame_text, {850, 572}, 0.34, muted);

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

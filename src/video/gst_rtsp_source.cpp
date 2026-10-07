#include "aibox/video_source.hpp"
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/rtsp/gstrtsptransport.h>
#include <gst/video/video.h>
#include <opencv2/imgproc.hpp>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <random>
#include <stdexcept>
namespace aibox
{
    struct GstVideoSource::Impl
    {
        std::string source_type, uri, decoder, username, password, error, session;
        mutable std::mutex error_mutex;
        GstElement *pipeline{}, *source{}, *sink{};
        GstElement *frame_queue{};
        GstBus *bus{};
        std::uint64_t next_frame{};
        static void pad_added(GstElement *, GstPad *pad, gpointer opaque)
        {
            auto *self = static_cast<Impl *>(opaque);
            GstPad *target = gst_element_get_static_pad(self->depay, "sink");
            if (!gst_pad_is_linked(target) && gst_pad_link(pad, target) != GST_PAD_LINK_OK)
            {
                std::lock_guard<std::mutex> lock(self->error_mutex);
                self->error = "Could not link RTSP H.264 stream pad";
            }
            gst_object_unref(target);
        }
        static void decoded_pad_added(GstElement *, GstPad *pad, gpointer opaque)
        {
            auto *self = static_cast<Impl *>(opaque);
            GstCaps *caps = gst_pad_get_current_caps(pad);
            if (!caps) caps = gst_pad_query_caps(pad, nullptr);
            bool is_video = false;
            if (caps && gst_caps_get_size(caps) > 0) {
                const GstStructure *structure = gst_caps_get_structure(caps, 0);
                const char *name = gst_structure_get_name(structure);
                is_video = name && g_str_has_prefix(name, "video/x-raw");
            }
            if (caps) gst_caps_unref(caps);
            if (!is_video) return;
            GstPad *target = gst_element_get_static_pad(self->frame_queue, "sink");
            if (!gst_pad_is_linked(target) && gst_pad_link(pad, target) != GST_PAD_LINK_OK) {
                std::lock_guard<std::mutex> lock(self->error_mutex);
                self->error = "Could not link decoded video stream";
            }
            gst_object_unref(target);
        }
        static void source_setup(GstElement *, GstElement *source, gpointer opaque)
        {
            auto *self = static_cast<Impl *>(opaque);
            if (self->username.empty() && self->password.empty()) return;
            GObjectClass *klass = G_OBJECT_GET_CLASS(source);
            if (!self->username.empty()) {
                if (g_object_class_find_property(klass, "user-id"))
                    g_object_set(source, "user-id", self->username.c_str(), nullptr);
                else if (g_object_class_find_property(klass, "username"))
                    g_object_set(source, "username", self->username.c_str(), nullptr);
            }
            if (g_object_class_find_property(klass, "user-pw") && !self->password.empty())
                g_object_set(source, "user-pw", self->password.c_str(), nullptr);
            else if (g_object_class_find_property(klass, "password") && !self->password.empty())
                g_object_set(source, "password", self->password.c_str(), nullptr);
        }
        GstElement *depay{};
    };
    static std::string uuid()
    {
        std::random_device rd;
        std::mt19937_64 g(rd());
        std::array<unsigned char, 16> b{};
        for (auto &x : b)
            x = static_cast<unsigned char>(g());
        b[6] = (b[6] & 0x0f) | 0x40;
        b[8] = (b[8] & 0x3f) | 0x80;
        char out[37];
        std::snprintf(out, sizeof(out), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
        return out;
    }
    GstVideoSource::GstVideoSource(std::string source_type, std::string uri,
        std::string decoder, std::string username, std::string password)
        : impl_(std::make_unique<Impl>())
    {
        if (uri.empty())
            throw std::invalid_argument("Video source is empty");
        if (source_type != "rtsp" && source_type != "video_url" && source_type != "file")
            throw std::invalid_argument("Unsupported video source type: " + source_type);
        if (source_type == "rtsp" && decoder != "omxh264dec" && decoder != "avdec_h264")
            throw std::invalid_argument("Unsupported H.264 decoder: " + decoder);
        impl_->source_type = std::move(source_type);
        impl_->uri = std::move(uri);
        impl_->decoder = std::move(decoder);
        impl_->username = std::move(username);
        impl_->password = std::move(password);
    }
    GstVideoSource::~GstVideoSource() { stop(); }
    void GstVideoSource::start()
    {
        stop();
        gst_init(nullptr, nullptr);
        auto *p = impl_.get();
        {
            std::lock_guard<std::mutex> lock(p->error_mutex);
            p->error.clear();
        }
        p->session = uuid();
        p->next_frame = 0;
        p->pipeline = gst_pipeline_new("ai-box-capture");
        const bool rtsp = p->source_type == "rtsp";
        std::string uri = p->uri;
        if (p->source_type == "file" && uri.rfind("file:///", 0) != 0) {
            GError *uri_error = nullptr;
            gchar *file_uri = gst_filename_to_uri(uri.c_str(), &uri_error);
            if (!file_uri) {
                const std::string message = uri_error ? uri_error->message : "Invalid video file path";
                if (uri_error) g_error_free(uri_error);
                stop();
                throw std::invalid_argument(message);
            }
            uri = file_uri;
            g_free(file_uri);
        }
        p->source = gst_element_factory_make(rtsp ? "rtspsrc" : "uridecodebin", "camera");
        p->depay = rtsp ? gst_element_factory_make("rtph264depay", "depay") : nullptr;
        auto *parse = rtsp ? gst_element_factory_make("h264parse", "parse") : nullptr;
        auto *dec = rtsp ? gst_element_factory_make(p->decoder.c_str(), "decoder") : nullptr;
        auto *convert = gst_element_factory_make("videoconvert", "convert");
        auto *capsfilter = gst_element_factory_make("capsfilter", "bgr-format");
        p->sink = gst_element_factory_make("appsink", "framesink");
        p->frame_queue = gst_element_factory_make("queue", "latest-decoded-frame-queue");
        if (!p->pipeline || !p->source || (rtsp && (!p->depay || !parse || !dec)) ||
            !convert || !capsfilter || !p->frame_queue || !p->sink)
        {
            stop();
            throw std::runtime_error(rtsp
                ? "Required RTSP/H.264 GStreamer element unavailable"
                : "Required URI video decoder GStreamer element unavailable (uridecodebin/videoconvert/appsink)");
        }
        GstUri *parsed = gst_uri_from_string(uri.c_str());
        if (!parsed)
        {
            stop();
            throw std::runtime_error("Invalid video URI");
        }
        gchar *safe_uri = gst_uri_to_string(parsed);
        gst_uri_unref(parsed);
        int latency_ms = 200;
        if (const char *configured_latency = std::getenv("AIBOX_RTSP_LATENCY_MS")) {
            char *end = nullptr;
            const long value = std::strtol(configured_latency, &end, 10);
            if (end != configured_latency && *end == '\0' && value >= 50 && value <= 1000)
                latency_ms = static_cast<int>(value);
            else
                g_warning("Ignoring invalid AIBOX_RTSP_LATENCY_MS; expected 50..1000");
        }
        if (rtsp) {
            g_object_set(p->source, "location", safe_uri, "latency", latency_ms,
                         "protocols", GST_RTSP_LOWER_TRANS_TCP, nullptr);
            if (g_object_class_find_property(G_OBJECT_GET_CLASS(p->source), "drop-on-latency"))
                g_object_set(p->source, "drop-on-latency", TRUE, nullptr);
        } else {
            g_object_set(p->source, "uri", safe_uri, nullptr);
        }
        g_free(safe_uri);
        if (rtsp) {
            if (!p->username.empty()) g_object_set(p->source, "user-id", p->username.c_str(), nullptr);
            if (!p->password.empty()) g_object_set(p->source, "user-pw", p->password.c_str(), nullptr);
        } else if (!rtsp) {
            g_signal_connect(p->source, "source-setup", G_CALLBACK(Impl::source_setup), p);
        }
        GstCaps *caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "BGR", nullptr);
        g_object_set(capsfilter, "caps", caps, nullptr);
        gst_caps_unref(caps);
        g_object_set(p->frame_queue, "max-size-buffers", 1, "max-size-bytes", 0,
                     "max-size-time", static_cast<guint64>(0), "leaky", 2, nullptr);
        g_object_set(p->sink, "max-buffers", 1, "drop", TRUE,
                     "sync", p->source_type == "file", nullptr);
        if (g_object_class_find_property(G_OBJECT_GET_CLASS(p->sink), "enable-last-sample"))
            g_object_set(p->sink, "enable-last-sample", FALSE, nullptr);
        if (rtsp) {
          gst_bin_add_many(GST_BIN(p->pipeline), p->source, p->depay, parse, dec,
                           p->frame_queue, convert, capsfilter, p->sink, nullptr);
        } else {
          gst_bin_add_many(GST_BIN(p->pipeline), p->source, p->frame_queue,
                           convert, capsfilter, p->sink, nullptr);
        }
        const gboolean linked = rtsp
            ? gst_element_link_many(p->depay, parse, dec, p->frame_queue, convert, capsfilter, p->sink, nullptr)
            : gst_element_link_many(p->frame_queue, convert, capsfilter, p->sink, nullptr);
        if (!linked)
        {
            stop();
            throw std::runtime_error("Could not link video decode pipeline");
        }
        g_signal_connect(p->source, "pad-added", G_CALLBACK(
            rtsp ? Impl::pad_added : Impl::decoded_pad_added), p);
        p->bus = gst_element_get_bus(p->pipeline);
        if (gst_element_set_state(p->pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
        {
            stop();
            throw std::runtime_error("Could not start GStreamer RTSP pipeline");
        }
    }
    std::optional<VideoFrame> GstVideoSource::pull(std::chrono::milliseconds timeout)
    {
        auto *p = impl_.get();
        if (!p->sink)
            return std::nullopt;
        GstSample *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(p->sink), static_cast<GstClockTime>(timeout.count()) * GST_MSECOND);
        if (!sample)
        {
            GstMessage *msg = gst_bus_pop_filtered(p->bus, static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
            if (msg)
            {
                GError *err = nullptr;
                gchar *debug = nullptr;
                const bool looped = GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS &&
                    p->source_type == "file" &&
                    gst_element_seek_simple(p->pipeline, GST_FORMAT_TIME,
                        static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT), 0);
                {
                    std::lock_guard<std::mutex> lock(p->error_mutex);
                    if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR)
                    {
                        gst_message_parse_error(msg, &err, &debug);
                        p->error = err ? err->message : "GStreamer error";
                    }
                    else if (looped)
                        p->error.clear();
                    else
                        p->error = "Video source ended";
                }
                if (err)
                    g_error_free(err);
                g_free(debug);
                gst_message_unref(msg);
            }
            return std::nullopt;
        }
        GstCaps *caps = gst_sample_get_caps(sample);
        GstVideoInfo info;
        gst_video_info_init(&info);
        if (!caps || !gst_video_info_from_caps(&info, caps))
        {
            gst_sample_unref(sample);
            {
                std::lock_guard<std::mutex> lock(p->error_mutex);
                p->error = "Invalid decoded video caps";
            }
            return std::nullopt;
        }
        GstBuffer *buffer = gst_sample_get_buffer(sample);
        GstVideoFrame mapped;
        if (!gst_video_frame_map(&mapped, &info, buffer, GST_MAP_READ))
        {
            gst_sample_unref(sample);
            {
                std::lock_guard<std::mutex> lock(p->error_mutex);
                p->error = "Could not map decoded frame";
            }
            return std::nullopt;
        }
        const int width = GST_VIDEO_INFO_WIDTH(&info), height = GST_VIDEO_INFO_HEIGHT(&info), stride = GST_VIDEO_FRAME_PLANE_STRIDE(&mapped, 0);
        const auto *data = static_cast<const std::uint8_t *>(GST_VIDEO_FRAME_PLANE_DATA(&mapped, 0));
        cv::Mat view(height, width, CV_8UC3, const_cast<std::uint8_t *>(data), static_cast<std::size_t>(stride));
        VideoFrame frame{view.clone(), p->next_frame++, p->session, std::chrono::steady_clock::now(), std::nullopt};
        if (GST_BUFFER_PTS_IS_VALID(buffer))
            frame.pts_ns = GST_BUFFER_PTS(buffer);
        gst_video_frame_unmap(&mapped);
        gst_sample_unref(sample);
        return frame;
    }
    void GstVideoSource::stop()
    {
        auto *p = impl_.get();
        if (p->pipeline)
        {
            gst_element_set_state(p->pipeline, GST_STATE_NULL);
            gst_object_unref(p->pipeline);
        }
        if (p->bus)
            gst_object_unref(p->bus);
        p->pipeline = p->source = p->sink = p->depay = p->frame_queue = nullptr;
        p->bus = nullptr;
    }
    std::string GstVideoSource::last_error() const
    {
        std::lock_guard<std::mutex> lock(impl_->error_mutex);
        return impl_->error;
    }
}

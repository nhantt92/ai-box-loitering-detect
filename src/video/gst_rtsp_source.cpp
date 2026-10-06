#include "aibox/video_source.hpp"
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/rtsp/gstrtsptransport.h>
#include <gst/video/video.h>
#include <opencv2/imgproc.hpp>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <random>
#include <stdexcept>
namespace aibox
{
    struct GstRtspSource::Impl
    {
        std::string uri, decoder, error, session;
        mutable std::mutex error_mutex;
        GstElement *pipeline{}, *source{}, *sink{};
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
    GstRtspSource::GstRtspSource(std::string uri, std::string decoder) : impl_(std::make_unique<Impl>())
    {
        if (uri.empty())
            throw std::invalid_argument("RTSP URI is empty");
        if (decoder != "omxh264dec" && decoder != "avdec_h264")
            throw std::invalid_argument("Unsupported H.264 decoder: " + decoder);
        impl_->uri = std::move(uri);
        impl_->decoder = std::move(decoder);
    }
    GstRtspSource::~GstRtspSource() { stop(); }
    void GstRtspSource::start()
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
        p->source = gst_element_factory_make("rtspsrc", "camera");
        p->depay = gst_element_factory_make("rtph264depay", "depay");
        auto *parse = gst_element_factory_make("h264parse", "parse");
        auto *dec = gst_element_factory_make(p->decoder.c_str(), "decoder");
        auto *convert = gst_element_factory_make("videoconvert", "convert");
        auto *capsfilter = gst_element_factory_make("capsfilter", "bgr-format");
        p->sink = gst_element_factory_make("appsink", "framesink");
        if (!p->pipeline || !p->source || !p->depay || !parse || !dec || !convert || !capsfilter || !p->sink)
        {
            stop();
            throw std::runtime_error("Required GStreamer element unavailable (rtspsrc/rtph264depay/h264parse/" + p->decoder + "/videoconvert/appsink)");
        }
        GstUri *parsed = gst_uri_from_string(p->uri.c_str());
        if (!parsed)
        {
            stop();
            throw std::runtime_error("Invalid RTSP URI");
        }
        const char *userinfo = gst_uri_get_userinfo(parsed);
        std::string uri_user, uri_password;
        if (userinfo && *userinfo)
        {
            std::string pair(userinfo);
            const auto colon = pair.find(':');
            uri_user = pair.substr(0, colon);
            if (colon != std::string::npos)
                uri_password = pair.substr(colon + 1);
            gst_uri_set_userinfo(parsed, nullptr);
        }
        gchar *safe_uri = gst_uri_to_string(parsed);
        gst_uri_unref(parsed);
        g_object_set(p->source, "location", safe_uri, "latency", 200, "protocols", GST_RTSP_LOWER_TRANS_TCP, nullptr);
        g_free(safe_uri);
        const char *user = std::getenv("AIBOX_RTSP_USER");
        const char *password = std::getenv("AIBOX_RTSP_PASSWORD");
        if (!user || !*user)
            user = uri_user.empty() ? nullptr : uri_user.c_str();
        if (!password || !*password)
            password = uri_password.empty() ? nullptr : uri_password.c_str();
        if (user)
            g_object_set(p->source, "user-id", user, nullptr);
        if (password)
            g_object_set(p->source, "user-pw", password, nullptr);
        GstCaps *caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "BGR", nullptr);
        g_object_set(capsfilter, "caps", caps, nullptr);
        gst_caps_unref(caps);
        g_object_set(p->sink, "max-buffers", 1, "drop", TRUE, "sync", FALSE, nullptr);
        gst_bin_add_many(GST_BIN(p->pipeline), p->source, p->depay, parse, dec, convert, capsfilter, p->sink, nullptr);
        if (!gst_element_link_many(p->depay, parse, dec, convert, capsfilter, p->sink, nullptr))
        {
            stop();
            throw std::runtime_error("Could not link H.264 decode pipeline");
        }
        g_signal_connect(p->source, "pad-added", G_CALLBACK(Impl::pad_added), p);
        p->bus = gst_element_get_bus(p->pipeline);
        if (gst_element_set_state(p->pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
        {
            stop();
            throw std::runtime_error("Could not start GStreamer RTSP pipeline");
        }
    }
    std::optional<VideoFrame> GstRtspSource::pull(std::chrono::milliseconds timeout)
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
                {
                    std::lock_guard<std::mutex> lock(p->error_mutex);
                    if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR)
                    {
                        gst_message_parse_error(msg, &err, &debug);
                        p->error = err ? err->message : "GStreamer error";
                    }
                    else
                        p->error = "RTSP stream ended";
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
    void GstRtspSource::stop()
    {
        auto *p = impl_.get();
        if (p->pipeline)
        {
            gst_element_set_state(p->pipeline, GST_STATE_NULL);
            gst_object_unref(p->pipeline);
        }
        if (p->bus)
            gst_object_unref(p->bus);
        p->pipeline = p->source = p->sink = p->depay = nullptr;
        p->bus = nullptr;
    }
    std::string GstRtspSource::last_error() const
    {
        std::lock_guard<std::mutex> lock(impl_->error_mutex);
        return impl_->error;
    }
}

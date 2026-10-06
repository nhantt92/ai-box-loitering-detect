#include "aibox/yolo.hpp"
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace aibox
{
    PreparedInput prepare_yolov5(const cv::Mat &bgr, int size)
    {
        if (bgr.empty() || size <= 0 || bgr.channels() != 3)
            throw std::invalid_argument("Expected non-empty 3-channel BGR image");
        const double scale = std::min(static_cast<double>(size) / bgr.cols, static_cast<double>(size) / bgr.rows);
        // Match the supplied AW YOLOv5 demo, which truncates scaled dimensions to int.
        const int rw = std::max(1, static_cast<int>(bgr.cols * scale));
        const int rh = std::max(1, static_cast<int>(bgr.rows * scale));
        const int left = (size - rw) / 2, top = (size - rh) / 2;
        cv::Mat resized, rgb, padded(size, size, CV_8UC3, cv::Scalar(0, 0, 0));
        cv::resize(bgr, resized, cv::Size(rw, rh), 0, 0, cv::INTER_LINEAR);
        cv::cvtColor(resized, rgb, cv::COLOR_BGR2RGB);
        rgb.copyTo(padded(cv::Rect(left, top, rw, rh)));
        std::vector<std::uint8_t> chw(static_cast<std::size_t>(3) * size * size);
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x)
                for (int c = 0; c < 3; ++c)
                    chw[static_cast<std::size_t>(c) * size * size + static_cast<std::size_t>(y) * size + x] = padded.at<cv::Vec3b>(y, x)[c];
        return {padded, std::move(chw), {bgr.cols, bgr.rows, rw, rh, left, top, scale}};
    }
    static float sigmoid(float x)
    {
        if (x >= 0)
        {
            const float z = std::exp(-x);
            return 1 / (1 + z);
        }
        const float z = std::exp(x);
        return z / (1 + z);
    }
    static float iou(const Box &a, const Box &b)
    {
        const float x0 = std::max(a.x, b.x), y0 = std::max(a.y, b.y), x1 = std::min(a.x + a.w, b.x + b.w), y1 = std::min(a.y + a.h, b.y + b.h);
        const float inter = std::max(0.f, x1 - x0) * std::max(0.f, y1 - y0);
        const float uni = a.w * a.h + b.w * b.h - inter;
        return uni > 0 ? inter / uni : 0;
    }
    std::vector<Detection> decode_yolov5(const std::vector<YoloHead> &heads, const Letterbox &t, float threshold, float nms, bool person_only)
    {
        static constexpr std::array<int, 3> strides{8, 16, 32};
        static constexpr float anchors[3][3][2] = {{{10, 13}, {16, 30}, {33, 23}}, {{30, 61}, {62, 45}, {59, 119}}, {{116, 90}, {156, 198}, {373, 326}}};
        if (t.source_width <= 0 || t.source_height <= 0 || t.resized_width <= 0 || t.resized_height <= 0 || !std::isfinite(threshold) || threshold < 0 || threshold > 1 || nms < 0 || nms > 1)
            throw std::invalid_argument("Invalid YOLO decoder contract");
        if (heads.size() != 3)
            throw std::invalid_argument("YOLOv5 COCO requires three output heads");
        std::vector<Detection> candidates;
        for (int level = 0; level < 3; ++level)
        {
            const int stride = strides[level];
            auto it = std::find_if(heads.begin(), heads.end(), [&](const YoloHead &h)
                                   { return h.stride == stride; });
            if (it == heads.end())
                throw std::invalid_argument("Missing YOLO output stride");
            const int gw = 640 / stride, gh = 640 / stride;
            const std::size_t expected = static_cast<std::size_t>(3) * gh * gw * 85;
            if (!it->data || it->elements != expected)
                throw std::invalid_argument("YOLO output tensor size mismatch");
            for (int a = 0; a < 3; ++a)
                for (int y = 0; y < gh; ++y)
                    for (int x = 0; x < gw; ++x)
                    {
                        const std::size_t off = ((static_cast<std::size_t>(a) * gh + y) * gw + x) * 85;
                        const float *v = it->data + off;
                        const float obj = sigmoid(v[4]);
                        int cls = 0;
                        float cp = 0;
                        const int first = person_only ? 0 : 0, last = person_only ? 1 : 80;
                        for (int c = first; c < last; ++c)
                        {
                            float q = sigmoid(v[5 + c]);
                            if (q > cp)
                            {
                                cp = q;
                                cls = c;
                            }
                        }
                        const float conf = obj * cp;
                        if (conf < threshold)
                            continue;
                        const float cx = (sigmoid(v[0]) * 2 - 0.5f + x) * stride, cy = (sigmoid(v[1]) * 2 - 0.5f + y) * stride, w = std::pow(sigmoid(v[2]) * 2, 2) * anchors[level][a][0], h = std::pow(sigmoid(v[3]) * 2, 2) * anchors[level][a][1];
                        float x0 = (cx - w / 2 - t.pad_left) * (static_cast<float>(t.source_width) / t.resized_width), y0 = (cy - h / 2 - t.pad_top) * (static_cast<float>(t.source_height) / t.resized_height), x1 = (cx + w / 2 - t.pad_left) * (static_cast<float>(t.source_width) / t.resized_width), y1 = (cy + h / 2 - t.pad_top) * (static_cast<float>(t.source_height) / t.resized_height);
                        x0 = std::clamp(x0, 0.f, static_cast<float>(t.source_width));
                        x1 = std::clamp(x1, 0.f, static_cast<float>(t.source_width));
                        y0 = std::clamp(y0, 0.f, static_cast<float>(t.source_height));
                        y1 = std::clamp(y1, 0.f, static_cast<float>(t.source_height));
                        if (x1 > x0 && y1 > y0)
                            candidates.push_back({{x0, y0, x1 - x0, y1 - y0}, cls, conf});
                    }
        }
        std::sort(candidates.begin(), candidates.end(), [](const Detection &a, const Detection &b)
                  { return a.confidence > b.confidence; });
        std::vector<Detection> kept;
        for (const auto &d : candidates)
        {
            bool accept = true;
            for (const auto &k : kept)
                if (d.class_id == k.class_id && iou(d.box, k.box) > nms)
                {
                    accept = false;
                    break;
                }
            if (accept)
                kept.push_back(d);
        }
        return kept;
    }
}

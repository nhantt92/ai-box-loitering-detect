#pragma once
#include "aibox/types.hpp"
#include <opencv2/core.hpp>
#include <array>
#include <vector>
namespace aibox {
struct Letterbox { int source_width{}, source_height{}, resized_width{}, resized_height{}, pad_left{}, pad_top{}; double scale{}; };
struct PreparedInput { cv::Mat rgb; std::vector<std::uint8_t> chw; Letterbox transform; };
PreparedInput prepare_yolov5(const cv::Mat& bgr, int input_size=640);
struct YoloHead { int stride{}; const float* data{}; std::size_t elements{}; };
struct Detection { Box box; int class_id{}; float confidence{}; };
std::vector<Detection> decode_yolov5(const std::vector<YoloHead>& heads, const Letterbox& transform,
                                      float score_threshold=0.01f, float nms_iou=0.45f,
                                      bool person_only=true);
}

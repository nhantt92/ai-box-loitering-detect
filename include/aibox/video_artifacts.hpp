#pragma once

#include <opencv2/core.hpp>

#include <algorithm>
#include <cstdlib>

namespace aibox {

// Allwinner's decoded RTSP output can contain a short, uniform magenta band
// at the bottom. Only treat it as padding when almost every sampled pixel in
// consecutive bottom rows has the same saturated pink/magenta signature.
inline int magenta_bottom_band_rows(const cv::Mat& bgr) {
  if (bgr.empty() || bgr.type() != CV_8UC3 || bgr.rows < 32 || bgr.cols < 32)
    return 0;

  const int max_scan = std::min({bgr.rows * 6 / 100, 64});
  const int step = std::max(1, bgr.cols / 256);
  int band_rows = 0;
  for (int y = bgr.rows - 1; y >= bgr.rows - max_scan; --y) {
    int pink = 0;
    int samples = 0;
    for (int x = 0; x < bgr.cols; x += step) {
      const cv::Vec3b pixel = bgr.at<cv::Vec3b>(y, x);
      const int blue = pixel[0];
      const int green = pixel[1];
      const int red = pixel[2];
      if (blue >= 170 && red >= 170 && green >= 65 && green <= 225 &&
          std::abs(blue - red) <= 90)
        ++pink;
      ++samples;
    }
    if (samples == 0 || pink * 100 < samples * 94) break;
    ++band_rows;
  }
  return band_rows >= 4 ? band_rows : 0;
}

}  // namespace aibox

#include "aibox/preview_publisher.hpp"

#include <opencv2/imgcodecs.hpp>
#include <cassert>
#include <chrono>

using namespace aibox;
using namespace std::chrono_literals;

namespace {
void publisher_encodes_once_and_shares_latest_bytes() {
  PreviewConfig config;
  config.fps = 5;
  config.max_width = 320;
  config.quality = 75;
  config.max_clients = 3;
  PreviewPublisher publisher(config);

  auto first_frame = std::make_shared<AnalysisFrame>();
  first_frame->bgr = cv::Mat(240, 640, CV_8UC3, cv::Scalar(20, 40, 60));
  first_frame->tracks.push_back({7, 0, {100, 50, 120, 160}, .92f, true, {}, {}});
  first_frame->zones.push_back({"entry", "Entry",
      {{.1, .1}, {.9, .1}, {.9, .9}, {.1, .9}}, 10, true});
  first_frame->signals.push_back({7, "loitering", "entry", "Entry", true,
                                  true, 11, 0, "seconds"});
  publisher.submit(first_frame);

  auto first = publisher.wait_after(0, 2s);
  assert(first && first->sequence == 1);
  assert(first->bytes && first->bytes->size() > 100);
  assert((*first->bytes)[0] == 0xff && (*first->bytes)[1] == 0xd8);
  const cv::Mat decoded = cv::imdecode(*first->bytes, cv::IMREAD_COLOR);
  assert(decoded.cols == 320 && decoded.rows == 120);
  assert(publisher.max_clients() == 3 && publisher.healthy());

  auto second_frame = std::make_shared<AnalysisFrame>(*first_frame);
  second_frame->frame_id = 2;
  second_frame->bgr = cv::Mat(240, 640, CV_8UC3, cv::Scalar(80, 40, 20));
  publisher.submit(second_frame);
  auto second = publisher.wait_after(first->sequence, 2s);
  assert(second && second->sequence > first->sequence);
  auto same = publisher.latest();
  assert(same == second && same->bytes == second->bytes);
}

void latest_frame_replaces_backlog_and_invalid_frame_reports_error() {
  PreviewConfig config;
  config.fps = 2;
  config.max_width = 320;
  PreviewPublisher publisher(config);
  auto invalid = std::make_shared<AnalysisFrame>();
  publisher.submit(invalid);
  const auto deadline = std::chrono::steady_clock::now() + 1s;
  while (publisher.healthy() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(5ms);
  assert(!publisher.healthy());
  assert(publisher.last_error().find("BGR frame") != std::string::npos);

  auto newest = std::make_shared<AnalysisFrame>();
  newest->frame_id = 99;
  newest->bgr = cv::Mat(180, 320, CV_8UC3, cv::Scalar(1, 2, 3));
  publisher.submit(newest);
  const auto recovered = publisher.wait_after(0, 2s);
  assert(recovered && publisher.healthy());
}
}  // namespace

int main() {
  publisher_encodes_once_and_shares_latest_bytes();
  latest_frame_replaces_backlog_and_invalid_frame_reports_error();
}

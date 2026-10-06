#pragma once
#include "aibox/analysis_frame.hpp"
#include <memory>
#include <string>
namespace aibox {
class HdmiRenderer {
 public:
  HdmiRenderer();
  ~HdmiRenderer();
  HdmiRenderer(const HdmiRenderer&)=delete;
  HdmiRenderer& operator=(const HdmiRenderer&)=delete;
  bool available() const;
  void close_display();
  void present(const std::shared_ptr<const AnalysisFrame>& frame,bool camera_connected,double ai_fps,const std::string& message);
  bool pump_events();
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}

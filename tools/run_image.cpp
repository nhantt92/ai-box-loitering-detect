#include "aibox/awnn_detector.hpp"
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <iostream>
#include <stdexcept>
int main(int argc,char**argv){
 if(argc<3){std::cerr<<"Usage: aibox-image-regression MODEL.nb INPUT_IMAGE [OUTPUT_IMAGE]\n";return 2;}
 try{cv::Mat image=cv::imread(argv[2],cv::IMREAD_COLOR);if(image.empty())throw std::runtime_error("Could not read input image");aibox::AwnnDetector detector(argv[1]);auto detections=detector.detect(image,0.01f);for(const auto&d:detections){cv::rectangle(image,cv::Rect(cv::Point(static_cast<int>(d.box.x),static_cast<int>(d.box.y)),cv::Point(static_cast<int>(d.box.x+d.box.w),static_cast<int>(d.box.y+d.box.h))),cv::Scalar(0,220,0),2);std::cout<<"class="<<d.class_id<<" confidence="<<d.confidence<<" box="<<d.box.x<<","<<d.box.y<<","<<d.box.w<<","<<d.box.h<<"\n";}const char*out=argc>3?argv[3]:"result-aibox.png";if(!cv::imwrite(out,image))throw std::runtime_error("Could not write output image");std::cout<<"detections="<<detections.size()<<" output="<<out<<"\n";return 0;}catch(const std::exception&e){std::cerr<<"inference failed: "<<e.what()<<"\n";return 1;}
}

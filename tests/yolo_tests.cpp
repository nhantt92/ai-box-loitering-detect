#include "aibox/yolo.hpp"
#include <cassert>
#include <vector>
using namespace aibox;
int main(){
    cv::Mat wide(360,640,CV_8UC3,cv::Scalar(10,20,30)); auto a=prepare_yolov5(wide);assert(a.chw.size()==3u*640*640);assert(a.transform.resized_width==640&&a.transform.resized_height==360&&a.transform.pad_top==140);const size_t pixel=static_cast<size_t>(140)*640;assert(a.chw[pixel]==30&&a.chw[640u*640u+pixel]==20&&a.chw[2u*640u*640u+pixel]==10);assert(a.chw[static_cast<size_t>(139)*640]==0);
    cv::Mat tall(640,320,CV_8UC3,cv::Scalar(1,2,3));auto b=prepare_yolov5(tall);assert(b.transform.resized_width==320&&b.transform.resized_height==640&&b.transform.pad_left==160);
    std::vector<float> p8(3*80*80*85),p16(3*40*40*85),p32(3*20*20*85);auto heads=std::vector<YoloHead>{{8,p8.data(),p8.size()},{16,p16.data(),p16.size()},{32,p32.data(),p32.size()}};
    // High logits at the last candidate verifies that the final tensor cell is considered safely.
    const size_t end=p32.size()-85; p32[end+0]=10;p32[end+1]=10;p32[end+2]=0;p32[end+3]=0;p32[end+4]=10;p32[end+5]=10;
    auto ds=decode_yolov5(heads,b.transform,0.5f,0.45f,true);assert(!ds.empty());assert(ds.front().class_id==0);
    bool rejected=false;try{heads[0].elements-=1;decode_yolov5(heads,b.transform);}catch(const std::invalid_argument&){rejected=true;}assert(rejected);
}

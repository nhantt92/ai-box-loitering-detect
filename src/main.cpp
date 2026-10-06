#include "aibox/renderer.hpp"
#include "aibox/tracker.hpp"
#include "aibox/config.hpp"
#include "aibox/behavior.hpp"
#include "aibox/event_store.hpp"
#include "aibox/preview_publisher.hpp"
#include "aibox/core_api.hpp"
#include "aibox/snapshot_writer.hpp"
#include "aibox/video_source.hpp"
#ifdef AIBOX_WITH_AWNN
#include "aibox/awnn_detector.hpp"
#endif
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace {
std::atomic<bool> running{true};
std::atomic<bool> camera_connected{false};
std::atomic<std::uint64_t> total_frames{0};
std::atomic<double> ai_fps{0.0};
std::atomic<double> latest_inference_ms{0.0};
std::shared_ptr<const aibox::AnalysisFrame> latest_frame;
std::mutex message_mutex;
std::string status_message;
std::string storage_message;
void stop_signal(int){running=false;}
 aibox::ByteTrackConfig to_tracker_config(const aibox::TrackingSettings& settings){
  aibox::ByteTrackConfig config;
  config.low_confidence=settings.low_confidence;config.high_confidence=settings.high_confidence;
  config.new_track_confidence=settings.new_track_confidence;
  config.first_match_iou=settings.first_match_iou;config.second_match_iou=settings.second_match_iou;
  config.unconfirmed_match_iou=settings.unconfirmed_match_iou;
  config.track_ttl_seconds=settings.track_ttl_seconds;config.max_predict_seconds=settings.max_predict_seconds;
  config.process_noise=settings.process_noise;config.measurement_noise=settings.measurement_noise;
  config.min_hits=settings.min_hits;return config;
 }
void set_message(const std::string& value){std::lock_guard<std::mutex> lock(message_mutex);status_message=value;}
std::string get_message(){std::lock_guard<std::mutex> lock(message_mutex);return status_message;}
void set_storage_message(const std::string& value){std::lock_guard<std::mutex> lock(message_mutex);storage_message=value;}
std::string get_display_message(){std::lock_guard<std::mutex> lock(message_mutex);if(status_message.empty())return storage_message;if(storage_message.empty())return status_message;return status_message+" | "+storage_message;}
void wait_for_retry_or_config(std::chrono::seconds duration,
    const std::shared_ptr<aibox::RuntimeConfigController>& config, int revision){
 const auto end=std::chrono::steady_clock::now()+duration;
 while(running&&std::chrono::steady_clock::now()<end&&
       config->current().revision==revision)
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}
void report_events(const std::vector<aibox::EventTransition>& transitions,
                   aibox::EventStore* store, aibox::SnapshotWriter* snapshots,
                   const cv::Mat* snapshot_frame){
 for(const auto& transition:transitions){
  const auto& event=transition.event;
  std::cout<<"event="<<(transition.started?"started":"ended")<<" type="<<event.type<<" id="<<event.id<<" track="<<event.track_id;
  if(!event.zone_id.empty())std::cout<<" zone="<<event.zone_id;
  if(!transition.started)std::cout<<" reason="<<event.reason;
  std::cout<<"\n";
  if(!store)continue;
  if(!store->enqueue(transition)){
   set_storage_message("Event storage queue full; transition not saved");
   std::cerr<<"Event storage queue full; transition not saved\n";
   continue;
  }
  if(transition.started&&snapshots&&snapshot_frame&&!snapshots->enqueue(event.id,*snapshot_frame)){
   set_storage_message("Snapshot queue full; event image was not saved");
   std::cerr<<"Snapshot queue full; event image was not saved\n";
  }
 }
}

void inference_worker(std::shared_ptr<aibox::RuntimeConfigController> config_controller,
                      aibox::PreviewPublisher& preview,
                      aibox::EventStore* event_store,aibox::SnapshotWriter* snapshots){
 auto runtime_config=config_controller->current();
 auto behavior=std::make_unique<aibox::BehaviorCoordinator>(runtime_config);
 int behavior_revision=runtime_config.revision;
#ifdef AIBOX_WITH_AWNN
 const std::string model=AIBOX_DEFAULT_MODEL_PATH;
 std::unique_ptr<aibox::AwnnDetector> detector;
 try{detector=std::make_unique<aibox::AwnnDetector>(model);}catch(const std::exception&e){set_message(std::string("NPU/model error: ")+e.what());std::cerr<<get_message()<<"\n";running=false;return;}
#else
 std::cerr<<"AIBOX_WITH_AWNN is disabled: RTSP capture is active, inference is unavailable.\n";
#endif
 std::chrono::seconds backoff(1);auto report=std::chrono::steady_clock::now();auto report_frames=std::uint64_t{0};aibox::ByteTracker tracker(to_tracker_config(runtime_config.tracking));
 while(running){
  runtime_config=config_controller->current();
  if(behavior_revision!=runtime_config.revision){
   behavior=std::make_unique<aibox::BehaviorCoordinator>(runtime_config);
   tracker=aibox::ByteTracker(to_tracker_config(runtime_config.tracking));
   behavior_revision=runtime_config.revision;
  }
  bool config_changed=false;
  try{
   report_events(behavior->reset("camera_disconnected",std::chrono::steady_clock::now()),event_store,snapshots,nullptr);
   std::cerr<<"Opening configured RTSP input; credentials are not printed.\n";
   aibox::GstRtspSource camera(runtime_config.camera_url,runtime_config.decoder);
   camera.start();tracker.reset();set_message("Waiting for camera frames");const auto connected_at=std::chrono::steady_clock::now();auto last_seen=connected_at;
   while(running){
   if(config_controller->current().revision!=runtime_config.revision){
     auto updated_config=config_controller->current();
     if(updated_config.camera_url!=runtime_config.camera_url||
        updated_config.decoder!=runtime_config.decoder){
      set_message("Applying updated camera input");config_changed=true;break;
     }
     report_events(behavior->reset("config_changed",std::chrono::steady_clock::now()),
                   event_store,snapshots,nullptr);
     runtime_config=std::move(updated_config);
     behavior=std::make_unique<aibox::BehaviorCoordinator>(runtime_config);
     tracker=aibox::ByteTracker(to_tracker_config(runtime_config.tracking));
     behavior_revision=runtime_config.revision;
     std::cerr<<"Applied analytics settings without reconnecting the camera.\n";
    }
    auto frame=camera.pull(std::chrono::seconds(1));
    if(!frame){if(std::chrono::steady_clock::now()-last_seen>=std::chrono::seconds(10)){const auto err=camera.last_error();set_message(err.empty()?"Camera stream timeout":err);std::cerr<<"Camera stream timeout"<<(err.empty()?"":": "+err)<<"\n";break;}continue;}
    last_seen=frame->received;camera_connected=true;set_message("");
    auto result=std::make_shared<aibox::AnalysisFrame>();result->bgr=std::move(frame->bgr);result->frame_id=frame->frame_id;result->session_id=frame->session_id;result->received=frame->received;
#ifdef AIBOX_WITH_AWNN
    const auto infer_start=std::chrono::steady_clock::now();result->people=detector->detect(result->bgr,runtime_config.model.score_threshold,runtime_config.model.nms_iou);result->inference_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-infer_start).count();
#endif
    result->tracks=tracker.update(result->people,result->received);
    latest_inference_ms.store(result->inference_ms,std::memory_order_relaxed);
    result->zones=runtime_config.zones;
    const auto behavior_update=behavior->update(result->tracks,result->received,result->session_id,result->bgr.cols,result->bgr.rows);
    result->signals=behavior_update.signals;
    report_events(behavior_update.events,event_store,snapshots,&result->bgr);
    if(event_store&&!event_store->healthy())set_storage_message("Event storage degraded: "+event_store->last_error());
    else if(snapshots&&!snapshots->healthy())set_storage_message("Snapshot storage degraded: "+snapshots->last_error());
    else if(event_store&&snapshots)set_storage_message("");
    total_frames.fetch_add(1,std::memory_order_relaxed);std::atomic_store_explicit(&latest_frame,std::shared_ptr<const aibox::AnalysisFrame>(result),std::memory_order_release);preview.submit(result);
    const auto now=std::chrono::steady_clock::now();if(now-report>=std::chrono::seconds(5)){const auto n=total_frames.load(std::memory_order_relaxed);ai_fps.store(static_cast<double>(n-report_frames)/std::chrono::duration<double>(now-report).count(),std::memory_order_relaxed);report_frames=n;report=now;
#ifdef AIBOX_WITH_AWNN
     std::cout<<"session="<<result->session_id<<" frame="<<result->frame_id<<" people="<<result->people.size()<<" inference_ms="<<result->inference_ms<<" received_frames="<<n<<"\n";
#else
     std::cout<<"session="<<result->session_id<<" frame="<<result->frame_id<<" image="<<result->bgr.cols<<"x"<<result->bgr.rows<<" received_frames="<<n<<"\n";
#endif
    }
    if(frame->received-connected_at>=std::chrono::seconds(10))backoff=std::chrono::seconds(1);
   }
  }catch(const std::exception&e){const std::string msg=e.what();set_message(msg);std::cerr<<"Camera unavailable: "<<msg<<"\n";if(msg.find("Required GStreamer element unavailable")!=std::string::npos||msg.find("Invalid RTSP URI")!=std::string::npos){running=false;break;}}
  report_events(behavior->reset(config_changed?"config_changed":"camera_disconnected",std::chrono::steady_clock::now()),event_store,snapshots,nullptr);
  camera_connected=false;if(!running)break;if(config_changed){tracker.reset();backoff=std::chrono::seconds(1);continue;}wait_for_retry_or_config(backoff,config_controller,runtime_config.revision);if(config_controller->current().revision!=runtime_config.revision){backoff=std::chrono::seconds(1);continue;}backoff=std::min(backoff*2,std::chrono::seconds(30));
 }
 camera_connected=false;if(event_store)event_store->flush();
}
}

int main(){
 std::signal(SIGINT,stop_signal);std::signal(SIGTERM,stop_signal);
 const char* config_env=std::getenv("AIBOX_CONFIG_PATH");const std::string config_path=config_env&&*config_env?config_env:"config/example.json";
 aibox::RuntimeConfig runtime_config;
 try{runtime_config=aibox::load_runtime_config(config_path);}catch(const std::exception&e){std::cerr<<"Config error: "<<e.what()<<"\n";return 1;}
 const char* uri_env=std::getenv("AIBOX_RTSP_URL");
 if(uri_env&&*uri_env)runtime_config.camera_url=uri_env;
 auto config_controller=std::make_shared<aibox::RuntimeConfigController>(runtime_config,config_path);
 const char* data_env=std::getenv("AIBOX_DATA");const std::string data_dir=data_env&&*data_env?data_env:"data";
 const char* db_env=std::getenv("AIBOX_EVENT_DB");const std::string db_path=db_env&&*db_env?db_env:data_dir+"/events.sqlite3";
 std::shared_ptr<aibox::EventStore> event_store;
 try{event_store=std::make_shared<aibox::EventStore>(db_path);}catch(const std::exception&e){set_storage_message(std::string("Event storage unavailable: ")+e.what());std::cerr<<"Event storage unavailable: "<<e.what()<<"\n";}
 std::unique_ptr<aibox::SnapshotWriter> snapshots;
 if(event_store){try{snapshots=std::make_unique<aibox::SnapshotWriter>(*event_store,data_dir,runtime_config.storage);}catch(const std::exception&e){set_storage_message(std::string("Snapshot storage unavailable: ")+e.what());std::cerr<<"Snapshot storage unavailable: "<<e.what()<<"\n";}}
 aibox::PreviewPublisher preview(runtime_config.preview);
 std::unique_ptr<aibox::CoreApiServer> api;
 try{api=std::make_unique<aibox::CoreApiServer>(preview,config_controller,event_store,[&]{
  aibox::CoreStatus status;status.camera_connected=camera_connected.load(std::memory_order_relaxed);status.ai_fps=ai_fps.load(std::memory_order_relaxed);status.frame_id=total_frames.load(std::memory_order_relaxed);const auto frame=std::atomic_load_explicit(&latest_frame,std::memory_order_acquire);if(frame)status.session_id=frame->session_id;if(frame&&status.camera_connected){status.person_detections=frame->people.size();for(const auto& track:frame->tracks)if(track.class_id==0&&track.observed)++status.person_tracks;for(const auto& signal:frame->signals)if(signal.type=="loitering")status.loitering.push_back(signal);}const auto active_zones=config_controller->current().zones;for(const auto& zone:active_zones)if(zone.enabled)++status.enabled_zones;status.inference_ms=latest_inference_ms.load(std::memory_order_relaxed);status.message=get_message();status.storage_state=event_store?(event_store->healthy()&&snapshots&&snapshots->healthy()?"ok":"degraded"):"unavailable";return status;
 },data_dir);std::cerr<<"Core API listening on 127.0.0.1:"<<api->port()<<"; token file: "<<api->token_file()<<"\n";}catch(const std::exception&e){set_message(std::string("Core API unavailable: ")+e.what());std::cerr<<"Core API unavailable: "<<e.what()<<"; continuing analytics and HDMI\n";}
 aibox::HdmiRenderer display;
 if(!display.available())std::cerr<<"Continuing headless; analytics worker is independent of HDMI.\n";
 std::thread worker(inference_worker,config_controller,std::ref(preview),event_store.get(),snapshots.get());
 int applied_aux_config_revision=config_controller->current().revision;
 auto last_render=std::chrono::steady_clock::now();
 while(running){
  const auto aux_config=config_controller->current();
  if(aux_config.revision!=applied_aux_config_revision){
   preview.configure(aux_config.preview);
   if(snapshots)snapshots->configure(aux_config.storage);
   applied_aux_config_revision=aux_config.revision;
  }
  display.pump_events();
  if(display.available()&&std::chrono::steady_clock::now()-last_render>=std::chrono::milliseconds(50)){
   auto frame=std::atomic_load_explicit(&latest_frame,std::memory_order_acquire);
   display.present(frame,camera_connected.load(std::memory_order_relaxed),ai_fps.load(std::memory_order_relaxed),get_display_message());last_render=std::chrono::steady_clock::now();
  }else if(!display.available())std::this_thread::sleep_for(std::chrono::milliseconds(100));
 }
 worker.join();return 0;
}

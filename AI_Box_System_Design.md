# Thiết kế AI Box Loitering / Running — Orange Pi Zero 3W A733

## 1. Phạm vi và cơ sở

Một camera RTSP cố định; phát hiện người bằng model YOLOv5 .nb trên NPU; tracking chỉ phục vụ Loitering và fast_motion/Running. Hiển thị HDMI đồng thời có web xem live, lịch sử và cấu hình. Chưa có PTZ, nhận diện danh tính, nhiều camera, MQTT hay clip trước/sau sự kiện.

Tại thời điểm lập thiết kế, người dùng xác nhận: RAM 4 GB, Orange Pi Bullseye (Debian 11), GStreamer 1.18.3, model v3 .nb chạy ảnh, pipeline RTSP H264 omxh264dec videoconvert autovideosink chạy. Cập nhật tiến độ hiện tại ở [docs/board-integration.md](docs/board-integration.md): người dùng đã chạy NPU inference, RTSP và HDMI; tracker/rules/SQLite mới được phát triển sau lần xác nhận đó và cần build/verify riêng trên board. Không dùng thông tin Bookworm/chưa decode trong bản thiết kế cũ làm cơ sở.

## Implementation status — 2026-10-06

Host implementation now has timestamp-aware ByteTrack-style tracking, config-validated loitering/fast-motion orchestration, HDMI overlays, bounded SQLite event writes with restart recovery, asynchronous event snapshots with retention, one shared JPEG preview cache, a loopback core API, and a session-authenticated FastAPI proxy/dashboard. The authenticated System page supports editing a credential-free RTSP URL; core persists it atomically and reconnects the capture worker. Eight C++ test suites pass in Debug; four FastAPI integration tests pass in a temporary environment built from the pinned web requirements, and JavaScript syntax checks pass. The RTSP web-update path still needs a board build and live verification. The HDMI path uses GStreamer output confirmed by the user. Remaining target requirements include hot config apply for other fields, systemd packaging, and long-duration board acceptance.

Hardware decode không có nghĩa BGR conversion, resize, overlay hoặc HDMI đều được tăng tốc. Đo riêng các công đoạn; không cam kết FPS trước benchmark.

## 2. Các tiến trình

- ai-box-core, C++17: capture, NPU, ByteTrack, behavior, event manager, SQLite writer, overlay, JPEG publisher; chạy được headless.
- ai-box-web, Python/FastAPI, một worker: authentication, static web, API proxy và MJPEG fan-out. Không import detector/tracker và không mở RTSP.
- HDMI renderer GStreamer `kmssink` nằm trên main thread của core, xác nhận hoạt động trên board với `sunxi-drm`; fallback phụ thuộc sink/driver có sẵn. Nếu không có màn hình, core vẫn phục vụ web.

Core API chỉ bind 127.0.0.1:8091, cần token nội bộ. Web bind LAN port 8080. Web gọi core qua HTTP loopback; đây là lựa chọn triển khai, không phải phụ thuộc bắt buộc của thuật toán. Không mở core API ra LAN.

## 3. Pipeline và concurrency

RTSP TCP → depay H264 → parse → omxh264dec → videoconvert → BGR → appsink. Giữ max-buffers=1 drop=true sync=false, tương thích GStreamer 1.18; không dùng thuộc tính chỉ có trong bản mới.

Capture worker liên tục pull/map/copy frame vào LatestFrameSlot; AI worker lấy frame mới nhất chưa xử lý. Mỗi slot chứa immutable frame cùng frame_id, camera_id, session_id, monotonic receive time và PTS nếu có. Video mapping phải xử lý stride; không giữ pointer sau unmap.

GStreamer PTS thuộc timeline pipeline, không tự coi là UTC hay timestamp camera. Bản đầu dùng receive monotonic cho khoảng thời gian, giữ PTS để chẩn đoán. Khi gap lớn hoặc backlog bất thường, reset cửa sổ tốc độ. Độ trễ end-to-end cần phép đo camera riêng; processing_ms không thể thay thế.

AI worker: preprocess → NPU → person detections → tracker → rules → AnalysisFrame. Video, box, ID, footpoint và event overlay phải thuộc CÙNG frame_id; không vẽ box cũ lên video mới mà không có dấu stale.

Main thread hiển thị AnalysisFrame mới nhất. Publisher worker encode JPEG tối đa 5 FPS, một lần cho mọi web client; publisher/HDMI không chặn AI. Event writer có queue hữu hạn; event metadata cần backpressure có giới hạn hoặc trạng thái storage_degraded, không silently drop. Không queue dài cho video. Không ghi JPEG live mỗi frame lên thẻ SD.

Stream timeout 10 giây; reconnect có backoff 1/2/4/8… tối đa 30 giây và jitter. Khi reconnect: kết thúc event với camera_disconnected, reset tracker/rules, tăng session_id. Startup tạo boot UUID để tránh trùng ID qua reboot. Lỗi cấu hình/plugin vĩnh viễn chuyển degraded rõ nguyên nhân, tránh retry nóng.

## 4. Module và dữ liệu

| Module | Trách nhiệm |
|---|---|
| video_source | RTSP/file replay, timestamp, session, reconnect |
| npu_detector | AWNN adapter, model contract, preprocess/postprocess |
| person_tracker | ByteTrack, observed/predicted, TTL theo thời gian |
| loitering_engine | Polygon, dwell quan sát được, debounce |
| running_engine | Quỹ đạo, quality gate, speed/hysteresis |
| event_manager | Một begin/end cho một episode, UUID |
| storage | SQLite/snapshot/retention, recovery |
| config_manager | Validate, revisions, atomic apply/persist |
| render | Annotated frame + HDMI scaling/layout |
| preview_publisher | JPEG cache theo sequence |
| core_api / web | Control API, auth, dashboard |

Detection: bbox ảnh nguồn, confidence, class_id. Track: boot/session/track ID, bbox, footpoint, first_seen, last_observed, observed flag, trajectory. AnalysisFrame: frame metadata, tracks, active event IDs, config revision và image. Event: UUID, camera/session/track, zone, type, start/end UTC + monotonic duration, peak_speed và unit, dwell, end_reason, config_revision, snapshot status/path.

## 5. Detector / tracker

Giữ đúng preprocessing đã chạy: 640×640, RGB planar uint8, padding đen và đúng layout 3 output heads. Không đổi quantization/anchor/output ordering tùy tiện. Xác minh model là COCO và person=0 từ SDK/model metadata. Chỉ lọc người trước tracker; không dùng NMS khác lớp làm mất box người.

ByteTrack cần cả detection confidence thấp cho liên kết thứ hai. Giá trị thử: low=0.10, high=0.50, new_track=0.60, NMS IoU=0.45; đây không phải ngưỡng được xác nhận. Detector không được giữ hardcode 0.4 từ demo cũ. Hai-stage assignment + Kalman + lost/reactivated/removed; không đặt tên ByteTrack cho tracker chỉ IoU greedy.

Dùng nguồn C++ có provenance, commit pin và license. Adapter phải xử lý FPS biến động: timestamp cho Kalman dt/expiry hoặc giải thích và kiểm thử phép thích nghi. Lost TTL theo giây thay vì số frame capture danh nghĩa. Không tính dwell/speed từ predicted-only points. Không coi track ID là danh tính người.

## 6. Loitering

Định nghĩa: một người ở trong polygon quá thời gian cho phép. Polygon lưu normalized [0,1], ít nhất 3 đỉnh, không tự cắt, không suy biến; footpoint = giữa cạnh dưới bbox. Khi stream aspect ratio/scene thay đổi phải yêu cầu xác nhận ROI/calibration, không tự coi còn đúng.

Mặc định thử: enter_confirm=1s, dwell_threshold=60s, exit_confirm=1s, occlusion_grace=2s. Tích lũy dwell từ các interval hai đầu đều observed và inside hợp lệ, không bridge gap quan sát dài. Khi confirmed enter, có thể credit các interval quan sát inside trong giai đoạn Entering; nêu và kiểm thử chính sách này.

Outside → Entering → Inside → Alarm → Exiting → Ended. Boundary policy rõ ràng (boundary thuộc inside) và debounce giảm dao động. Trong Exiting không cộng outside interval; nếu quay vào trước confirm thì giữ episode. Occlusion tạm pause dwell, quá grace kết thúc track_lost. Reconnect/reset kết thúc với lý do riêng. Mỗi episode một started, một ended, không alarm mỗi frame. Một người có thể có event khác nhau ở các zone.

## 7. Running / fast_motion

Không có calibration: tốc độ hình ảnh theo chiều cao người/giây (body_heights_per_second), dùng median bbox height trong window làm chuẩn; vẫn chịu phối cảnh/bbox noise nên tên sự kiện fast_motion. UI ghi rõ Nghi ngờ chạy / Fast motion. Mặc định tính năng disabled cho đến khi người vận hành chọn ngưỡng từ clip đi bộ/đi nhanh/chạy.

Có calibration: ít nhất 4 cặp điểm không thẳng hàng trên mặt sàn với tọa độ thực mét, kiểm tra reprojection error, polygon vùng hiệu chuẩn và ánh xạ hữu hạn. Footpoint ngoài vùng hiệu chuẩn không được báo m/s. Homography chỉ áp dụng mặt phẳng sàn; cần kiểm chứng riêng ở khu vực dốc/bậc thang.

Window thử 1s, >=5 quan sát hợp lệ, span>=0.5s. Estimate velocity từ robust fit các footpoint theo timestamp sau lọc outlier, không cộng đường đi nhiễu ở người đứng yên. Loại jumps, bbox quá nhỏ, dt<=0, gap quá dài và đổi session. Không tính predicted points.

Ngưỡng start/end theo đúng unit của mode, end<start. Xác nhận start thử 0.8s, end 1s. Nếu mất quan sát ngắn giữ state nhưng không tăng confirm timer; quá grace kết thúc track_lost. Không đặt ngưỡng m/s chung mọi camera. Calibration/threshold đổi thì kết thúc active episode config_changed và reset cửa sổ.

## 8. HDMI và web

HDMI 1280×720 là layout tham chiếu; scale theo độ phân giải màn hình, giữ aspect ratio video. Video chiếm vùng chính; cạnh phải hiển thị số người, camera state, alarm active, event gần nhất; chân màn hình là IP/URL, FPS AI, NPU ms, trạng thái storage. Màu người bình thường xanh, Loitering đỏ, fast_motion cam; luôn có chữ/icon ngoài màu. Camera mất phải có màn hình cảnh báo và tuổi frame, không giữ hình cũ trông như live. Không phát beep phần cứng nếu chưa có thiết bị âm thanh.

Web dùng HTML/CSS/JS không CDN, giao diện tiếng Anh bản đầu và tách chuỗi để đổi tiếng Việt sau. Các trang:

1. Live: MJPEG annotated, status 1Hz, zone overlay, active alarms.
2. Zones: frozen frame editor, polygon normalized, enable/disable, tên/ngưỡng từng zone, xác nhận apply. Map mouse/touch đúng phần ảnh sau letterbox CSS.
3. Running: mode, thresholds/units, window/confirm, calibration point editor và status validated/unconfigured.
4. Camera/System: camera URL write-only, decoder hardware/software, AI rate cap, HDMI, preview FPS/quality, storage limits.
5. Events: phân trang/filter type/date/zone/state, snapshot, end reason, CSV export.

Live dùng MJPEG JPEG cache tối đa 5 FPS, 640px width, quality 70 làm cấu hình benchmark ban đầu. Web status polling 1Hz; chưa cần WebRTC/HLS/WebSocket. Tối đa 3 client thử, giới hạn rõ và disconnect chậm; không encode mỗi client. Nếu web restart, core tiếp tục AI/HDMI. FastAPI chỉ một worker tránh nhân bản publisher/cache.

## 9. API và áp dụng config

| Public route | Ý nghĩa |
|---|---|
| POST /api/login, POST /api/logout | Session auth |
| GET /api/status | Health, metrics, current revision |
| GET /api/config | Effective config (no credentials are stored in camera URL) |
| PUT /api/config/camera | RTSP URL + expected revision; validate, persist, reconnect |
| PUT /api/camera/credentials | Secret write-only, không echo |
| GET /api/config/jobs/{id} | Trạng thái apply/reconnect |
| GET /api/frame.jpg | JPEG cache + sequence |
| GET /api/stream.mjpg | MJPEG fan-out cùng cache |
| GET /api/events | Pagination/filter |
| GET /api/events/{id}/snapshot | Lookup path từ event UUID |
| GET /api/events.csv | Export giới hạn phạm vi |

Rules validation: finite numbers, valid polygon, sensible limits, speed units, threshold ordering, preview/client caps, HTTP body size. Reject unknown keys. Optimistic concurrency: revision mismatch 409. Schema errors 422. API core và web dùng một schema/contract; C++ core là authority cuối cùng.

PUT tạo validated config job (202) nếu cần apply tại frame boundary/reconnect. Distinguish desired_revision và effective_revision. Luật/ROI đổi: end event config_changed, reset affected state, apply giữa frame. RTSP/decoder đổi: stop source, new session; model/input contract đổi cần controlled restart riêng. HDMI on/off do main thread quản lý. Preview/storage limits hot apply. Không restart Linux.

Core serialize tất cả update, atomic write temp+fsync+rename, permissions 0600 cho secrets, giữ last-known-good. Xác định success khi cấu hình hiệu lực và lưu thành công. Nếu camera mới sai, status hiển thị desired nhưng disconnected; UI có rollback thao tác, không tự ghi đè ngược với update mới hơn. Job fail phải báo rõ không quảng cáo success.

## 10. Persistence và bảo vệ truy cập

SQLite một writer trong core, WAL, busy timeout, transaction ngắn; web chỉ đọc qua core API. Snapshot encode/write ngoài AI path qua queue hữu hạn, atomic rename, DB có pending/ready/failed/deleting/expiring/expired. Startup reconcile event còn active thành ended/process_restarted và sửa snapshot pending/deleting/expiring. Retention xóa snapshot+metadata nhất quán theo retention_days và max_snapshot_bytes; ảnh của event active có thể bị hết hạn để thỏa quota trong khi giữ metadata đang hoạt động. Disk low báo degraded, ưu tiên metadata, không làm core crash. DB/WAL trên local filesystem, không network share.

LAN cần login cho cả live/stream/config/event/snapshot. Setup initial admin password một lần qua CLI, lưu hash, không default password. HttpOnly SameSite session cookie, CSRF/Origin cho mutations, rate limit login, giới hạn session/clients. HTTP dùng cho LAN test; nếu truy cập mạng không tin cậy dùng HTTPS reverse proxy/VPN, không port-forward bản HTTP này. Không log/echo URL password; URL trên UI redacted, update secret tách riêng. Snapshot endpoint không nhận arbitrary filesystem path.

## 11. Build / vận hành

CMake C++17, OpenCV, GStreamer app/video, SDL2, SQLite, Eigen/ByteTrack dependency, JSON/HTTP library pin commit/license. Python virtualenv, pin version đã kiểm thử tương thích Python 3.9 Bullseye; không dùng syntax chỉ Python 3.10+. Không đóng gói SDK bí mật hoặc tự tạo AWNN symbols giả.

Core headless service khởi động lúc boot. HDMI qua user desktop autostart hoặc service tích hợp đúng graphical session, cùng core single-instance lock; hai mode exclusive. Nếu cần HDMI trước login phải làm riêng display backend/session, không giả định SDL2 DRM/KMS có sẵn. Web system service restart độc lập; static UI phục vụ local.

Data /var/lib/ai-box; config /etc/ai-box; state lock/runtime /run/ai-box. Chạy non-root với nhóm quyền NPU/video thực tế; khảo sát device node trước khi viết udev. Log rotation và systemd restart/backoff. Không dùng sleep dài hoặc unlimited retry/error log. Không gọi shell với dữ liệu web.

## 12. Tiêu chí nghiệm thu

- Demo dog.jpg vẫn chạy; box non-square đúng; source RGB/CHW/output đầu-cuối đúng.
- Replay deterministic với fake clock: dwell, debounce, occlusion, lost, reconnect và config-change không trùng event.
- Tracker low confidence association, crossing/occlusion, variable dt/ID reset; đo thực tế, không tuyên bố ID tuyệt đối ổn định.
- Người đứng yên có bbox jitter không báo fast_motion; đi nhanh/chạy kiểm chứng từ clip real; calibration known distance có sai số được ghi nhận.
- HDMI và web cùng frame/result; 3 web clients không nhân inference, slow client không chặn AI.
- Invalid ROI/config không đổi effective state; concurrent update 409; secrets không nằm response/log; unauthorized stream/mutation bị từ chối.
- Mất camera/recover, mất HDMI, restart web, storage full, crash recovery và graceful shutdown.
- Burn-in 8 giờ rồi 24 giờ trên board: RAM, nhiệt, CPU, AI FPS, dropped frames, read-to-analysis delay, inference time, recovery time. Mục tiêu AI >=5 FPS và read-to-analysis p95<500ms chỉ là mục tiêu thử ở substream 640×360, không phải hiệu năng đã đo. Dùng fake-clock tests thay vì chờ đủ 60 giây.

## Nguồn kỹ thuật

- GStreamer appsink queue/drop: https://gstreamer.freedesktop.org/documentation/application-development/advanced/pipeline-manipulation.html
- ByteTrack upstream + paper: https://github.com/FoundationVision/ByteTrack
- SDL2 RenderPresent main-thread requirement: https://wiki.libsdl.org/SDL2/SDL_RenderPresent
- SQLite isolation/WAL: https://www.sqlite.org/isolation.html
- FastAPI streaming responses: https://fastapi.tiangolo.com/advanced/custom-response/

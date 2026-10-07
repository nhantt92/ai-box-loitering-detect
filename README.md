# AI Box Loitering / Fast Motion

This repository implements the Orange Pi Zero 3W A733 AI Box pipeline in milestones. The supplied SDK YOLOv5 demo is at `/home/nhantt/2026/docker_images_v2.0.x/docker_data/ai-sdk/examples/yolov5`; its contracts informed the OpenCV preprocessing and decoder. The user has confirmed NPU inference, RTSP video and HDMI output on the target board; the new tracker, behavior and SQLite changes still need board verification. See [board integration status](docs/board-integration.md).

## Build rule engine

```sh
cmake -S . -B build -DAIBOX_WITH_AWNN=OFF -DAIBOX_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Requires CMake 3.16+, a C++17 compiler, OpenCV, GStreamer development headers, `nlohmann-json3-dev`, and `libsqlite3-dev`. The CPU build captures video but does not fabricate detections. It runs geometry/rule, timestamp-aware tracking, runtime config parsing and event persistence tests without the vendor NPU SDK.

Run the authenticated web proxy integration checks in the web virtualenv with `python tests/test_web_app.py`; they start a local mock core API and cover login gating, camera/behavior config proxying, JSON/CSV/image proxying and MJPEG relay.

For an AArch64 Linux toolchain that matches the Orange Pi image and SDK ABI, enable the real adapter with `-DAIBOX_WITH_AWNN=ON -DAIBOX_AWNN_SDK_ROOT=/home/nhantt/2026/docker_images_v2.0.x/docker_data/ai-sdk`. The supplied VIP shared libraries are AArch64; a native x86_64 link is expected to fail. The adapter checks NBG input/output shapes and format through VIP. The user has already run the board image regression successfully and reported two detections from `dog.jpg`; repeat it after code changes using [deploy-orange-pi.md](docs/deploy-orange-pi.md).

For deployment and a manual board test on `192.168.1.34`, follow [deploy-orange-pi.md](docs/deploy-orange-pi.md).

## RTSP capture input

`ai-box-core` loads `config/example.json` by default; set `AIBOX_CONFIG_PATH` to another JSON file. The default source is `rtsp://192.168.1.6:554/profile2`. In **Camera / System**, select RTSP/RTSPS, HTTP(S) video URL, or a local video file on the Orange Pi. RTSP keeps the tested H.264 hardware decoder path; URI/file sources use GStreamer's `uridecodebin` and require installed demux/decoder plugins. Local files loop at end of stream. Source username/password are separate fields; the password is never returned by the API and the core writes it to the config file with mode `0600`. Legacy `AIBOX_RTSP_USER` and `AIBOX_RTSP_PASSWORD` are startup fallbacks only until credentials are explicitly managed; saving the camera settings, including clearing username/password, records that choice and suppresses both fallbacks after restart. `AIBOX_RTSP_URL` is ignored so a saved web source always wins. Decoder choices for RTSP are `omxh264dec` and `avdec_h264`; the latter requires the GStreamer libav plugin. RTSP capture runs independently from NPU inference. A leaky queue directly after decode and a latest-frame capture mailbox each keep only the newest frame, so a slow inference cycle does not accumulate display latency. Preview hides detection boxes more than 750 ms behind the camera frame and reports a stale overlay, rather than showing boxes on unrelated current pixels. AI FPS remains the measured inference rate and can be lower than camera/display FPS. The TCP pipeline defaults to 200 ms jitter latency; set `AIBOX_RTSP_LATENCY_MS` between 50 and 1000 ms to tune buffering. Camera source/credential changes reconnect; analytics changes reset only affected tracking/behavior state, while preview and storage changes preserve tracks and dwell.

The web preview uses an independent JPEG worker with a latest-frame schedule; encode time no longer adds a full extra FPS interval, and slow encodes replace pending frames rather than building latency. Defaults are 1280 px, JPEG quality 82 and 10 FPS. Existing saved preview settings are retained; change them on **Tracking / Performance**. The Live page reports the actual encoded image size so a low-resolution camera profile can be distinguished from web scaling.

With AWNN enabled on the matching AArch64 SDK build, detections enter the tracker and behavior coordinator. The YOLOv5 model outputs the 80 COCO classes; the decoder defaults to class ID 0 (`person`) and the authenticated **Model & AI** page lets you select any subset, including all classes. Selected objects are tracked and labeled in the web/HDMI preview. ByteTrack preserves IDs on short detection gaps and can re-associate a detection using gated Kalman center distance when predicted boxes briefly stop overlapping. Unobserved tracks appear as dashed `TEMP` boxes until the configured track TTL expires. Loitering retains the episode during its lost-track grace period but pauses dwell accumulation until the person is observed again. **Đám đông** adds per-enabled-zone people counts and a crowd event, with separate trigger/clear thresholds and confirmation intervals. Its default threshold is five people to trigger and three to clear. `config/example.json` intentionally contains no zone and disables behavior alerts by default. In the web dashboard, use **Vùng loitering** to draw normalized polygons directly over the live preview and set the dwell threshold; crowd detection uses these same zones. During dwell, the annotated preview reports progress; the loitering event begins after the configured dwell threshold. **Chuyển động nhanh** remains the existing fast-motion proxy and its `running` config/event semantics are preserved. **Phát hiện người chạy** adds a separate `running_detection` configuration and `running` event, with independent speed thresholds and confirmation timing. Both motion rules estimate track speed in person-box heights per second; the YOLO person class does not classify gait, so running detection is a speed-based proxy and should be tuned against footage from the installed camera. Saving a zone or analytics setting recreates tracker/rule state without restarting RTSP; camera source changes reconnect the input.

Begin/end event transitions are written by a dedicated SQLite thread to `AIBOX_EVENT_DB`, or `$AIBOX_DATA/events.sqlite3`, defaulting to `./data/events.sqlite3`. SQLite uses WAL and the DB file is restricted to owner read/write. Active rows are closed with `process_restarted` during startup recovery. Event-start snapshots are JPEG encoded on a bounded background queue and written atomically under `$AIBOX_DATA/snapshots` with owner-only permissions. Retention removes expired events and enforces `retention_days` and `max_snapshot_bytes`; active event metadata is retained if its image must be evicted for quota. The core API exposes events, snapshots, effective configuration, cached JPEG and MJPEG on loopback only, protected by a 0600 token file. FastAPI proxies these routes after browser login; run both processes as the same board user. The web console provides Vietnamese and English, a responsive device overview, model/runtime profile metadata, confidence and NMS thresholds, camera source/decoder, loitering timing, polygon zones, separate fast-motion and running proxy rules, ByteTrack association parameters, web preview limits, snapshot retention, event history and snapshot links. The language preference is kept in the browser. Only the validated `yolov5-a733-640` profile is currently selectable by the runtime; the console presents it as a profile card and keeps model input/runtime metadata read-only. Adding another model requires registering and validating its preprocessing, tensor layout, output decoder and target runtime in the core before offering it in the UI.

## Web control process

Requires Python 3.9+ and the pinned dependencies in `web/requirements.txt` (including `httpx` for the core proxy):

```sh
python3 -m venv .venv
. .venv/bin/activate
pip install -r web/requirements.txt
AIBOX_DATA=./data uvicorn web.app:app --host 0.0.0.0 --port 8080 --workers 1
```

Create the first administrator from a local terminal with `AIBOX_DATA=./data python tools/setup_admin.py`. No default password is configured. A random session signing key is saved in `AIBOX_DATA/session.key` with mode 0600. Set `AIBOX_COOKIE_SECURE=1` when HTTPS is terminated by a trusted reverse proxy; the local HTTP test default leaves this flag off. Use HTTPS or a VPN outside a trusted LAN. Start `ai-box-core` before opening the dashboard. The web process proxies the local API and does not maintain a duplicate events database; the browser session does not receive the core bearer token.

## HDMI live display

`ai-box-core` sends annotated video through GStreamer. On the tested Orange Pi image it selects `kmssink` with the `sunxi-drm` driver for direct HDMI output; it falls back to `fbdevsink` or `autovideosink`. It shows observed tracked people with IDs, configured zones and active loitering/fast-motion alerts, plus camera and inference status. Press **Ctrl+C** in the SSH terminal to stop the process. Set `AIBOX_KMS_DRIVER` only when the board exposes a different DRM driver.

## Current boundaries

Implemented: strict startup config validation; C++ polygon geometry, loitering debounce/dwell, crowd occupancy with hysteresis and motion rules connected to timestamped tracks; event begin/end transitions; SQLite writer with recovery; YOLOv5 preprocessing/output decoding; timestamp-aware ByteTrack-style high/low confidence association with Kalman prediction, Hungarian matching and expiry; RTSP capture/reconnect with selectable H.264 decoder; AWNN inference; annotated HDMI zones/alerts. Host tests cover tracker crossing/low-confidence/prediction/TTL, behavior debounce/lost/reconnect/config parsing and SQLite recovery.

Still missing: camera calibration/homography, systemd packaging, and long-duration board acceptance of the recent behavior-config and web UI changes. See [board integration status](docs/board-integration.md) for what has actually been verified on the Orange Pi.

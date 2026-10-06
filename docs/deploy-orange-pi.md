# Manual test deployment to Orange Pi

These steps run the current core interactively; no systemd service is installed. They assume the board is Debian 11 Bullseye, AArch64, and reachable at `192.168.1.34`. Replace `<board-user>` with the SSH account configured on the board.

## Copy the project and required SDK files

Run from the development machine:

```sh
rsync -av --exclude='build*/' --exclude='.venv/' --exclude='data/' --exclude='__pycache__/' \
  --exclude='config/example.json' \
  /home/nhantt/2026/ai-box-loitering-detect/ \
  <board-user>@192.168.1.34:~/ai-box-loitering-detect/

rsync -av /home/nhantt/2026/docker_images_v2.0.x/docker_data/ai-sdk/examples/yolov5/ \
  <board-user>@192.168.1.34:~/ai-sdk/examples/yolov5/
rsync -av /home/nhantt/2026/docker_images_v2.0.x/docker_data/ai-sdk/examples/libawnn_viplite/ \
  <board-user>@192.168.1.34:~/ai-sdk/examples/libawnn_viplite/
rsync -av /home/nhantt/2026/docker_images_v2.0.x/docker_data/ai-sdk/viplite-tina/lib/aarch64-none-linux-gnu/v2.0/ \
  <board-user>@192.168.1.34:~/ai-sdk/viplite-tina/lib/aarch64-none-linux-gnu/v2.0/
```

If this SDK already exists on the board, use its path in the build command instead of copying it. Keep the same directory layout expected by `AIBOX_AWNN_SDK_ROOT`.

## Install build dependencies

SSH to the board and confirm the target architecture and decoder plugin:

```sh
ssh <board-user>@192.168.1.34
uname -m
gst-inspect-1.0 omxh264dec >/dev/null && echo 'hardware decoder plugin found'
```

`uname -m` should report `aarch64`. If packages are already installed, skip the install command.

```sh
sudo apt update
sudo apt install -y build-essential cmake pkg-config libopencv-dev \
  libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
  nlohmann-json3-dev libsqlite3-dev
```

Before configuring CMake, confirm the SQLite development files are present:

```sh
dpkg-query -W -f='${Status} ${Version}\n' libsqlite3-dev
test -f /usr/include/sqlite3.h && echo 'SQLite header found'
ldconfig -p | grep -m1 'libsqlite3.so'
```

If CMake reports `Could NOT find SQLite3 (missing: SQLite3_INCLUDE_DIR SQLite3_LIBRARY)`, `libsqlite3-dev` is missing or its installation did not complete. Install it and inspect the result before retrying configure:

```sh
sudo apt install libsqlite3-dev
dpkg-query -W -f='${Status} ${Version}\n' libsqlite3-dev
```

The runtime package `libsqlite3-0` alone is not enough; CMake also needs `/usr/include/sqlite3.h` and the development linker file. If APT reports dependency/version conflicts, inspect the exact candidates first instead of upgrading or downgrading unrelated packages:

```sh
apt-cache policy libsqlite3-0 libsqlite3-dev
apt-mark showhold
```

Bullseye LTS ended on 2026-08-31. During the archive transition, `security.debian.org` can still publish an index that points at package files which now return `404`. If that happens, use Debian Snapshot for the final Bullseye security state. This is a frozen historical repository and does not provide future security updates. Open the file:

```sh
sudoedit /etc/apt/sources.list
```

Replace the active `bullseye-security` line (whether its host is Huawei Cloud or `security.debian.org`) with:

```text
deb [check-valid-until=no] https://snapshot.debian.org/archive/debian-security/20260831T235959Z/ bullseye-security main contrib non-free
```

Then refresh, inspect the transaction, and install only if the simulation does not propose unexpected changes to board/vendor packages:

```sh
sudo apt clean
sudo apt update
apt-cache policy libsqlite3-0 libsqlite3-dev
sudo apt-get -s install libsqlite3-dev
```

If the simulation is limited to the SQLite development files and any required matching SQLite runtime library, run `sudo apt install libsqlite3-dev`. Do not accept an unexpected broad upgrade or downgrade; capture the simulation output for review first.

Only proceed to `cmake --build` and `ctest` after `cmake -S ...` exits successfully. A failed configure can leave an old build directory behind, and `ctest` may then report no tests or stale tests.

## Build and check the still-image NPU path

```sh
cd ~/ai-box-loitering-detect
cmake -S . -B build-board \
  -DAIBOX_WITH_AWNN=ON \
  -DAIBOX_BUILD_TESTS=ON \
  -DAIBOX_AWNN_SDK_ROOT="$HOME/ai-sdk"
cmake --build build-board -j2
ctest --test-dir build-board --output-on-failure
```

Set the runtime library path and run the detector on the SDK's dog image before testing RTSP:

```sh
export LD_LIBRARY_PATH="$HOME/ai-sdk/viplite-tina/lib/aarch64-none-linux-gnu/v2.0${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
./build-board/aibox-image-regression \
  "$HOME/ai-sdk/examples/yolov5/model/v3/yolov5.nb" \
  "$HOME/ai-sdk/examples/yolov5/input_data/dog.jpg" \
  "$HOME/result-aibox.png"
```

This checks the new VIP metadata guard, AWNN execution and CPU YOLO postprocessing. If the VIP output dimensions differ from the demo assumptions, it will stop with an error. Do not continue to the live test until the original vendor demo and this image path both run on the board.

## Configure a loitering zone

The checked-in config deliberately has `"zones": []` and running detection disabled: the application must not guess the monitored area or a walking/running threshold. Edit `config/example.json` on the board and add a zone using normalized image coordinates from 0 to 1. For example, replace the empty array with:

```json
"zones": [{
  "id": "entrance",
  "name": "Entrance",
  "enabled": true,
  "polygon": [[0.20, 0.20], [0.80, 0.20], [0.80, 0.90], [0.20, 0.90]],
  "dwell_seconds": 60
}]
```

These points are only a syntax example; set them to the actual area in the camera frame. The person footpoint is used for zone membership. Configuration is validated at startup, and edits require restarting the application. Fast-motion remains disabled until its thresholds are tuned for this camera.

## Run against the default RTSP camera

The default endpoint is `rtsp://192.168.1.6:554/profile2`; username/password are supplied separately and are not stored in the project. Without an X11/Wayland desktop, the annotated output prefers GStreamer `kmssink` for direct DRM/KMS output, then `fbdevsink`; with a desktop it uses `autovideosink`. Run from SSH. The chosen sink and any output errors appear in the terminal. Press **Ctrl+C** in SSH to stop the process.

```sh
cd ~/ai-box-loitering-detect
export LD_LIBRARY_PATH="$HOME/ai-sdk/viplite-tina/lib/aarch64-none-linux-gnu/v2.0${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export AIBOX_RTSP_USER=admin
read -rsp 'RTSP password: ' AIBOX_RTSP_PASSWORD
printf '\n'
export AIBOX_RTSP_PASSWORD
./build-board/ai-box-core
```

By default the core reads `config/example.json` and writes event rows to `./data/events.sqlite3`; it writes event-start snapshots under `./data/snapshots`. Override with `AIBOX_CONFIG_PATH`, `AIBOX_DATA`, or `AIBOX_EVENT_DB`. Snapshots are queued off the inference path and are limited by the config's `retention_days` and `max_snapshot_bytes`.

To change the RTSP URL while the app is running, open the authenticated dashboard's **Camera / System** page and save the new URL, for example `rtsp://192.168.1.6:554/profile2`. The core validates and persists it, then reconnects. Do not include username/password in the URL. `AIBOX_RTSP_URL` is a startup override; leave it unset when the web-saved URL should be used after restarting the core. For repeated tests, put `AIBOX_RTSP_USER` and `AIBOX_RTSP_PASSWORD` in a private environment file with mode `0600`; do not put credentials in command-line arguments or checked-in files.

If the selected decoder plugin is unavailable, the process reports the missing element. The config accepts `omxh264dec` (the tested hardware path) or `avdec_h264` (requires the libav plugin). If the camera cannot connect or the stream stops, it reports a timeout and retries with backoff. If dynamic libraries fail to load, inspect `ldd build-board/ai-box-core` and add the actual SDK runtime directory to `LD_LIBRARY_PATH`.

## Run the web dashboard

Keep the core running in the first SSH session. In a second session, run FastAPI as the same board user so it can read the core's private token file:

```sh
cd ~/ai-box-loitering-detect
export AIBOX_DATA="$HOME/ai-box-loitering-detect/data"
python3 -m venv .venv
. .venv/bin/activate
pip install -r web/requirements.txt
python tools/setup_admin.py
python -m uvicorn web.app:app --host 0.0.0.0 --port 8080 --workers 1
```

Run the setup command and Uvicorn from the activated virtualenv so both use its Python 3 interpreter. After signing in, open **Camera / System**, edit **RTSP stream URL**, and select **Save and reconnect**. The camera can take several seconds to reconnect; status and live preview update when frames resume.

Open `http://192.168.1.34:8080` and sign in with the administrator password created by `setup_admin.py`. A snapshot appears after a loitering or fast-motion event starts; the default config has no zone and fast-motion disabled, so configure a measured zone before expecting an event image. The board run is still required to verify event creation, snapshot permissions, web retrieval and retention end to end.

For a TLS reverse proxy, set `AIBOX_COOKIE_SECURE=1` on the web process so the session cookie is sent only over HTTPS. The direct board HTTP command above is for a trusted LAN test.

## What this verifies

The user has already confirmed the vendor model, RTSP path and HDMI output on the board. The new build adds tracking, loitering/fast-motion rules, SQLite event persistence, snapshots/retention and the web proxy; these still need target-board verification. The systemd service is not implemented. Do not treat a successful host test as target-board verification; Bullseye uses older GStreamer/OpenCV/SQLite than the development host.

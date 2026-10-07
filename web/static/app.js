const $ = (selector) => document.querySelector(selector);
let refreshTimer;
let language = "vi";
try { language = localStorage.getItem("aibox-language") === "en" ? "en" : "vi"; } catch (_error) { /* use default */ }
let currentPage = "live";
let currentConfig = null;
let latestStatus = null;
let runningDraft = null;
let runningDraftEnabled = null;
let runningDetectDraft = null;
let runningDetectDraftEnabled = null;
let zoneFrameTimer = null;
let zoneFrameRequest = null;
let zoneFrameImage = null;
let zoneFrameStatus = null;
let zoneFrameObjectUrl = null;
let zoneFrameLoading = false;
let zoneDiagnosticNode = null;

const COCO_CLASSES = [
  "person", "bicycle", "car", "motorcycle", "airplane", "bus", "train", "truck", "boat", "traffic light",
  "fire hydrant", "stop sign", "parking meter", "bench", "bird", "cat", "dog", "horse", "sheep", "cow",
  "elephant", "bear", "zebra", "giraffe", "backpack", "umbrella", "handbag", "tie", "suitcase", "frisbee",
  "skis", "snowboard", "sports ball", "kite", "baseball bat", "baseball glove", "skateboard", "surfboard", "tennis racket", "bottle",
  "wine glass", "cup", "fork", "knife", "spoon", "bowl", "banana", "apple", "sandwich", "orange",
  "broccoli", "carrot", "hot dog", "pizza", "donut", "cake", "chair", "couch", "potted plant", "bed",
  "dining table", "toilet", "tv", "laptop", "mouse", "remote", "keyboard", "cell phone", "microwave", "oven",
  "toaster", "sink", "refrigerator", "book", "clock", "vase", "scissors", "teddy bear", "hair drier", "toothbrush",
];

const strings = {
  vi: {
    language: "Ngôn ngữ", login: "Đăng nhập", loginSubtitle: "Quản trị thiết bị AI Box", password: "Mật khẩu quản trị",
    navLabel: "Điều hướng chính", workspace: "KHÔNG GIAN LÀM VIỆC", live: "Trực tiếp", model: "Model & AI",
    zones: "Vùng loitering", behavior: "Hành vi", crowd: "Đám đông", performance: "Tracking / Hiệu năng",
    loiteringEvent: "Lưu trú lâu", fastMotionEvent: "Chuyển động nhanh", runningEvent: "Người chạy", crowdEvent: "Đám đông",
    running: "Chuyển động nhanh", runningDetect: "Phát hiện người chạy", system: "Camera / Hệ thống", events: "Sự kiện",
    liveIntro: "Tình trạng thiết bị và luồng phân tích thời gian thực.",
    modelIntro: "Model đang nạp và ngưỡng suy luận áp dụng cho mỗi frame.",
    systemIntro: "Nguồn video, bộ giải mã và chính sách lưu dữ liệu.",
    zonesIntro: "Định nghĩa vùng quan tâm trực tiếp trên hình camera.",
    behaviorIntro: "Thời gian xác nhận và quy tắc kết thúc episode loitering.",
    crowdIntro: "Cảnh báo mật độ người theo từng zone đã vẽ.",
    runningIntro: "Cấu hình cảnh báo proxy chuyển động nhanh theo track người.",
    runningDetectIntro: "Phát hiện người có tốc độ di chuyển cao bằng track và ngưỡng riêng, độc lập với cảnh báo chuyển động nhanh.",
    performanceIntro: "Ghép nối track, luồng preview và dung lượng lưu trữ.",
    eventsIntro: "Lịch sử cảnh báo và ảnh snapshot được lưu trên thiết bị.",
    device: "Thiết bị", cameraState: "Camera", modelState: "Model runtime", frameRate: "AI FPS",
    latency: "Độ trễ suy luận", frame: "Frame", streamTitle: "Luồng phân tích",
    modelProfile: "Model profile", modelAvailable: "PROFILE ĐANG KHẢ DỤNG",
    modelArchitecture: "Kiến trúc", modelInput: "Kích thước input", modelRuntime: "Runtime",
    modelClassesTitle: "Các lớp cần phát hiện", modelClassesHelp: "Chỉ giữ các lớp cần dùng để giảm số track và cảnh báo nhiễu. Muốn dùng loitering hoặc phát hiện người chạy, hãy giữ lớp person được chọn.", modelClassSearch: "Tìm lớp COCO…", selectAllClasses: "Chọn tất cả", personOnly: "Chỉ người", classesSelected: "lớp đã chọn", requireClass: "Hãy chọn ít nhất một lớp COCO.",
    modelProfileName: "YOLOv5 · A733 NPU", modelClass: "COCO · 80 lớp",
    scoreThreshold: "Ngưỡng confidence", nmsThreshold: "Ngưỡng NMS IoU",
    thresholdHelp: "Confidence thấp tăng số detection và tải xử lý; NMS IoU điều chỉnh mức gộp các hộp trùng.",
    cameraUrl: "Địa chỉ nguồn video", sourceType: "Loại nguồn", rtspSource: "Camera RTSP/RTSPS", videoUrlSource: "Link video HTTP(S)", fileSource: "File video trên Orange Pi", decoder: "Bộ giải mã H.264 cho RTSP", hardwareDecoder: "Phần cứng (omxh264dec)", softwareDecoder: "Phần mềm (avdec_h264)",
    cameraUsername: "Tên đăng nhập nguồn", cameraPassword: "Mật khẩu nguồn", keepPassword: "Xóa mật khẩu đã lưu", passwordConfigured: "Đã lưu mật khẩu; để trống để giữ nguyên.",
    cameraHelp: "Hỗ trợ camera RTSP/RTSPS, link video HTTP(S) và đường dẫn file tuyệt đối như /home/orangepi/video.mp4. Link HTTP(S) cần định dạng mà GStreamer giải mã được. Không nhúng thông tin đăng nhập vào URL.",
    cameraSave: "Lưu cấu hình camera", cameraSaving: "Đang lưu cấu hình camera…", cameraSaved: "Đã lưu. Core đang kết nối lại luồng video.",
    storageSection: "Lưu trữ sự kiện", retentionDays: "Thời hạn lưu sự kiện (ngày)", snapshotQuota: "Dung lượng snapshot tối đa (GB)",
    loiterEntry: "Xác nhận vào vùng (giây)", loiterExit: "Xác nhận rời vùng (giây)", loiterLost: "Grace period khi mất track (giây)", loiterGap: "Khoảng quan sát tối đa để cộng dwell (giây)",
    trackingSection: "Tham số ByteTrack", lowConf: "Confidence thấp", highConf: "Confidence cao", newTrackConf: "Confidence tạo track mới",
    firstIou: "IoU ghép lượt 1", secondIou: "IoU ghép lượt 2", unconfirmedIou: "IoU track chưa xác nhận",
    trackTtl: "Thời gian giữ track (giây)", maxPredict: "Thời gian dự đoán tối đa (giây)", processNoise: "Process noise", measurementNoise: "Measurement noise", minHits: "Số hit xác nhận track",
    previewSection: "Preview web", previewFps: "Tần số preview (FPS)", previewWidth: "Chiều rộng tối đa (px)", previewQuality: "Chất lượng JPEG", previewClients: "Số client tối đa", previewResolution: "Độ phân giải preview",
    storageNote: "Thay đổi preview và retention được áp dụng trực tiếp; giới hạn dung lượng có thể xóa snapshot cũ theo quota.",
    saveAll: "Lưu và áp dụng cấu hình", saving: "Đang áp dụng…", configSaved: "Đã lưu và áp dụng cấu hình.",
    cameraSection: "Nguồn camera", modelNote: "Profile hiện tại được khóa theo model NBG đã kiểm chứng trên A733. Có thể mở rộng danh mục model khi runtime có profile tương thích.",
    inferenceSection: "Tham số suy luận", loiterSection: "Quy tắc loitering", loiterNote: "Các mốc thời gian lọc dao động track và xác nhận trạng thái ở trong hoặc ngoài vùng.",
    crowdSection: "Ngưỡng đám đông", crowdNote: "Đếm track người ổn định có chân nằm trong zone bật. Cảnh báo có ngưỡng bật/tắt riêng và xác nhận theo thời gian.",
    crowdEnable: "Bật crowd detection", crowdTrigger: "Số người để bật cảnh báo", crowdClear: "Số người để xóa cảnh báo", crowdEnterConfirm: "Xác nhận đông người (giây)", crowdExitConfirm: "Xác nhận hết đông (giây)", crowdMaxGap: "Khoảng frame tối đa để cộng xác nhận (giây)", crowdSaved: "Đã lưu cấu hình crowd detection.", crowdThresholdError: "Ngưỡng xóa phải nhỏ hơn ngưỡng bật.", crowdNoZones: "Hãy tạo và bật ít nhất một zone trong trang Vùng loitering.", crowdActive: "CROWD ĐANG BẬT", peopleCount: "người",
    trackingNote: "Tinh chỉnh ghép nối track. Track mất detection được giữ ID tạm theo TTL và vẽ nét đứt; dwell loitering tạm dừng trong lúc không quan sát, không cộng thời gian ước đoán.",
    previewNote: "Khuyến nghị Orange Pi: 1280 px, JPEG 82, tối đa 10 FPS. Nếu mạng yếu, giảm FPS trước; nếu CPU quá tải, giảm chiều rộng.",
    logout: "Đăng xuất", offline: "Mất kết nối", cameraConnected: "Camera đang kết nối",
    cameraDisconnected: "Camera mất kết nối", coreOffline: "Core không phản hồi",
    inferTime: "Thời gian suy luận", storage: "Lưu trữ", unknown: "không rõ", fps: "AI FPS", frame: "Frame",
    message: "Thông báo", eventsRecent: "Sự kiện phân tích gần đây",
    loiterDiagnostics: "Chẩn đoán loitering trực tiếp", enabledZones: "Zone đang bật", personDetections: "Detection người", personTracks: "Track người ổn định",
    diagNoZones: "Chưa có zone bật. Hãy vẽ polygon hợp lệ và bật zone.", diagNoDetections: "Chưa phát hiện người trong frame gần nhất. Kiểm tra khung người ở trang Trực tiếp và ngưỡng confidence.",
    diagNoTracks: "Có detection nhưng chưa có track ổn định. Giữ người rõ trong khung hình và kiểm tra ngưỡng tạo track.",
    diagZoneOutside: "ngoài zone", diagZoneDwell: "đang tính dwell", diagZoneAlarm: "LOITERING ĐANG BẬT", diagZoneLost: "mất quan sát; dwell đang tạm dừng", overlayDelayed: "Overlay AI bị trễ; đã ẩn khung detection",
    started: "Bắt đầu (UTC)", type: "Loại", zone: "Zone", duration: "Thời lượng",
    state: "Trạng thái", image: "Ảnh", active: "đang diễn ra", ended: "đã kết thúc",
    view: "Xem", none: "không có", eventsError: "Không tải được sự kiện",
    cameraUrl: "Địa chỉ nguồn video", cameraHelp: "Ví dụ: rtsp://192.168.1.6:554/profile2, https://host/video.mp4 hoặc /home/orangepi/video.mp4. Không nhúng tài khoản hoặc mật khẩu vào URL.",
    cameraSave: "Lưu và kết nối lại", cameraSaving: "Đang lưu địa chỉ RTSP…",
    cameraSaved: "Đã lưu. Core đang kết nối lại camera.", saveError: "Không lưu được",
    runningNote: "Đây là cảnh báo proxy theo tốc độ track, đơn vị chiều cao hộp người/giây; sự kiện và ngưỡng vẫn độc lập với phát hiện người chạy.",
    runningDetectNote: "Model COCO hiện tại chỉ nhận diện lớp người, không phân biệt dáng đi/chạy. Tính năng này dùng tốc độ track (chiều cao hộp người/giây) làm proxy; hãy bật và tinh chỉnh bằng video tại vị trí lắp camera.",
    runningEnable: "Bật phát hiện chuyển động nhanh", runningDetectEnable: "Bật phát hiện người chạy", startThreshold: "Ngưỡng bắt đầu (body heights/s)",
    endThreshold: "Ngưỡng kết thúc (body heights/s)", window: "Cửa sổ đo tốc độ (giây)",
    minObservations: "Số mẫu tối thiểu", minSpan: "Khoảng mẫu tối thiểu (giây)",
    startConfirm: "Xác nhận bắt đầu (giây)", endConfirm: "Xác nhận kết thúc (giây)",
    lostGrace: "Giữ trạng thái khi mất track (giây)", maxGap: "Khoảng quan sát tối đa (giây)",
    saveApply: "Lưu và áp dụng", endBelowStart: "Ngưỡng kết thúc phải nhỏ hơn ngưỡng bắt đầu để tránh trạng thái bật/tắt liên tục.",
    runningSaved: "Đã lưu. Core đang áp dụng cấu hình mới.", zoneHelp: "Chọn vùng rồi bấm lần lượt các góc trên ảnh để vẽ polygon. Cần ít nhất 3 điểm; người được tính ở trong vùng khi tâm mép dưới hộp người nằm trong polygon. Thời gian lưu trú đặt ngưỡng phát cảnh báo.",
    zoneSelect: "Vùng giám sát", addZone: "Thêm vùng", deleteZone: "Xóa vùng", zoneName: "Tên vùng",
    dwell: "Thời gian phát cảnh báo (giây)", enableZone: "Bật vùng", undo: "Hoàn tác điểm",
    clearPolygon: "Xóa polygon", zoneImageAlt: "Ảnh camera trực tiếp; bấm để vẽ polygon vùng",
    selectZone: "Chọn vùng", enabledSuffix: "đang bật", disabledSuffix: "đang tắt",
    addZoneHelp: "Bấm ít nhất 3 điểm trên ảnh camera để vẽ vùng.", noCameraImage: "Không nhận được ảnh camera. Kiểm tra trang Trực tiếp và kết nối core.",
    loadingFrame: "Đang tải ảnh camera…",
    zonePoints: "điểm. Dùng Hoàn tác điểm để sửa lần bấm cuối.", zoneNeedsSelect: "Hãy thêm hoặc chọn vùng trước.",
    confirmDelete: "Xóa vùng", deletePending: "Thay đổi chỉ có hiệu lực sau khi lưu.",
    invalidZone: "Mỗi vùng cần có tên; vùng đang bật phải có ít nhất 3 điểm và thời gian hợp lệ.",
    zoneSaved: "Đã lưu. Core đang áp dụng cấu hình vùng.", pageConfigError: "Không tải được cấu hình",
    loginFailed: "Đăng nhập thất bại. Kiểm tra mật khẩu hoặc trạng thái dịch vụ.",
    liveAlt: "Hình camera đã gắn kết quả phân tích", zoneTitle: "Vùng loitering",
    runningTitle: "Chuyển động nhanh", runningDetectTitle: "Phát hiện người chạy", systemTitle: "Camera / Hệ thống", eventsTitle: "Sự kiện",
  },
  en: {
    language: "Language", login: "Sign in", loginSubtitle: "Manage your AI Box device", password: "Admin password",
    navLabel: "Main navigation", workspace: "WORKSPACE", live: "Overview", model: "Model & AI",
    zones: "Loitering zones", behavior: "Behavior", crowd: "Crowd", performance: "Tracking / Performance",
    loiteringEvent: "Loitering", fastMotionEvent: "Fast motion", runningEvent: "Running", crowdEvent: "Crowd",
    running: "Fast motion", runningDetect: "Running detection", system: "Camera / System", events: "Events",
    liveIntro: "Device health and real-time analytics stream.",
    modelIntro: "Loaded model profile and inference thresholds applied to each frame.",
    systemIntro: "Video source, decoder and event retention policy.",
    zonesIntro: "Define regions of interest directly on the camera image.",
    behaviorIntro: "Confirmation timing and episode end rules for loitering.",
    crowdIntro: "People-density alerts for each configured zone.",
    runningIntro: "Configure the fast-motion proxy alert for tracked people.",
    runningDetectIntro: "Detect high-speed people using an independent track-speed rule and thresholds.",
    performanceIntro: "Track association, preview streaming and storage capacity.",
    eventsIntro: "Alert history and snapshots stored on this device.",
    device: "Device", cameraState: "Camera", modelState: "Model runtime", frameRate: "AI FPS",
    latency: "Inference latency", frame: "Frame", streamTitle: "Analytics stream",
    modelProfile: "Model profile", modelAvailable: "AVAILABLE PROFILE",
    modelArchitecture: "Architecture", modelInput: "Input size", modelRuntime: "Runtime",
    modelClassesTitle: "Detection classes", modelClassesHelp: "Keep only classes you need to reduce tracks and noisy alerts. Keep person selected to use loitering or running detection.", modelClassSearch: "Search COCO classes…", selectAllClasses: "Select all", personOnly: "Person only", classesSelected: "selected classes", requireClass: "Select at least one COCO class.",
    modelProfileName: "YOLOv5 · A733 NPU", modelClass: "COCO · 80 classes",
    scoreThreshold: "Confidence threshold", nmsThreshold: "NMS IoU threshold",
    thresholdHelp: "Lower confidence yields more detections and higher processing load; NMS IoU controls how overlapping boxes are merged.",
    cameraUrl: "Video source address", sourceType: "Source type", rtspSource: "RTSP/RTSPS camera", videoUrlSource: "HTTP(S) video URL", fileSource: "Video file on Orange Pi", decoder: "RTSP H.264 decoder", hardwareDecoder: "Hardware (omxh264dec)", softwareDecoder: "Software (avdec_h264)",
    cameraUsername: "Source username", cameraPassword: "Source password", keepPassword: "Clear saved password", passwordConfigured: "Password saved; leave blank to keep it.",
    cameraHelp: "Supports RTSP/RTSPS cameras, HTTP(S) video links and absolute local paths such as /home/orangepi/video.mp4. HTTP(S) links must use a format supported by GStreamer. Do not embed credentials in the URL.",
    cameraSave: "Save camera settings", cameraSaving: "Saving camera settings…", cameraSaved: "Saved. Core is reconnecting to the video stream.",
    storageSection: "Event storage", retentionDays: "Event retention (days)", snapshotQuota: "Maximum snapshot storage (GB)",
    loiterEntry: "Zone entry confirmation (seconds)", loiterExit: "Zone exit confirmation (seconds)", loiterLost: "Track loss grace period (seconds)", loiterGap: "Maximum observation gap credited to dwell (seconds)",
    trackingSection: "ByteTrack parameters", lowConf: "Low confidence", highConf: "High confidence", newTrackConf: "New track confidence",
    firstIou: "First-stage match IoU", secondIou: "Second-stage match IoU", unconfirmedIou: "Unconfirmed track IoU",
    trackTtl: "Track lifetime (seconds)", maxPredict: "Maximum prediction time (seconds)", processNoise: "Process noise", measurementNoise: "Measurement noise", minHits: "Hits to confirm track",
    previewSection: "Web preview", previewFps: "Preview rate (FPS)", previewWidth: "Maximum width (px)", previewQuality: "JPEG quality", previewClients: "Maximum clients", previewResolution: "Preview resolution",
    storageNote: "Preview and retention settings apply immediately. Storage quota may remove older snapshots when needed.",
    saveAll: "Save and apply settings", saving: "Applying…", configSaved: "Settings saved and applied.",
    cameraSection: "Camera source", modelNote: "The current profile is locked to the NBG model verified on A733. Additional model profiles can be added when a compatible runtime is available.",
    inferenceSection: "Inference parameters", loiterSection: "Loitering rules", loiterNote: "Timing controls for track jitter filtering and confirming entry or exit from a zone.",
    crowdSection: "Crowd threshold", crowdNote: "Counts stable person tracks whose footpoint lies inside an enabled zone. Trigger and clear thresholds use separate confirmation times.",
    crowdEnable: "Enable crowd detection", crowdTrigger: "People to trigger alert", crowdClear: "People to clear alert", crowdEnterConfirm: "Crowd start confirmation (seconds)", crowdExitConfirm: "Crowd clear confirmation (seconds)", crowdMaxGap: "Maximum frame gap credited to confirmation (seconds)", crowdSaved: "Crowd detection settings saved.", crowdThresholdError: "The clear threshold must be lower than the trigger threshold.", crowdNoZones: "Create and enable at least one zone on the Loitering zones page.", crowdActive: "CROWD ACTIVE", peopleCount: "people",
    trackingNote: "Tune track association. Tracks with missing detections keep a temporary ID through the TTL and appear dashed; loitering dwell pauses while unobserved and does not count estimated time.",
    previewNote: "Orange Pi recommendation: 1280 px, JPEG 82, up to 10 FPS. Reduce FPS first on a weak network; reduce width if the CPU is overloaded.",
    logout: "Sign out", offline: "Offline", cameraConnected: "Camera connected",
    cameraDisconnected: "Camera disconnected", coreOffline: "Core unavailable",
    inferTime: "Inference time", storage: "Storage", unknown: "unknown", fps: "AI FPS", frame: "Frame",
    message: "Message", eventsRecent: "Recent analysis events",
    loiterDiagnostics: "Live loitering diagnostics", enabledZones: "Enabled zones", personDetections: "Person detections", personTracks: "Stable person tracks",
    diagNoZones: "No enabled zone. Draw a valid polygon and enable it.", diagNoDetections: "No person detected in the latest frame. Check the Live page and confidence threshold.",
    diagNoTracks: "Person detections exist but no stable track yet. Keep the person clearly visible and check the new-track threshold.",
    diagZoneOutside: "outside zone", diagZoneDwell: "dwell in progress", diagZoneAlarm: "LOITERING ACTIVE", diagZoneLost: "not observed; dwell paused", overlayDelayed: "AI overlay is stale; detection boxes are hidden",
    started: "Started (UTC)", type: "Type", zone: "Zone", duration: "Duration",
    state: "State", image: "Image", active: "active", ended: "ended",
    view: "View", none: "none", eventsError: "Could not load events",
    cameraUrl: "Video source address", cameraHelp: "Examples: rtsp://192.168.1.6:554/profile2, https://host/video.mp4 or /home/orangepi/video.mp4. Do not embed credentials in the URL.",
    cameraSave: "Save and reconnect", cameraSaving: "Saving RTSP address…",
    cameraSaved: "Saved. Core is reconnecting to the camera.", saveError: "Could not save",
    runningNote: "This is a track-speed proxy in person-box heights per second. Its event and thresholds remain independent from running detection.",
    runningDetectNote: "The current COCO model detects the person class but does not classify gait. This feature uses track speed (person-box heights per second) as a proxy; enable and tune it against footage from the installed camera.",
    runningEnable: "Enable fast-motion detection", runningDetectEnable: "Enable running detection", startThreshold: "Start threshold (body heights/s)",
    endThreshold: "End threshold (body heights/s)", window: "Speed window (seconds)",
    minObservations: "Minimum observations", minSpan: "Minimum sample span (seconds)",
    startConfirm: "Start confirmation (seconds)", endConfirm: "End confirmation (seconds)",
    lostGrace: "Keep state after track loss (seconds)", maxGap: "Maximum observation gap (seconds)",
    saveApply: "Save and apply", endBelowStart: "The end threshold must be lower than the start threshold to prevent rapid state toggling.",
    runningSaved: "Saved. Core is applying the updated settings.", zoneHelp: "Select a zone, then click its corners on the live image to draw the polygon. Use at least 3 points. A person is inside when the bottom-center of their bounding box is inside the polygon. Dwell sets the alarm threshold.",
    zoneSelect: "Zone", addZone: "Add zone", deleteZone: "Delete zone", zoneName: "Zone name",
    dwell: "Alarm dwell threshold (seconds)", enableZone: "Enable zone", undo: "Undo point",
    clearPolygon: "Clear polygon", zoneImageAlt: "Live camera image; click to draw a zone polygon",
    selectZone: "Select zone", enabledSuffix: "enabled", disabledSuffix: "disabled",
    addZoneHelp: "Click at least 3 points on the camera image to draw this zone.", noCameraImage: "Camera image is unavailable. Check the Live page and core connection.",
    loadingFrame: "Loading camera frame…",
    zonePoints: "point(s). Use Undo point to remove the last click.", zoneNeedsSelect: "Add or select a zone first.",
    confirmDelete: "Delete zone", deletePending: "This change takes effect after saving.",
    invalidZone: "Every zone needs a name; enabled zones need at least 3 points and a valid dwell time.",
    zoneSaved: "Saved. Core is applying the zone settings.", pageConfigError: "Could not load configuration",
    loginFailed: "Sign in failed. Check the password or service status.",
    liveAlt: "Live camera view with analysis overlays", zoneTitle: "Loitering zones",
    runningTitle: "Fast motion", runningDetectTitle: "Running detection", systemTitle: "Camera / System", eventsTitle: "Events",
  },
};

function t(key) { return strings[language][key] || key; }

function stopZoneFramePolling() {
  if (zoneFrameTimer) clearInterval(zoneFrameTimer);
  zoneFrameTimer = null;
  if (zoneFrameRequest) zoneFrameRequest.abort();
  zoneFrameRequest = null;
  zoneFrameImage = null;
  zoneFrameStatus = null;
  zoneFrameLoading = false;
  if (zoneFrameObjectUrl) URL.revokeObjectURL(zoneFrameObjectUrl);
  zoneFrameObjectUrl = null;
}

async function refreshZoneFrame() {
  const image = zoneFrameImage;
  if (!image || zoneFrameLoading) return;
  zoneFrameLoading = true;
  const controller = new AbortController();
  zoneFrameRequest = controller;
  let nextUrl = null;
  try {
    const response = await fetch(`/api/frame.jpg?ts=${Date.now()}`, {
      cache: "no-store", signal: controller.signal,
    });
    if (!response.ok) throw Error(`HTTP ${response.status}`);
    nextUrl = URL.createObjectURL(await response.blob());
    if (zoneFrameImage !== image) { URL.revokeObjectURL(nextUrl); return; }
    image.onload = () => {
      if (zoneFrameImage !== image) { URL.revokeObjectURL(nextUrl); return; }
      if (zoneFrameObjectUrl && zoneFrameObjectUrl !== nextUrl)
        URL.revokeObjectURL(zoneFrameObjectUrl);
      zoneFrameObjectUrl = nextUrl;
      image.classList.add("preview-ready");
      image.parentElement.classList.add("has-frame");
      if (zoneFrameStatus && [t("noCameraImage"), t("loadingFrame")].includes(zoneFrameStatus.textContent))
        zoneFrameStatus.textContent = "";
    };
    image.onerror = () => {
      if (nextUrl !== zoneFrameObjectUrl) URL.revokeObjectURL(nextUrl);
    };
    image.src = nextUrl;
  } catch (error) {
    if (error.name !== "AbortError" && zoneFrameImage === image && !image.naturalWidth && zoneFrameStatus)
      zoneFrameStatus.textContent = t("noCameraImage");
  } finally {
    if (zoneFrameRequest === controller) {
      zoneFrameRequest = null;
      zoneFrameLoading = false;
    }
  }
}

function applyStaticLanguage() {
  document.documentElement.lang = language;
  $("#language-label").textContent = t("language");
  $("#language-select").value = language;
  $("#login-title").textContent = t("login");
  $("#login-subtitle").textContent = t("loginSubtitle");
  $("#password").placeholder = t("password");
  $("#login-button").textContent = t("login");
  $("nav").setAttribute("aria-label", t("navLabel"));
  document.querySelectorAll("nav [data-page]").forEach((button) => {
    const label = button.querySelector("span:last-child");
    if (label) label.textContent = t(button.dataset.page);
  });
  document.querySelectorAll("[data-nav-section]").forEach((node) => {
    node.textContent = t(node.dataset.navSection);
  });
  $("#logout").textContent = t("logout");
  if (!latestStatus) $("#state").textContent = t("offline");
  if (latestStatus) renderStatus(latestStatus);
  if (currentPage) {
    const pageTitle = $("#page-title");
    const pageIntro = $("#page-intro");
    if (pageTitle) pageTitle.textContent = t(currentPage);
    if (pageIntro) pageIntro.textContent = t(`${currentPage}Intro`);
  }
}

function setLanguage(value) {
  language = value === "en" ? "en" : "vi";
  try { localStorage.setItem("aibox-language", language); } catch (_error) { /* optional preference */ }
  applyStaticLanguage();
  if (!$("#dashboard").hidden) showPage(currentPage, true);
}

async function api(path, options = {}) {
  const response = await fetch(path, {
    ...options,
    headers: { "Content-Type": "application/json", ...(options.headers || {}) },
  });
  if (!response.ok) {
    const body = await response.text();
    let detail = body || `HTTP ${response.status}`;
    try {
      const parsed = JSON.parse(body);
      detail = parsed.detail || parsed.error || detail;
    } catch (_error) { /* plain text */ }
    throw Error(detail);
  }
  return response.status === 204 ? null : response.json();
}

function renderStatus(value) {
  latestStatus = value;
  const connected = value.camera === "connected";
  const pill = $("#state");
  pill.textContent = connected ? t("cameraConnected") : t("cameraDisconnected");
  pill.className = `state-pill ${connected ? "connected" : "disconnected"}`;
  const details = [
    `${t("fps")}: ${Number(value.ai_fps || 0).toFixed(1)}`,
    `${t("inferTime")}: ${Number(value.inference_ms || 0).toFixed(1)} ms`,
    value.overlay_stale ? t("overlayDelayed") : "",
    `${t("frame")}: ${value.frame_id || 0}`,
    `${t("storage")}: ${value.storage || t("unknown")}`,
    value.message ? `${t("message")}: ${value.message}` : "",
    ...loiterDiagnosticLines(value),
  ].filter(Boolean);
  const statusNode = $("#status");
  if (statusNode) statusNode.textContent = details.join("\n");
  const metricCamera = $("#metric-camera");
  if (metricCamera) metricCamera.textContent = connected ? t("cameraConnected") : t("cameraDisconnected");
  const metricFps = $("#metric-fps");
  if (metricFps) metricFps.textContent = Number(value.ai_fps || 0).toFixed(1);
  const metricLatency = $("#metric-latency");
  if (metricLatency) metricLatency.textContent = `${Number(value.inference_ms || 0).toFixed(1)} ms`;
  const metricFrame = $("#metric-frame");
  if (metricFrame) metricFrame.textContent = value.frame_id || 0;
  renderZoneDiagnostics(value);
}

function loiterDiagnosticLines(value) {
  const detections = Number(value.person_detections || 0);
  const tracks = Number(value.person_tracks || 0);
  const zones = Number(value.enabled_zones || 0);
  const lines = [`${t("enabledZones")}: ${zones} · ${t("personDetections")}: ${detections} · ${t("personTracks")}: ${tracks}`];
  if (value.overlay_stale) lines.push(t("overlayDelayed"));
  else if (!zones) lines.push(t("diagNoZones"));
  else if (!detections) lines.push(t("diagNoDetections"));
  else if (!tracks) lines.push(t("diagNoTracks"));
  if (value.overlay_stale) return lines;
  for (const signal of value.loitering || []) {
    const name = signal.zone_name || signal.zone_id || "Zone";
    const dwell = Number(signal.dwell_seconds || 0).toFixed(1);
    const target = Number(signal.target_seconds || 0).toFixed(1);
    const timer = `${t("diagZoneDwell")}: ${dwell}/${target} s`;
    let phase = t("diagZoneOutside");
    if (!signal.observed) phase = `${t("diagZoneLost")} · ${timer}`;
    else if (signal.active) phase = `${t("diagZoneAlarm")} · ${timer}`;
    else if (signal.inside) phase = timer;
    lines.push(`${name} · #${signal.track_id}: ${phase}`);
  }
  lines.push(...crowdDiagnosticLines(value));
  return lines;
}

function crowdDiagnosticLines(value) {
  return (value.crowd || []).map((signal) => {
    const name = signal.zone_name || signal.zone_id || "Zone";
    const active = signal.active ? ` · ${t("crowdActive")}` : "";
    return `${name} · ${signal.count}/${signal.threshold} ${t("peopleCount")}${active}`;
  });
}

function renderZoneDiagnostics(value) {
  if (!zoneDiagnosticNode || !zoneDiagnosticNode.isConnected) return;
  const lines = loiterDiagnosticLines(value);
  const list = zoneDiagnosticNode.querySelector(".diagnostic-lines");
  list.replaceChildren();
  for (const text of lines) {
    const item = document.createElement("li"); item.textContent = text; list.append(item);
  }
  zoneDiagnosticNode.dataset.alert = (value.loitering || []).some((signal) => signal.active) ||
      (value.crowd || []).some((signal) => signal.active) ? "true" : "false";
}

async function refreshStatus() {
  try {
    renderStatus(await api("/api/status"));
  } catch (error) {
    latestStatus = null;
    $("#state").textContent = t("coreOffline");
    $("#state").className = "state-pill disconnected";
    const statusNode = $("#status");
    if (statusNode) statusNode.textContent = error.message;
    if (zoneDiagnosticNode?.isConnected) {
      const list = zoneDiagnosticNode.querySelector(".diagnostic-lines");
      list.replaceChildren();
      const item = document.createElement("li"); item.textContent = `${t("coreOffline")}: ${error.message}`; list.append(item);
    }
  }
}

function renderEvents(content) {
  const message = document.createElement("p");
  message.textContent = t("eventsRecent");
  content.append(message);
  return api("/api/events?offset=0&limit=100").then((data) => {
    const table = document.createElement("table");
    const head = document.createElement("thead");
    const headerRow = document.createElement("tr");
    for (const label of [t("started"), t("type"), t("zone"), t("duration"), t("state"), t("image")]) {
      const cell = document.createElement("th");
      cell.textContent = label;
      headerRow.append(cell);
    }
    head.append(headerRow);
    table.append(head);
    const body = document.createElement("tbody");
    for (const event of data.items) {
      const row = document.createElement("tr");
      const eventType = event.type === "loitering" ? t("loiteringEvent")
        : event.type === "fast_motion" ? t("fastMotionEvent")
        : event.type === "running" ? t("runningEvent")
        : event.type === "crowd" ? `${t("crowdEvent")} · ${event.crowd_count}/${event.crowd_threshold}` : event.type;
      for (const value of [
        event.started_utc,
        eventType,
        event.zone_id || "—",
        `${Number(event.duration_seconds).toFixed(1)} s`,
        event.active ? t("active") : event.end_reason || t("ended"),
      ]) {
        const cell = document.createElement("td");
        cell.textContent = value;
        row.append(cell);
      }
      const snapshot = document.createElement("td");
      if (event.snapshot_url) {
        const link = document.createElement("a");
        link.href = event.snapshot_url;
        link.target = "_blank";
        link.rel = "noopener";
        link.textContent = t("view");
        snapshot.append(link);
      } else {
        snapshot.textContent = event.snapshot_state || t("none");
      }
      row.append(snapshot);
      body.append(row);
    }
    table.append(body);
    content.append(table);
  }).catch((error) => {
    message.textContent = `${t("eventsError")}: ${error.message}`;
  });
}

function renderCameraSettings(content, config) {
  const form = document.createElement("form");
  const cameraCard = createSettingsCard(t("cameraSection"), t("cameraHelp"));
  const cameraGrid = cameraCard.grid;
  const sourceType = document.createElement("select");
  for (const [value, label] of [["rtsp", t("rtspSource")], ["video_url", t("videoUrlSource")], ["file", t("fileSource")]]) {
    const option = document.createElement("option"); option.value = value; option.textContent = label; sourceType.append(option);
  }
  sourceType.value = config.camera.source_type || "rtsp";
  cameraGrid.append(fieldWrapper(t("sourceType"), sourceType));
  const url = numberOrTextField("url", t("cameraUrl"), config.camera.url, "text", cameraGrid);
  url.autocomplete = "off";
  url.spellcheck = false;
  const username = numberOrTextField("camera-username", t("cameraUsername"), config.camera.username || "", "text", cameraGrid);
  username.autocomplete = "username";
  const password = numberOrTextField("camera-password", t("cameraPassword"), "", "password", cameraGrid);
  password.autocomplete = "new-password";
  password.required = false;
  password.placeholder = config.camera.password_configured ? t("passwordConfigured") : "";
  const clearPassword = document.createElement("input");
  clearPassword.type = "checkbox";
  const clearPasswordRow = fieldWrapper(t("keepPassword"), clearPassword);
  const passwordRow = password.parentElement;
  const decoder = document.createElement("select");
  for (const [value, label] of [["omxh264dec", t("hardwareDecoder")], ["avdec_h264", t("softwareDecoder")]]) {
    const option = document.createElement("option"); option.value = value; option.textContent = label; decoder.append(option);
  }
  decoder.value = config.camera.decoder;
  const decoderRow = fieldWrapper(t("decoder"), decoder);
  cameraGrid.append(passwordRow, clearPasswordRow, decoderRow);
  const updateSourceFields = () => {
    decoderRow.hidden = sourceType.value !== "rtsp";
    username.parentElement.hidden = sourceType.value === "file";
    passwordRow.hidden = sourceType.value === "file";
    clearPasswordRow.hidden = sourceType.value === "file" || !config.camera.password_configured;
    url.placeholder = sourceType.value === "file"
      ? "/home/orangepi/video.mp4"
      : sourceType.value === "video_url" ? "https://host/video.mp4" : "rtsp://192.168.1.6:554/profile2";
  };
  sourceType.addEventListener("change", updateSourceFields);
  clearPassword.addEventListener("change", () => { password.disabled = clearPassword.checked; });
  updateSourceFields();

  const storageCard = createSettingsCard(t("storageSection"), t("storageNote"));
  const retention = numberOrTextField("retention", t("retentionDays"), config.storage.retention_days, "number", storageCard.grid, { min: 1, max: 3650, step: 1 });
  const quotaGb = numberOrTextField("quota", t("snapshotQuota"), (config.storage.max_snapshot_bytes / (1024 ** 3)).toFixed(2), "number", storageCard.grid, { min: 0.001, max: 1024, step: 0.1 });
  const state = makeFormState();
  const save = makeSaveButton(t("saveAll"));
  form.append(cameraCard.card, storageCard.card, save, state);
  form.addEventListener("submit", async (event) => {
    event.preventDefault();
    const candidate = JSON.parse(JSON.stringify(config));
    candidate.camera.source_type = sourceType.value;
    candidate.camera.url = url.value.trim();
    candidate.camera.username = username.value.trim();
    candidate.camera.decoder = decoder.value;
    if (clearPassword.checked) {
      candidate.camera.password = null;
      candidate.camera.password_configured = false;
    } else if (password.value) {
      candidate.camera.password = password.value;
      candidate.camera.password_configured = true;
    } else {
      delete candidate.camera.password;
    }
    candidate.storage.retention_days = Number.parseInt(retention.value, 10);
    candidate.storage.max_snapshot_bytes = Math.round(Number(quotaGb.value) * (1024 ** 3));
    await submitSettings(config, candidate, save, state);
  });
  content.append(form);
}

function fieldWrapper(labelText, input, helpText = "") {
  const wrapper = document.createElement("label");
  wrapper.className = "setting";
  const label = document.createElement("span"); label.textContent = labelText;
  wrapper.append(label, input);
  if (helpText) { const help = document.createElement("small"); help.textContent = helpText; wrapper.append(help); }
  return wrapper;
}

function numberOrTextField(id, label, value, type, grid, bounds = {}, helpText = "") {
  const input = document.createElement("input");
  input.id = id; input.type = type; input.value = value; input.required = true;
  for (const [key, bound] of Object.entries(bounds)) input[key] = bound;
  if (type === "url") { input.autocomplete = "off"; input.spellcheck = false; }
  grid.append(fieldWrapper(label, input, helpText));
  return input;
}

function createSettingsCard(title, description = "") {
  const card = document.createElement("section"); card.className = "settings-card";
  const heading = document.createElement("div"); heading.className = "card-heading";
  const h = document.createElement("h2"); h.textContent = title; heading.append(h);
  if (description) { const p = document.createElement("p"); p.textContent = description; heading.append(p); }
  const grid = document.createElement("div"); grid.className = "settings-grid";
  card.append(heading, grid);
  return { card, grid };
}

function makeFormState() {
  const state = document.createElement("p"); state.className = "form-state"; state.setAttribute("role", "status"); return state;
}

function makeSaveButton(label = t("saveApply")) {
  const button = document.createElement("button"); button.type = "submit"; button.className = "button-primary"; button.textContent = label; return button;
}

async function submitSettings(config, candidate, button, state) {
  button.disabled = true;
  state.textContent = t("saving");
  try {
    const result = await api("/api/config", { method: "PUT", body: JSON.stringify(candidate) });
    candidate.revision = result.revision;
    Object.assign(config, candidate);
    state.textContent = t("configSaved");
    return true;
  } catch (error) {
    state.textContent = `${t("saveError")}: ${error.message}`;
    return false;
  } finally { button.disabled = false; }
}

function renderModelSettings(content, config) {
  const profile = createSettingsCard(t("modelProfile"), t("modelNote"));
  const profileSummary = document.createElement("div"); profileSummary.className = "model-summary";
  profileSummary.innerHTML = `<span class="model-symbol">NPU</span>`;
  const identity = document.createElement("div");
  const name = document.createElement("strong"); name.textContent = t("modelProfileName");
  const id = document.createElement("code"); id.textContent = config.model.profile;
  identity.append(name, id);
  const badge = document.createElement("span"); badge.className = "badge badge-success"; badge.textContent = t("modelAvailable");
  profileSummary.append(identity, badge);
  profile.card.append(profileSummary);
  const facts = document.createElement("div"); facts.className = "model-facts";
  for (const [label, value] of [[t("modelArchitecture"), t("modelClass")], [t("modelInput"), `${config.model.input_width} × ${config.model.input_height}`], [t("modelRuntime"), config.model.runtime]]) {
    const fact = document.createElement("div"); fact.className = "model-fact";
    const caption = document.createElement("small"); caption.textContent = label;
    const contentValue = document.createElement("strong"); contentValue.textContent = value;
    fact.append(caption, contentValue); facts.append(fact);
  }
  profile.card.append(facts);

  const form = document.createElement("form");
  const infer = createSettingsCard(t("inferenceSection"), t("thresholdHelp"));
  const score = numberOrTextField("score-threshold", t("scoreThreshold"), config.model.score_threshold, "number", infer.grid, { min: 0, max: 1, step: 0.01 });
  const nms = numberOrTextField("nms-threshold", t("nmsThreshold"), config.model.nms_iou, "number", infer.grid, { min: 0, max: 1, step: 0.01 });
  const classesCard = createSettingsCard(t("modelClassesTitle"), t("modelClassesHelp"));
  const selectedClasses = new Set(config.model.classes || [0]);
  const classSearch = document.createElement("input");
  classSearch.type = "search";
  classSearch.placeholder = t("modelClassSearch");
  classSearch.className = "coco-class-search";
  const classActions = document.createElement("div");
  classActions.className = "coco-class-actions";
  const selectAll = document.createElement("button");
  selectAll.type = "button";
  selectAll.className = "button-quiet";
  selectAll.textContent = t("selectAllClasses");
  const personOnly = document.createElement("button");
  personOnly.type = "button";
  personOnly.className = "button-quiet";
  personOnly.textContent = t("personOnly");
  const selectedCount = document.createElement("small");
  classActions.append(selectAll, personOnly, selectedCount);
  const classList = document.createElement("div");
  classList.className = "coco-class-list";
  classesCard.grid.style.gridTemplateColumns = "minmax(0, 1fr)";
  const classInputs = [];
  const updateCount = () => { selectedCount.textContent = `${selectedClasses.size} ${t("classesSelected")}`; };
  for (const [id, className] of COCO_CLASSES.entries()) {
    const label = document.createElement("label");
    label.className = "coco-class-option";
    const input = document.createElement("input");
    input.type = "checkbox";
    input.checked = selectedClasses.has(id);
    const text = document.createElement("span");
    text.textContent = `${className} · ${id}`;
    input.addEventListener("change", () => {
      if (input.checked) selectedClasses.add(id);
      else selectedClasses.delete(id);
      updateCount();
    });
    label.append(input, text);
    classList.append(label);
    classInputs.push({ label, input, className, id });
  }
  classSearch.addEventListener("input", () => {
    const query = classSearch.value.trim().toLowerCase();
    for (const item of classInputs)
      item.label.hidden = !`${item.className} ${item.id}`.includes(query);
  });
  selectAll.addEventListener("click", () => {
    for (const item of classInputs) { item.input.checked = true; selectedClasses.add(item.id); }
    updateCount();
  });
  personOnly.addEventListener("click", () => {
    selectedClasses.clear(); selectedClasses.add(0);
    for (const item of classInputs) item.input.checked = item.id === 0;
    updateCount();
  });
  classesCard.grid.append(classSearch, classActions, classList);
  updateCount();
  const state = makeFormState(); const save = makeSaveButton();
  form.append(infer.card, classesCard.card, save, state);
  form.addEventListener("submit", async (event) => {
    event.preventDefault();
    if (!selectedClasses.size) { state.textContent = t("requireClass"); return; }
    const candidate = JSON.parse(JSON.stringify(config));
    candidate.model.score_threshold = Number(score.value);
    candidate.model.nms_iou = Number(nms.value);
    candidate.model.classes = [...selectedClasses].sort((a, b) => a - b);
    await submitSettings(config, candidate, save, state);
  });
  content.append(profile.card, form);
}

function renderLoiterSettings(content, config) {
  const form = document.createElement("form");
  const card = createSettingsCard(t("loiterSection"), t("loiterNote"));
  const fields = {};
  for (const [key, label, value, bounds] of [
    ["enter_confirm_seconds", t("loiterEntry"), config.loitering.enter_confirm_seconds, { min: 0, step: 0.1 }],
    ["exit_confirm_seconds", t("loiterExit"), config.loitering.exit_confirm_seconds, { min: 0, step: 0.1 }],
    ["lost_grace_seconds", t("loiterLost"), config.loitering.lost_grace_seconds, { min: 0, step: 0.1 }],
    ["max_observation_gap_seconds", t("loiterGap"), config.loitering.max_observation_gap_seconds, { min: 0.1, step: 0.1 }],
  ]) fields[key] = numberOrTextField(key, label, value, "number", card.grid, bounds);
  const state = makeFormState(); const save = makeSaveButton();
  form.append(card.card, save, state);
  form.addEventListener("submit", async (event) => {
    event.preventDefault();
    const candidate = JSON.parse(JSON.stringify(config));
    for (const [key, input] of Object.entries(fields)) candidate.loitering[key] = Number(input.value);
    await submitSettings(config, candidate, save, state);
  });
  content.append(form);
}

function renderPerformanceSettings(content, config) {
  const form = document.createElement("form");
  const tracking = createSettingsCard(t("trackingSection"), t("trackingNote"));
  const trackingFields = {};
  const trackingLabels = {
    low_confidence: t("lowConf"), high_confidence: t("highConf"),
    new_track_confidence: t("newTrackConf"), first_match_iou: t("firstIou"),
    second_match_iou: t("secondIou"), unconfirmed_match_iou: t("unconfirmedIou"),
    track_ttl_seconds: t("trackTtl"), max_predict_seconds: t("maxPredict"),
    process_noise: t("processNoise"), measurement_noise: t("measurementNoise"),
    min_hits: t("minHits"),
  };
  for (const [key, label] of Object.entries(trackingLabels)) {
    const isThreshold = key.includes("confidence") || key.includes("iou");
    trackingFields[key] = numberOrTextField(key, label, config.tracking[key], "number", tracking.grid,
      isThreshold ? { min: 0, max: 1, step: 0.01 } : { min: key === "min_hits" ? 1 : 0.01, step: key === "min_hits" ? 1 : 0.1 });
  }
  const preview = createSettingsCard(t("previewSection"), t("previewNote"));
  const previewFields = {};
  const previewLabels = { fps: t("previewFps"), max_width: t("previewWidth"), quality: t("previewQuality"), max_clients: t("previewClients") };
  for (const [key, label] of Object.entries(previewLabels)) {
    const bounds = key === "fps" ? { min: 1, max: 10, step: 1 }
      : key === "max_width" ? { min: 160, max: 1920, step: 1 }
      : key === "quality" ? { min: 20, max: 95, step: 1 } : { min: 1, max: 8, step: 1 };
    previewFields[key] = numberOrTextField(key, label, config.preview[key], "number", preview.grid, bounds);
  }
  const state = makeFormState(); const save = makeSaveButton();
  form.append(tracking.card, preview.card, save, state);
  form.addEventListener("submit", async (event) => {
    event.preventDefault();
    const candidate = JSON.parse(JSON.stringify(config));
    for (const [key, input] of Object.entries(trackingFields))
      candidate.tracking[key] = key === "min_hits" ? Number.parseInt(input.value, 10) : Number(input.value);
    for (const [key, input] of Object.entries(previewFields))
      candidate.preview[key] = Number.parseInt(input.value, 10);
    await submitSettings(config, candidate, save, state);
  });
  content.append(form);
}

function renderBehaviorEditor(content, config, page) {
  if (page === "model") { renderModelSettings(content, config); return; }
  if (page === "zones") {
    renderZoneEditor(content, config);
    return;
  }
  if (page === "behavior") { renderLoiterSettings(content, config); return; }
  if (page === "crowd") { renderCrowdSettings(content, config); return; }
  if (page === "performance") { renderPerformanceSettings(content, config); return; }
  if (page === "runningDetect") { renderSpeedRuleSettings(content, config, "running_detection", true); return; }
  renderRunningSettings(content, config);
}

function renderCrowdSettings(content, config) {
  const form = document.createElement("form");
  const card = createSettingsCard(t("crowdSection"), t("crowdNote"));
  if (!(config.zones || []).some((zone) => zone.enabled)) {
    const warning = document.createElement("p");
    warning.className = "notice";
    warning.textContent = t("crowdNoZones");
    content.append(warning);
  }
  const enabledWrap = document.createElement("label");
  enabledWrap.className = "setting";
  enabledWrap.append(t("crowdEnable"));
  const enabled = document.createElement("input");
  enabled.type = "checkbox";
  enabled.checked = config.crowd.enabled;
  enabledWrap.append(enabled);
  card.grid.append(enabledWrap);
  const trigger = numberOrTextField("crowd-trigger", t("crowdTrigger"), config.crowd.trigger_people, "number", card.grid, { min: 1, step: 1 });
  const clear = numberOrTextField("crowd-clear", t("crowdClear"), config.crowd.clear_people, "number", card.grid, { min: 0, step: 1 });
  const enter = numberOrTextField("crowd-enter", t("crowdEnterConfirm"), config.crowd.enter_confirm_seconds, "number", card.grid, { min: 0, step: 0.1 });
  const exit = numberOrTextField("crowd-exit", t("crowdExitConfirm"), config.crowd.exit_confirm_seconds, "number", card.grid, { min: 0, step: 0.1 });
  const gap = numberOrTextField("crowd-gap", t("crowdMaxGap"), config.crowd.max_observation_gap_seconds, "number", card.grid, { min: 0.1, step: 0.1 });
  const state = makeFormState();
  const save = makeSaveButton();
  form.append(card.card, save, state);
  form.addEventListener("submit", async (event) => {
    event.preventDefault();
    if (Number(clear.value) >= Number(trigger.value)) {
      state.textContent = t("crowdThresholdError");
      clear.focus();
      return;
    }
    const candidate = JSON.parse(JSON.stringify(config));
    candidate.crowd = {
      enabled: enabled.checked,
      trigger_people: Number.parseInt(trigger.value, 10),
      clear_people: Number.parseInt(clear.value, 10),
      enter_confirm_seconds: Number(enter.value),
      exit_confirm_seconds: Number(exit.value),
      max_observation_gap_seconds: Number(gap.value),
    };
    if (await submitSettings(config, candidate, save, state)) state.textContent = t("crowdSaved");
  });
  content.append(form);
}

function renderRunningSettings(content, config) {
  renderSpeedRuleSettings(content, config, "running", false);
}

function renderSpeedRuleSettings(content, config, configKey, isRunningDetect) {
  const running = config[configKey];
  const note = document.createElement("p");
  note.className = "notice";
  note.textContent = t(isRunningDetect ? "runningDetectNote" : "runningNote");
  const form = document.createElement("form");
  const grid = document.createElement("div");
  grid.className = "settings-grid";
  const fields = {};
  function numberField(key, label, step = "0.1", min = "0") {
    const wrapper = document.createElement("label");
    wrapper.className = "setting";
    wrapper.textContent = label;
    const input = document.createElement("input");
    input.type = "number";
    input.min = min;
    input.step = step;
    input.required = true;
    const draft = isRunningDetect ? runningDetectDraft : runningDraft;
    input.value = draft && draft[key] !== undefined ? draft[key] : running[key];
    wrapper.append(input);
    grid.append(wrapper);
    fields[key] = input;
  }
  const enableWrap = document.createElement("label");
  enableWrap.className = "setting";
  enableWrap.append(t(isRunningDetect ? "runningDetectEnable" : "runningEnable"));
  const enable = document.createElement("input");
  enable.type = "checkbox";
  const enabledDraft = isRunningDetect ? runningDetectDraftEnabled : runningDraftEnabled;
  enable.checked = enabledDraft === null ? running.enabled : enabledDraft;
  enableWrap.append(enable);
  grid.append(enableWrap);
  numberField("start_threshold", t("startThreshold"));
  numberField("end_threshold", t("endThreshold"));
  numberField("window_seconds", t("window"), "0.1", "0.1");
  numberField("min_observations", t("minObservations"), "1", "2");
  numberField("min_span_seconds", t("minSpan"), "0.1", "0.1");
  numberField("start_confirm_seconds", t("startConfirm"));
  numberField("end_confirm_seconds", t("endConfirm"));
  numberField("lost_grace_seconds", t("lostGrace"));
  numberField("max_gap_seconds", t("maxGap"), "0.1", "0.1");
  const state = document.createElement("p");
  state.className = "form-state";
  state.setAttribute("role", "status");
  const save = document.createElement("button");
  save.type = "submit";
  save.textContent = t("saveApply");
  form.append(grid, save, state);
  form.addEventListener("submit", async (event) => {
    event.preventDefault();
    const start = Number(fields.start_threshold.value);
    const end = Number(fields.end_threshold.value);
    if (!(end < start)) {
      state.textContent = t("endBelowStart");
      fields.end_threshold.focus();
      return;
    }
    save.disabled = true;
    try {
      const candidate = JSON.parse(JSON.stringify(config));
      candidate[configKey].enabled = enable.checked;
      for (const [key, input] of Object.entries(fields))
        candidate[configKey][key] = key === "min_observations" ? Number.parseInt(input.value, 10) : Number(input.value);
      const result = await api("/api/config", { method: "PUT", body: JSON.stringify(candidate) });
      config.revision = result.revision;
      config[configKey] = candidate[configKey];
      if (isRunningDetect) { runningDetectDraft = null; runningDetectDraftEnabled = null; }
      else { runningDraft = null; runningDraftEnabled = null; }
      state.textContent = t("runningSaved");
    } catch (error) {
      state.textContent = `${t("saveError")}: ${error.message}`;
    } finally {
      save.disabled = false;
    }
  });
  for (const [key, input] of Object.entries(fields))
    input.addEventListener("input", () => {
      const draft = isRunningDetect ? (runningDetectDraft ||= {}) : (runningDraft ||= {});
      draft[key] = input.value;
    });
  enable.addEventListener("change", () => {
    if (isRunningDetect) runningDetectDraftEnabled = enable.checked;
    else runningDraftEnabled = enable.checked;
  });
  content.append(note, form);
}

function renderZoneEditor(content, config) {
  const diagnostics = document.createElement("section");
  diagnostics.className = "loiter-diagnostics";
  const diagnosticsTitle = document.createElement("h2");
  diagnosticsTitle.textContent = t("loiterDiagnostics");
  const diagnosticLines = document.createElement("ul");
  diagnosticLines.className = "diagnostic-lines";
  diagnostics.append(diagnosticsTitle, diagnosticLines);
  zoneDiagnosticNode = diagnostics;
  if (latestStatus) renderZoneDiagnostics(latestStatus);
  const help = document.createElement("p");
  help.textContent = t("zoneHelp");
  const layout = document.createElement("div");
  layout.className = "zone-layout";
  const tools = document.createElement("section");
  tools.className = "zone-tools";
  const selectLabel = document.createElement("label");
  selectLabel.textContent = t("zoneSelect");
  const select = document.createElement("select");
  select.setAttribute("aria-label", t("selectZone"));
  const add = document.createElement("button");
  add.type = "button";
  add.textContent = t("addZone");
  const remove = document.createElement("button");
  remove.type = "button";
  remove.textContent = t("deleteZone");
  const nameLabel = document.createElement("label");
  nameLabel.textContent = t("zoneName");
  const name = document.createElement("input");
  name.maxLength = 80;
  const dwellLabel = document.createElement("label");
  dwellLabel.textContent = t("dwell");
  const dwell = document.createElement("input");
  dwell.type = "number";
  dwell.min = "0";
  dwell.step = "1";
  dwell.required = true;
  const enabledLabel = document.createElement("label");
  const enabled = document.createElement("input");
  enabled.type = "checkbox";
  enabledLabel.append(enabled, ` ${t("enableZone")}`);
  const undo = document.createElement("button");
  undo.type = "button";
  undo.textContent = t("undo");
  const clear = document.createElement("button");
  clear.type = "button";
  clear.textContent = t("clearPolygon");
  const save = document.createElement("button");
  save.type = "button";
  save.textContent = t("saveApply");
  const state = document.createElement("p");
  state.setAttribute("role", "status");
  tools.append(selectLabel, select, add, remove, nameLabel, name,
               dwellLabel, dwell, enabledLabel, undo, clear, save, state);

  const stage = document.createElement("div");
  stage.className = "zone-stage";
  const image = document.createElement("img");
  image.alt = t("zoneImageAlt");
  image.onerror = () => {
    if (!image.naturalWidth) state.textContent = t("noCameraImage");
  };
  const overlay = document.createElementNS("http://www.w3.org/2000/svg", "svg");
  overlay.setAttribute("viewBox", "0 0 1000 1000");
  overlay.setAttribute("preserveAspectRatio", "none");
  overlay.setAttribute("role", "application");
  overlay.setAttribute("aria-label", t("zoneImageAlt"));
  stage.append(image, overlay);
  layout.append(tools, stage);
  content.append(diagnostics, help, layout);
  zoneFrameImage = image;
  zoneFrameStatus = state;
  state.textContent = t("loadingFrame");
  refreshZoneFrame();
  zoneFrameTimer = setInterval(refreshZoneFrame, 1000);

  let selectedId = "";
  const selected = () => config.zones.find((zone) => zone.id === selectedId);
  function refreshSelect() {
    select.replaceChildren();
    for (const zone of config.zones) {
      const option = document.createElement("option");
      option.value = zone.id;
      option.textContent = `${zone.name || zone.id} (${zone.enabled ? t("enabledSuffix") : t("disabledSuffix")})`;
      select.append(option);
    }
    if (selectedId && config.zones.some((zone) => zone.id === selectedId))
      select.value = selectedId;
    else selectedId = config.zones[0]?.id || "";
    if (selectedId) select.value = selectedId;
    refreshFields();
  }
  function refreshFields() {
    const zone = selected();
    const hasZone = Boolean(zone);
    select.disabled = config.zones.length === 0;
    remove.disabled = !hasZone;
    name.disabled = dwell.disabled = enabled.disabled = !hasZone;
    undo.disabled = clear.disabled = !hasZone;
    name.value = zone?.name || "";
    dwell.value = zone ? zone.dwell_seconds : "";
    enabled.checked = Boolean(zone?.enabled);
    enabled.disabled = !zone || (zone.polygon || []).length < 3;
    drawPolygon();
  }
  function drawPolygon() {
    overlay.replaceChildren();
    const zone = selected();
    if (!zone) return;
    const points = (zone.polygon || []).map(([x, y]) => `${x * 1000},${y * 1000}`).join(" ");
    if (points) {
      const shape = document.createElementNS(overlay.namespaceURI,
        zone.polygon.length >= 3 ? "polygon" : "polyline");
      shape.setAttribute("points", points);
      shape.setAttribute("class", "zone-draft");
      overlay.append(shape);
    }
    for (const [x, y] of zone.polygon || []) {
      const point = document.createElementNS(overlay.namespaceURI, "circle");
      point.setAttribute("cx", x * 1000);
      point.setAttribute("cy", y * 1000);
      point.setAttribute("r", "8");
      point.setAttribute("class", "zone-point");
      overlay.append(point);
    }
  }
  select.addEventListener("change", () => { selectedId = select.value; refreshFields(); });
  add.addEventListener("click", () => {
    let suffix = config.zones.length + 1;
    while (config.zones.some((zone) => zone.id === `zone-${suffix}`)) suffix += 1;
    const zone = { id: `zone-${suffix}`, name: `Zone ${suffix}`, enabled: false,
                   polygon: [], dwell_seconds: 60 };
    config.zones.push(zone);
    selectedId = zone.id;
    state.textContent = t("addZoneHelp");
    refreshSelect();
  });
  remove.addEventListener("click", () => {
    if (!window.confirm(`${t("confirmDelete")} “${selected()?.name || selectedId}”? ${t("deletePending")}`)) return;
    config.zones = config.zones.filter((zone) => zone.id !== selectedId);
    selectedId = config.zones[0]?.id || "";
    refreshSelect();
  });
  name.addEventListener("input", () => {
    const zone = selected();
    if (zone) {
      zone.name = name.value;
      const option = Array.from(select.options).find((item) => item.value === zone.id);
      if (option) option.textContent = `${zone.name || zone.id} (${zone.enabled ? t("enabledSuffix") : t("disabledSuffix")})`;
    }
  });
  dwell.addEventListener("input", () => {
    const zone = selected();
    if (zone) zone.dwell_seconds = dwell.value === "" ? Number.NaN : Number(dwell.value);
  });
  enabled.addEventListener("change", () => {
    const zone = selected();
    if (zone) { zone.enabled = enabled.checked; refreshSelect(); select.value = zone.id; }
  });
  undo.addEventListener("click", () => {
    const zone = selected();
    if (zone?.polygon.length) zone.polygon.pop();
    refreshFields();
  });
  clear.addEventListener("click", () => {
    const zone = selected();
    if (zone) { zone.polygon = []; zone.enabled = false; }
    refreshFields();
  });
  overlay.addEventListener("click", (event) => {
    const zone = selected();
    if (!zone) { state.textContent = t("zoneNeedsSelect"); return; }
    const rect = overlay.getBoundingClientRect();
    if (!rect.width || !rect.height) return;
    const x = Math.max(0, Math.min(1, (event.clientX - rect.left) / rect.width));
    const y = Math.max(0, Math.min(1, (event.clientY - rect.top) / rect.height));
    zone.polygon.push([Number(x.toFixed(5)), Number(y.toFixed(5))]);
    if (zone.polygon.length >= 3) zone.enabled = true;
    state.textContent = `${zone.polygon.length} ${t("zonePoints")}`;
    refreshFields();
  });
  save.addEventListener("click", async () => {
    if (config.zones.some((zone) => !zone.id || !zone.name.trim() ||
        !Number.isFinite(Number(zone.dwell_seconds)) || Number(zone.dwell_seconds) < 0 ||
        (zone.enabled && zone.polygon.length < 3))) {
      state.textContent = t("invalidZone");
      return;
    }
    save.disabled = true;
    try {
      const candidate = JSON.parse(JSON.stringify(config));
      const result = await api("/api/config", { method: "PUT", body: JSON.stringify(candidate) });
      config.revision = result.revision;
      state.textContent = t("zoneSaved");
    } catch (error) {
      state.textContent = `${t("saveError")}: ${error.message}`;
    } finally {
      save.disabled = false;
    }
  });
  refreshSelect();
}

async function showPage(page, reuseConfig = false) {
  stopZoneFramePolling();
  zoneDiagnosticNode = null;
  currentPage = page;
  const content = $("#content");
  content.replaceChildren();
  $("#page-title").textContent = t(page);
  $("#page-intro").textContent = t(`${page}Intro`);
  $("#page-eyebrow").textContent = `AI BOX  /  ${page.toUpperCase()}`;
  document.querySelectorAll("nav [data-page]").forEach((button) => {
    if (button.dataset.page === page) button.setAttribute("aria-current", "page");
    else button.removeAttribute("aria-current");
  });

  if (page === "live") {
    const metrics = document.createElement("div");
    metrics.className = "metric-grid";
    for (const [id, label, value] of [
      ["metric-camera", t("cameraState"), t("offline")],
      ["metric-fps", t("frameRate"), "—"],
      ["metric-latency", t("latency"), "—"],
      ["metric-resolution", t("previewResolution"), "—"],
      ["metric-frame", t("frame"), "—"],
    ]) {
      const card = document.createElement("section"); card.className = "metric-card";
      const caption = document.createElement("small"); caption.textContent = label;
      const number = document.createElement("strong"); number.id = id; number.textContent = value;
      card.append(caption, number); metrics.append(card);
    }
    content.append(metrics);
    const panel = document.createElement("section"); panel.className = "surface live-panel";
    const heading = document.createElement("div"); heading.className = "card-heading";
    const streamTitle = document.createElement("h2"); streamTitle.textContent = t("streamTitle"); heading.append(streamTitle);
    const image = document.createElement("img");
    image.id = "live";
    image.alt = t("liveAlt");
    image.src = "/api/stream.mjpg";
    image.className = "live-image";
    image.addEventListener("load", () => {
      const metric = $("#metric-resolution");
      if (metric && image.naturalWidth && image.naturalHeight)
        metric.textContent = `${image.naturalWidth} × ${image.naturalHeight}`;
    });
    const status = document.createElement("pre");
    status.id = "status";
    status.className = "status-details";
    panel.append(heading, image, status);
    content.append(panel);
    refreshStatus();
    return;
  }

  if (page === "events") {
    await renderEvents(content);
    return;
  }

  try {
    let config = currentConfig;
    if (!reuseConfig || !config) {
      config = await api("/api/config");
      currentConfig = config;
      runningDraft = null;
      runningDraftEnabled = null;
      runningDetectDraft = null;
      runningDetectDraftEnabled = null;
    }
    if (page === "system") {
      renderCameraSettings(content, config);
    } else {
      renderBehaviorEditor(content, config, page);
    }
  } catch (error) {
    const message = document.createElement("p");
    message.textContent = `${t("pageConfigError")}: ${error.message}`;
    content.append(message);
  }
}

async function show() {
  try {
    await api("/api/status");
    $("#login").hidden = true;
    $("#dashboard").hidden = false;
    await showPage("live");
    clearInterval(refreshTimer);
    refreshTimer = setInterval(refreshStatus, 1000);
  } catch (_error) {
    // Keep the login page visible until authentication and core status succeed.
  }
}

$("#login form").addEventListener("submit", async (event) => {
  event.preventDefault();
  try {
    await api("/api/login", {
      method: "POST",
      body: JSON.stringify({ password: $("#password").value }),
    });
    await show();
  } catch (_error) {
    $("#message").textContent = t("loginFailed");
  }
});

$("#logout").onclick = async () => {
  await api("/api/logout", { method: "POST" });
  location.reload();
};
document.querySelectorAll("[data-page]").forEach((button) => {
  button.onclick = () => showPage(button.dataset.page);
});
$("#language-select").addEventListener("change", (event) => setLanguage(event.target.value));
applyStaticLanguage();
show();

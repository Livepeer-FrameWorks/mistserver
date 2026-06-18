/**
 * @file ndi.cpp
 * @brief Implementation of NDI device discovery and I/O operations
 */

#include "device_ndi.h"

#include "defines.h"
#include "timing.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <set>

// Helper function to convert NDI FourCC codes to string representations
const char *NDIlib_FourCC_type_string(NDIlib_FourCC_video_type_e fourcc) {
  switch (fourcc) {
    case NDIlib_FourCC_video_type_UYVY: return "UYVY";
    case NDIlib_FourCC_video_type_UYVA: return "UYVA";
    case NDIlib_FourCC_video_type_P216: return "P216";
    case NDIlib_FourCC_video_type_PA16: return "PA16";
    case NDIlib_FourCC_video_type_YV12: return "YV12";
    case NDIlib_FourCC_video_type_I420: return "I420";
    case NDIlib_FourCC_video_type_NV12: return "NV12";
    case NDIlib_FourCC_video_type_BGRA: return "BGRA";
    case NDIlib_FourCC_video_type_BGRX: return "BGRX";
    case NDIlib_FourCC_video_type_RGBA: return "RGBA";
    case NDIlib_FourCC_video_type_RGBX: return "RGBX";
    default: return "Unknown";
  }
}

namespace NDI {

  static std::atomic<int> ndiRefCount{0};

  bool initialize() {
    if (ndiRefCount.fetch_add(1) == 0) {
      if (!NDIlib_initialize()) {
        ndiRefCount.fetch_sub(1);
        ERROR_MSG("NDI: Failed to initialize NDI library");
        return false;
      }
    }
    return true;
  }

  void deinitialize() {
    int prev;
    do {
      prev = ndiRefCount.load();
      if (prev <= 0) return;
    } while (!ndiRefCount.compare_exchange_weak(prev, prev - 1));
    if (prev == 1) { NDIlib_destroy(); }
  }

  const NDIlib_source_t *Discovery::getSources(uint32_t & count) {
    if (!finder) {
      ERROR_MSG("NDI: Finder not initialized");
      count = 0;
      return nullptr;
    }

    // Enumerate: wait up to 5 seconds, but break early if no new sources for 2 seconds
    const uint64_t maxWaitMs = 5000;
    const uint64_t idleMs = 2000;
    uint64_t tStart = Util::bootMS();
    uint64_t tLastChange = tStart;

    // Track seen sources by name|url to detect additions
    std::set<std::string> seen;

    // Seed seen set once at the beginning
    uint32_t seedCount = 0;
    const NDIlib_source_t *seed = NDIlib_find_get_current_sources(finder, &seedCount);
    if (seedCount == 0) {
      INFO_MSG("NDI: No immediate sources, waiting for changes...");
    } else {
      for (uint32_t i = 0; i < seedCount; ++i) {
        const char *nme = seed[i].p_ndi_name ? seed[i].p_ndi_name : "";
        const char *url = seed[i].p_url_address ? seed[i].p_url_address : "";
        seen.insert(std::string(nme) + "|" + std::string(url));
      }
      INFO_MSG("NDI: Found %u sources immediately", seedCount);
      tLastChange = Util::bootMS();
    }

    while (Util::bootMS() - tStart < maxWaitMs) {
      // Wait for up to 250ms for any change notification
      bool changed = NDIlib_find_wait_for_sources(finder, 250);
      if (changed) {
        // Fetch current list once on change and log any new sources
        uint32_t curCount = 0;
        const NDIlib_source_t *cur = NDIlib_find_get_current_sources(finder, &curCount);
        for (uint32_t i = 0; i < curCount; ++i) {
          const char *nme = cur[i].p_ndi_name ? cur[i].p_ndi_name : "";
          const char *url = cur[i].p_url_address ? cur[i].p_url_address : "";
          std::string key = std::string(nme) + "|" + std::string(url);
          if (!seen.count(key)) {
            INFO_MSG("NDI: New source: %s (%s)", nme, url);
            seen.insert(key);
          }
        }
        tLastChange = Util::bootMS();
      }
      // If no changes for idleMs, assume stabilized
      if (Util::bootMS() - tLastChange >= idleMs) { break; }
    }

    // Final snapshot right before returning; return the pointer from this call
    const NDIlib_source_t *finalList = NDIlib_find_get_current_sources(finder, &count);
    if (!finalList) {
      INFO_MSG("NDI: No sources found after timeout");
      return nullptr;
    }
    INFO_MSG("NDI: Returning %u sources after wait", count);
    return finalList;
  }

  const NDIlib_source_t *Discovery::getCurrentSourcesNoWait(uint32_t & count) {
    if (!finder) {
      ERROR_MSG("NDI: Finder not initialized");
      count = 0;
      return nullptr;
    }
    // Non-blocking fetch of current sources
    return NDIlib_find_get_current_sources(finder, &count);
  }

  //
  // Discovery Implementation (Used by Controller)
  //

  Discovery::Discovery() {
    finder = NDIlib_find_create_v2(nullptr);
  }

  Discovery::~Discovery() {
    cleanupTallyReceivers();
    if (finder) {
      NDIlib_find_destroy(finder);
      finder = nullptr;
    }
  }

  //
  // Device Implementation
  //

  Device::Device() : receiver(nullptr), sender(nullptr), connected(false), ownedSource(), hasSource(false) {
    receiverConfig = NDIlib_recv_create_v3_t{};
  }

  Device::~Device() {
    disconnect();
  }

  void Device::disconnect() {
    if (receiver) {
      NDIlib_recv_destroy(receiver);
      receiver = nullptr;
    }
    if (sender) {
      NDIlib_send_destroy(sender);
      sender = nullptr;
    }
    connected = false;
    INFO_MSG("NDI: Disconnected");
  }

  bool Device::isConnected() const {
    return connected;
  }

  //
  // Core Methods (Used by all contexts)
  //

  bool Device::setSource(const NDIlib_source_t *source) {
    if (!source) {
      WARN_MSG("NDI: Invalid source provided");
      return false;
    }

    name = source->p_ndi_name ? source->p_ndi_name : "";
    address = source->p_url_address ? source->p_url_address : "";
    ownedSource.p_ndi_name = name.c_str();
    ownedSource.p_url_address = address.c_str();
    hasSource = true;

    INFO_MSG("NDI: Source set to %s (%s)", name.c_str(), address.c_str());
    return true;
  }

  ::Device::DeviceInfo Device::queryCapabilities() const {
    ::Device::DeviceInfo info;

    // Get device and source capabilities
    auto deviceCaps = getDeviceCaps();
    auto sourceCaps = getSourceCaps();

    // Basic device info
    info.id = name;
    info.host = address;
    info.name = name;
    info.manufacturer = "NewTek";
    info.model = "NDI Device";
    info.firmwareVersion = NDIlib_version();
    info.serialNumber = "";
    info.status = isConnected() ? "connected" : "disconnected";

    // Core capabilities
    info.hasPTZ = deviceCaps.hasPTZ;
    info.hasAudio = !sourceCaps.audioFormats.empty();
    info.hasMetadata = true; // NDI always has metadata

    // PTZ features
    if (deviceCaps.hasPTZ) { info.ptzFeatures = {"pan_tilt", "zoom", "focus", "preset", "home", "stop"}; }

    // Protocol config
    ::Device::ProtocolConfig ndiConfig;
    ndiConfig.type = "ndi";
    ndiConfig.address = address;
    ndiConfig.port = 0;

    // Protocol capabilities
    ndiConfig.capabilities.hasPTZ = deviceCaps.hasPTZ;
    ndiConfig.capabilities.hasAudio = !sourceCaps.audioFormats.empty();
    ndiConfig.capabilities.hasMetadata = true;
    ndiConfig.capabilities.hasVideo = !sourceCaps.videoFormats.empty();
    ndiConfig.capabilities.hasRecording = deviceCaps.hasRecording;
    ndiConfig.capabilities.hasWebControl = deviceCaps.hasWebControl;
    ndiConfig.capabilities.hasTally = deviceCaps.hasTally;

    // Add supported formats
    for (const auto & format : sourceCaps.videoFormats) {
      ndiConfig.capabilities.supportedFormats.push_back(format.colorFormat);
      ndiConfig.capabilities.supportedResolutions.push_back(std::to_string(format.width) + "x" + std::to_string(format.height));
      ndiConfig.capabilities.supportedFramerates.push_back(std::to_string(format.frameRate));
    }

    for (const auto & format : sourceCaps.audioFormats) {
      ndiConfig.capabilities.supportedAudioFormats.push_back(format.format);
    }

    // Add supported commands
    if (deviceCaps.hasPTZ) {
      ndiConfig.capabilities.supportedCommands = {"pan_tilt", "zoom", "focus", "preset", "home", "stop"};
    }

    // Store protocol config
    info.protocols["ndi"] = ndiConfig;

    // Add stream endpoints
    // Main NDI stream
    ::Device::StreamEndpoint ndiStream;
    ndiStream.name = name + " NDI";
    ndiStream.protocol = "ndi";
    ndiStream.transport = "tcp";
    ndiStream.format = sourceCaps.videoFormats.empty() ? "unknown" : sourceCaps.videoFormats[0].colorFormat;
    if (!sourceCaps.videoFormats.empty()) {
      ndiStream.width = sourceCaps.videoFormats[0].width;
      ndiStream.height = sourceCaps.videoFormats[0].height;
      ndiStream.fps = sourceCaps.videoFormats[0].frameRate;
      ndiStream.resolution = std::to_string(ndiStream.width) + "x" + std::to_string(ndiStream.height);
      ndiStream.framerate = std::to_string(ndiStream.fps);
    }
    ndiStream.uri = "ndi:" + name;
    info.streams.push_back(ndiStream);

    // Add web control URL if available
    if (deviceCaps.hasWebControl) { info.webControlUrl = getWebControlUrl(); }

    return info;
  }

  Device::DeviceCapabilities Device::getDeviceCaps() const {
    DeviceCapabilities caps;

    if (!connected || !hasSource) { return caps; }

    // Check PTZ support
    caps.hasPTZ = NDIlib_recv_ptz_is_supported(receiver);

    // Check recording support
    caps.hasRecording = NDIlib_recv_recording_is_supported(receiver);

    // Check tally support
    NDIlib_tally_t tally;
    caps.hasTally = sender && (NDIlib_send_get_tally(sender, &tally, 0) == true);

    // Check web control
    std::string webUrl = getWebControlUrl();
    caps.hasWebControl = !webUrl.empty();

    // Add supported commands
    if (caps.hasPTZ) { caps.supportedCommands = {"pan_tilt", "zoom", "stop"}; }

    return caps;
  }

  SourceCapabilities Device::getSourceCaps() const {
    SourceCapabilities caps;

    if (!connected || !receiver || !hasSource) { return caps; }

    // Initialize frame structures to zero
    NDIlib_video_frame_v2_t videoFrame = {};
    NDIlib_audio_frame_v3_t audioFrame = {};
    NDIlib_metadata_frame_t metadataFrame = {};

    // Try multiple times to get both video and audio format info.
    // HX2/HX3 sources may need several seconds for the decoder to produce a usable frame.
    bool gotVideo = false, gotAudio = false;
    int attempts = 0;
    const int maxAttempts = 30;

    while ((!gotVideo || !gotAudio) && attempts < maxAttempts) {
      NDIlib_frame_type_e frameType = NDIlib_recv_capture_v3(receiver, &videoFrame, &audioFrame, &metadataFrame, 200);

      switch (frameType) {
        case NDIlib_frame_type_video:
          if (!gotVideo && videoFrame.xres > 0 && videoFrame.yres > 0) {
            if (videoFrame.xres > 16384 || videoFrame.yres < 2 || videoFrame.yres > 16384) {
              WARN_MSG(
                "NDI: Ignoring bogus video frame %dx%d @ %d/%d fps (FourCC=0x%08X) — likely compressed passthrough",
                videoFrame.xres, videoFrame.yres, videoFrame.frame_rate_N, videoFrame.frame_rate_D, (uint32_t)videoFrame.FourCC);
            } else {
              VideoFormat format;
              format.width = videoFrame.xres;
              format.height = videoFrame.yres;
              format.fps_n = videoFrame.frame_rate_N;
              format.fps_d = videoFrame.frame_rate_D;
              format.frameRate = (format.fps_d > 0) ? (float)format.fps_n / (float)format.fps_d : 0.0f;
              const char *fourcc = NDIlib_FourCC_type_string(videoFrame.FourCC);
              format.colorFormat = fourcc ? fourcc : "unknown";
              caps.videoFormats.push_back(format);
              caps.hasVideo = true;
              gotVideo = true;
            }
          }
          NDIlib_recv_free_video_v2(receiver, &videoFrame);
          break;

        case NDIlib_frame_type_audio:
          if (!gotAudio && audioFrame.no_channels > 0) {
            AudioFormat format;
            format.sampleRate = audioFrame.sample_rate;
            format.channels = audioFrame.no_channels;
            format.bitDepth = 32; // NDI always uses 32-bit float
            format.format = "PCM"; // NDI audio is always PCM float
            caps.audioFormats.push_back(format);
            caps.hasAudio = true;
            gotAudio = true;
          }
          NDIlib_recv_free_audio_v3(receiver, &audioFrame);
          break;

        case NDIlib_frame_type_metadata: NDIlib_recv_free_metadata(receiver, &metadataFrame); break;

        default:
          // No frame received, continue
          break;
      }
      attempts++;
    }

    // Add supported color formats
    caps.supportedColorFormats = {"UYVY", "UYVA", "P216", "PA16", "YV12", "I420",
                                  "NV12", "BGRA", "BGRX", "RGBA", "RGBX"};

    // Add supported bandwidths
    caps.supportedBandwidths = {"audio-only", "lowest", "highest"};

    return caps;
  }

  //
  // Connection Management
  //

  bool Device::setConfig(const InputConfig & config) {
    // Store configuration for later use in connect()
    if (!config.colorFormat.empty()) {
      if (config.colorFormat == "UYVY_RGBA") {
        receiverConfig.color_format = NDIlib_recv_color_format_e_UYVY_RGBA;
      } else if (config.colorFormat == "BGRX_BGRA") {
        receiverConfig.color_format = NDIlib_recv_color_format_e_BGRX_BGRA;
      } else {
        receiverConfig.color_format = NDIlib_recv_color_format_e_UYVY_BGRA;
      }
    }

    if (!config.bandwidth.empty()) {
      if (config.bandwidth == "lowest") {
        receiverConfig.bandwidth = NDIlib_recv_bandwidth_lowest;
      } else if (config.bandwidth == "audio-only") {
        receiverConfig.bandwidth = NDIlib_recv_bandwidth_audio_only;
      } else {
        receiverConfig.bandwidth = NDIlib_recv_bandwidth_highest;
      }
    }

    receiverConfig.allow_video_fields = config.allowVideoFields;
    receiverConfig.p_ndi_recv_name = "MistServer";

    INFO_MSG("NDI: Input configuration updated");
    return true;
  }

  bool Discovery::connectByName(Device & node, const std::string & target) {
    // Find the NDI source using discovery
    uint32_t numSources = 0;
    const NDIlib_source_t *sources = getSources(numSources);

    if (!sources || !numSources) {
      WARN_MSG("NDI: No sources found when trying to connect to %s", target.c_str());
      return false;
    }

    // Find matching source by name/address
    const NDIlib_source_t *targetSource = nullptr;
    for (uint32_t i = 0; i < numSources; i++) {
      const NDIlib_source_t & source = sources[i];
      if (!source.p_ndi_name || !source.p_url_address) { continue; }

      INFO_MSG("NDI: Checking source %s (%s) against target %s", source.p_ndi_name, source.p_url_address, target.c_str());

      // Try to match by either name or address
      if ((source.p_ndi_name && target == source.p_ndi_name) || (source.p_url_address && target == source.p_url_address)) {
        targetSource = &source;
        INFO_MSG("NDI: Found matching source: %s (%s)", source.p_ndi_name, source.p_url_address);
        break;
      }
    }

    if (!targetSource) {
      WARN_MSG("NDI: Source not found: %s", target.c_str());
      return false;
    }

    // Set the source and connect
    if (!node.setSource(targetSource)) {
      WARN_MSG("Failed to set NDI source: %s", target.c_str());
      return false;
    }
    return node.connect();
  }

  bool Device::connect() {
    if (isConnected()) {
      WARN_MSG("NDI: Already connected");
      return true;
    }

    if (!hasSource) {
      WARN_MSG("NDI: No source set");
      return false;
    }

    // Clean up existing receiver if any
    if (receiver) {
      NDIlib_recv_destroy(receiver);
      receiver = nullptr;
    }

    // Create receiver with stored config or defaults
    bool hasCustomConfig =
      receiverConfig.p_ndi_recv_name != nullptr || receiverConfig.color_format != 0 || receiverConfig.bandwidth != 0;

    receiver = NDIlib_recv_create_v3(hasCustomConfig ? &receiverConfig : nullptr);
    if (!receiver) {
      ERROR_MSG("NDI: Failed to create receiver");
      return false;
    }

    // Connect to the source
    NDIlib_recv_connect(receiver, &ownedSource);

    // Wait a bit for connection to establish
    Util::sleep(100);

    // Verify connection by trying to get connection count
    if (NDIlib_recv_get_no_connections(receiver) < 0) {
      ERROR_MSG("NDI: Failed to establish connection to source");
      NDIlib_recv_destroy(receiver);
      receiver = nullptr;
      return false;
    }

    connected = true;
    INFO_MSG("NDI: Connected to %s", name.c_str());
    return true;
  }

  //
  // Input Operations
  //

  NDIlib_frame_type_e Device::receiveAny(NDIlib_video_frame_v2_t & video, NDIlib_audio_frame_v3_t & audio,
                                         NDIlib_metadata_frame_t & metadata) {
    if (!receiver || !connected) return NDIlib_frame_type_none;
    return NDIlib_recv_capture_v3(receiver, &video, &audio, &metadata, 1000);
  }

  void Device::freeVideo(NDIlib_video_frame_v2_t & frame) {
    if (receiver && frame.p_data) { NDIlib_recv_free_video_v2(receiver, &frame); }
  }

  bool Device::receiveAudio(NDIlib_audio_frame_v2_t & frame) {
    if (!receiver || !connected) return false;
    return NDIlib_recv_capture_v2(receiver, nullptr, &frame, nullptr, 1000) == NDIlib_frame_type_audio;
  }

  void Device::freeAudio(NDIlib_audio_frame_v3_t & frame) {
    if (receiver && frame.p_data) { NDIlib_recv_free_audio_v3(receiver, &frame); }
  }

  void Device::freeAudio(NDIlib_audio_frame_v2_t & frame) {
    if (receiver && frame.p_data) { NDIlib_recv_free_audio_v2(receiver, &frame); }
  }

  void Device::freeMetadata(NDIlib_metadata_frame_t & frame) {
    if (receiver) NDIlib_recv_free_metadata(receiver, &frame);
  }

  bool Device::addConnectionMetadata(const NDIlib_metadata_frame_t & metadata) {
    if (!receiver) return false;
    NDIlib_recv_add_connection_metadata(receiver, &metadata);
    return true;
  }

  bool Device::clearMetadata() {
    if (!receiver) return false;
    NDIlib_recv_clear_connection_metadata(receiver);
    return true;
  }

  bool Device::sendReceiverMetadata(const NDIlib_metadata_frame_t & metadata) {
    if (!receiver) return false;
    NDIlib_recv_send_metadata(receiver, &metadata);
    return true;
  }

  bool Device::setTally(const NDIlib_tally_t & tally) {
    if (!receiver) return false;
    NDIlib_recv_set_tally(receiver, &tally);
    return true;
  }

  //
  // Output Operations
  //

  bool Device::openOutput() {
    if (sender) {
      WARN_MSG("NDI: Output already initialized");
      return true;
    }

    // Create sender with clocking disabled
    // See header file for detailed explanation of NDI clocking behavior
    NDIlib_send_create_t config = {0}; // Zero initialize all fields
    config.p_ndi_name = name.c_str();
    config.p_groups = nullptr;
    config.clock_video = false; // Disable video clocking
    config.clock_audio = false; // Disable audio clocking

    sender = NDIlib_send_create(&config);
    if (!sender) {
      ERROR_MSG("NDI: Failed to create sender");
      return false;
    }

    connected = true;
    INFO_MSG("NDI: Opened output with name '%s'", name.c_str());
    return true;
  }

  bool Device::sendVideoAsync(const NDIlib_video_frame_v2_t *frame) {
    if (!sender) return false;
    if (frame) {
      // Convert v2 frame to v1 frame for async sending
      NDIlib_video_frame_t v1_frame = {0};
      v1_frame.xres = frame->xres;
      v1_frame.yres = frame->yres;
      v1_frame.FourCC = frame->FourCC;
      v1_frame.frame_rate_N = frame->frame_rate_N;
      v1_frame.frame_rate_D = frame->frame_rate_D;
      v1_frame.picture_aspect_ratio = frame->picture_aspect_ratio;
      v1_frame.frame_format_type = frame->frame_format_type;
      v1_frame.timecode = frame->timecode;
      v1_frame.p_data = frame->p_data;
      v1_frame.line_stride_in_bytes = frame->line_stride_in_bytes;
      NDIlib_send_send_video_async(sender, &v1_frame);
    } else {
      // Synchronize by sending nullptr
      NDIlib_send_send_video_async(sender, nullptr);
    }
    return true;
  }

  bool Device::sendVideo(const NDIlib_video_frame_v2_t & frame) {
    if (!sender) return false;
    NDIlib_send_send_video_v2(sender, &frame);
    return true;
  }

  bool Device::sendAudio(const NDIlib_audio_frame_v3_t & frame) {
    if (!sender) return false;
    NDIlib_send_send_audio_v3(sender, &frame);
    return true;
  }

  bool Device::sendMetadata(const NDIlib_metadata_frame_t & metadata) {
    if (!sender) return false;
    NDIlib_send_send_metadata(sender, &metadata);
    return true;
  }

  bool Device::getPerformanceInfo(float & video_fps, float & dropped_fps, float & audio_fps) const {
    if (!receiver) return false;

    NDIlib_recv_performance_t total;
    NDIlib_recv_performance_t dropped;
    NDIlib_recv_get_performance(receiver, &total, &dropped);

    uint64_t now = time(nullptr);
    if (perfLastCheckTime == 0) {
      perfLastCheckTime = now;
      perfLastTotal = total;
      perfLastDropped = dropped;
      video_fps = 0;
      dropped_fps = 0;
      audio_fps = 0;
      return true;
    }

    float timeDelta = (float)(now - perfLastCheckTime);
    if (timeDelta <= 0) timeDelta = 1.0f;

    video_fps = (total.video_frames - perfLastTotal.video_frames) / timeDelta;
    dropped_fps = (dropped.video_frames - perfLastDropped.video_frames) / timeDelta;
    audio_fps = (total.audio_frames - perfLastTotal.audio_frames) / timeDelta;

    perfLastCheckTime = now;
    perfLastTotal = total;
    perfLastDropped = dropped;

    return true;
  }

  bool Device::getQueueInfo(int & video_frames, int & audio_frames, int & metadata_frames) const {
    if (!receiver) return false;

    NDIlib_recv_queue_t queue;
    NDIlib_recv_get_queue(receiver, &queue);

    // Get current frame counts in queues
    video_frames = queue.video_frames;
    audio_frames = queue.audio_frames;
    metadata_frames = queue.metadata_frames;

    return true;
  }

  int Device::getConnectionCount() const {
    if (!connected || !sender) { return 0; }
    return NDIlib_send_get_no_connections(sender, 0);
  }

  std::string Device::getWebControlUrl() const {
    if (!connected || !receiver) { return ""; }
    const char *url = NDIlib_recv_get_web_control(receiver);
    std::string webUrl = url ? url : "";
    if (url) { NDIlib_recv_free_string(receiver, url); }
    return webUrl;
  }

  bool Device::hasPTZControl() const {
    if (!connected || !receiver) return false;
    return NDIlib_recv_ptz_is_supported(receiver);
  }

  bool Device::ptzZoom(float zoom) {
    if (!connected || !receiver) return false;
    return NDIlib_recv_ptz_zoom_speed(receiver, zoom);
  }

  bool Device::ptzPanTilt(float pan, float tilt) {
    if (!connected || !receiver) return false;
    return NDIlib_recv_ptz_pan_tilt_speed(receiver, pan, tilt);
  }

  bool Device::ptzStop() {
    if (!connected || !receiver) return false;
    // Stop all movement
    NDIlib_recv_ptz_pan_tilt_speed(receiver, 0.0f, 0.0f);
    NDIlib_recv_ptz_zoom_speed(receiver, 0.0f);
    return true;
  }

  bool Device::sendPTZ(const ::Device::PTZCommand & cmd) {
    if (!hasPTZControl()) {
      ERROR_MSG("NDI: Device does not support PTZ control");
      return false;
    }

    try {
      switch (cmd.action) {
        case ::Device::PTZAction::PanTilt:
          if (cmd.args.find("pan") != cmd.args.end() && cmd.args.find("tilt") != cmd.args.end()) {
            return ptzPanTilt(cmd.args.at("pan"), cmd.args.at("tilt"));
          }
          break;
        case ::Device::PTZAction::Zoom:
          if (cmd.args.find("zoom") != cmd.args.end()) { return ptzZoom(cmd.args.at("zoom")); }
          break;
        case ::Device::PTZAction::Stop: return ptzStop();
        case ::Device::PTZAction::Home: return ptzPanTilt(0.0f, 0.0f) && ptzZoom(0.0f);
        default: ERROR_MSG("NDI: Unsupported PTZ command: %d", static_cast<int>(cmd.action)); return false;
      }
      ERROR_MSG("NDI: Missing required PTZ arguments");
      return false;
    } catch (const std::exception & e) {
      ERROR_MSG("NDI: PTZ command failed: %s", e.what());
      return false;
    }
  }

  bool Discovery::sendCommand(const std::string & deviceId, const ::Device::PTZCommand & cmd) {
    uint32_t count = 0;
    const NDIlib_source_t *sources = getCurrentSourcesNoWait(count);
    if (!sources || count == 0) {
      WARN_MSG("NDI PTZ: No sources available (count=%u)", count);
      return false;
    }

    HIGH_MSG("NDI PTZ: Searching %u sources for device '%s'", count, deviceId.c_str());

    const NDIlib_source_t *match = nullptr;
    for (uint32_t i = 0; i < count; ++i) {
      if (sources[i].p_url_address && deviceId == sources[i].p_url_address) {
        match = &sources[i];
        break;
      }
    }
    if (!match) {
      for (uint32_t i = 0; i < count; ++i) {
        if (sources[i].p_url_address && std::string(sources[i].p_url_address).find(deviceId) != std::string::npos) {
          match = &sources[i];
          break;
        }
      }
    }
    if (!match) {
      WARN_MSG("NDI PTZ: No source matched device '%s'", deviceId.c_str());
      return false;
    }

    HIGH_MSG("NDI PTZ: Matched source '%s' at '%s'", match->p_ndi_name ? match->p_ndi_name : "?",
             match->p_url_address ? match->p_url_address : "?");

    Device device;
    if (!device.setSource(match) || !device.connect()) {
      WARN_MSG("NDI PTZ: Failed to connect to matched source");
      return false;
    }
    bool result = device.sendPTZ(cmd);
    HIGH_MSG("NDI PTZ: sendPTZ returned %s", result ? "true" : "false");
    return result;
  }

  std::unique_ptr<::Device::Base> Discovery::createDevice(const ::Device::DeviceInfo & info) const {
    uint32_t count = 0;
    const NDIlib_source_t *sources = const_cast<Discovery *>(this)->getCurrentSourcesNoWait(count);
    if (!sources || count == 0) return nullptr;

    std::string targetName = info.name;
    std::string targetAddr;
    auto protoIt = info.protocols.find("ndi");
    if (protoIt != info.protocols.end()) targetAddr = protoIt->second.address;

    const NDIlib_source_t *match = nullptr;
    for (uint32_t i = 0; i < count; ++i) {
      const auto & s = sources[i];
      if (!s.p_ndi_name || !s.p_url_address) continue;
      if ((!targetName.empty() && targetName == s.p_ndi_name) || (!targetAddr.empty() && targetAddr == s.p_url_address)) {
        match = &s;
        break;
      }
    }
    if (!match) return nullptr;

    auto dev = std::unique_ptr<Device>(new Device());
    if (!dev->setSource(match)) return nullptr;
    return dev;
  }

  bool Discovery::setTally(const std::string & deviceId, bool program, bool preview) {
    if (!program && !preview) {
      // Turn off: destroy the persistent receiver so the sender no longer sees our tally
      auto it = tallyReceivers_.find(deviceId);
      if (it != tallyReceivers_.end()) {
        INFO_MSG("NDI: Tally OFF for %s", deviceId.c_str());
        tallyReceivers_.erase(it);
      }
      return true;
    }

    auto it = tallyReceivers_.find(deviceId);
    if (it == tallyReceivers_.end()) {
      // Need to create a persistent receiver for this source
      uint32_t count = 0;
      const NDIlib_source_t *sources = getCurrentSourcesNoWait(count);
      if (!sources || count == 0) return false;

      const NDIlib_source_t *match = nullptr;
      for (uint32_t i = 0; i < count; ++i) {
        if (sources[i].p_url_address && std::string(sources[i].p_url_address).find(deviceId) != std::string::npos) {
          match = &sources[i];
          break;
        }
        if (sources[i].p_ndi_name && std::string(sources[i].p_ndi_name).find(deviceId) != std::string::npos) {
          match = &sources[i];
          break;
        }
      }
      if (!match) return false;

      auto dev = std::unique_ptr<Device>(new Device());
      if (!dev->setSource(match) || !dev->connect()) return false;
      INFO_MSG("NDI: Tally ON for %s", deviceId.c_str());
      it = tallyReceivers_.emplace(deviceId, std::move(dev)).first;
    }

    NDIlib_tally_t tally;
    tally.on_program = program;
    tally.on_preview = preview;
    return it->second->setTally(tally);
  }

  void Discovery::cleanupTallyReceivers() {
    tallyReceivers_.clear();
  }

  // Async Discovery Implementation
  bool Discovery::startAsyncDiscovery(::Device::DiscoveryCallback callback, uint32_t timeoutMs) {
    if (asyncRunning) {
      HIGH_MSG("NDI: Async discovery already running");
      return false;
    }

    if (!finder) {
      ERROR_MSG("NDI: Finder not initialized");
      return false;
    }

    INFO_MSG("NDI: Starting async discovery (timeout: %u ms)", timeoutMs);

    asyncCallback = callback;
    asyncTimeoutMs = timeoutMs;
    asyncStartTime =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    asyncRunning = true;
    asyncDevices.clear();
    lastSourceCount = 0;

    return true;
  }

  void Discovery::stopAsyncDiscovery() {
    if (!asyncRunning) return;

    INFO_MSG("NDI: Stopping async discovery");
    asyncRunning = false;
    asyncCallback = nullptr;
  }

  bool Discovery::checkForNewSources() {
    if (!asyncRunning || !finder) return false;

    try {
      // Non-blocking poll of current sources (do not wait inside event loop)
      uint32_t currentCount = 0;
      const NDIlib_source_t *sources = NDIlib_find_get_current_sources(finder, &currentCount);

      if (currentCount != lastSourceCount) {
        INFO_MSG("NDI: Source count changed from %u to %u", lastSourceCount, currentCount);
        lastSourceCount = currentCount;

        // Process all current sources
        if (sources && currentCount > 0) {
          std::vector<::Device::DeviceInfo> newDevices;

          for (uint32_t i = 0; i < currentCount; i++) {
            const NDIlib_source_t & source = sources[i];
            if (!source.p_ndi_name || !source.p_url_address) continue;

            ::Device::DeviceInfo info;
            info.id = std::string(source.p_ndi_name);
            info.name = std::string(source.p_ndi_name);
            info.host = std::string(source.p_url_address);
            info.manufacturer = "NewTek";
            info.model = "NDI Source";

            // Extract IP from NDI address format (typically "name (ip:port)")
            std::string address = source.p_url_address;
            size_t start = address.find('(');
            size_t end = address.find(')', start);
            if (start != std::string::npos && end != std::string::npos) {
              std::string ipPort = address.substr(start + 1, end - start - 1);
              size_t colonPos = ipPort.find(':');
              if (colonPos != std::string::npos) {
                info.host = ipPort.substr(0, colonPos);
                try {
                  uint16_t port = std::stoi(ipPort.substr(colonPos + 1));
                  info.protocols["ndi"].port = port;
                } catch (...) {
                  info.protocols["ndi"].port = 5353; // Default NDI port
                }
              }
            }

            // Set NDI protocol config
            info.protocols["ndi"].type = "ndi";
            info.protocols["ndi"].address = info.host;
            info.hasAudio = true;
            info.hasMetadata = true;

            // Check if this is a new device
            bool isNew = true;
            for (const auto & existing : asyncDevices) {
              if (existing.id == info.id) {
                isNew = false;
                break;
              }
            }

            if (isNew) {
              asyncDevices.push_back(info);
              newDevices.push_back(info);
              HIGH_MSG("NDI: Async discovered new device: %s", info.name.c_str());
            }
          }

          // Call callback with new devices if any found
          if (!newDevices.empty() && asyncCallback) { asyncCallback(newDevices); }
        }
      }

      // Check for timeout
      if (asyncTimeoutMs > 0) {
        uint64_t currentTime =
          std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();

        if (currentTime - asyncStartTime >= asyncTimeoutMs) {
          INFO_MSG("NDI: Async discovery timeout reached, found %zu devices", asyncDevices.size());
          stopAsyncDiscovery();
          return true; // Signal timeout
        }
      }

      return false; // Continue running
    } catch (const std::exception & e) {
      WARN_MSG("NDI: Error checking for new sources: %s", e.what());
      return false;
    }
  }

} // namespace NDI

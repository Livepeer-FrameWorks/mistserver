/**
 * @file input_ndi.cpp
 * @brief NDI input implementation
 *
 * TODO: Consider implementing NDIlib_framesync_instance_t for better frame synchronization
 * The NDI SDK provides a frame synchronizer object that can:
 * - Maintain A/V sync
 * - Handle different frame rates
 * - Provide frame rate conversion
 * - Buffer frames to reduce jitter
 * - Automatically handle timestamp management
 * See NDIlib_framesync_create() in the NDI SDK documentation
 */

#include "input_ndi.h"

#include <mist/defines.h>
#include <mist/shared_memory.h>
#include <mist/stream.h>

#include <cstdlib>
#include <cstring>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace Mist {
  inNDI::inNDI(Util::Config *cfg) : Input(cfg) {
    capa["name"] = "NDI";
    capa["desc"] = "NDI input using NDI SDK";
    capa["source_match"] = "ndi:*";
    capa["always_match"] = capa["source_match"];
    capa["priority"] = 11;

    // Initialize NDI library before creating any NDI objects
    if (!NDI::initialize()) {
      Util::logExitReason(ER_READ_START_FAILURE, "Failed to initialize NDI library");
      exit(1);
    }
    dev.reset(new NDI::Device());
    sourceFinder.reset(new NDI::Discovery());

    audioSampleRate = 0;
    audioSampleDepth = 0;
    audioChannels = 0;
    videoTrackIdx = INVALID_TRACK_ID;
    audioTrackIdx = INVALID_TRACK_ID;
    metadataTrackIdx = INVALID_TRACK_ID;
    force8Bit = true;
    hasFrames = true;
    lastWidth = 0;
    lastHeight = 0;
    lastFpks = 0;

    // Default NDI settings
    ndiColorFormat = "UYVY_BGRA";
    ndiBandwidth = "highest";

    // Initialize metrics
    lastMetricsUpdate = 0;
    lastVideoFps = 0;
    lastDroppedFps = 0;
    lastAudioFps = 0;
    lastVideoQueue = 0;
    lastAudioQueue = 0;
    lastMetadataQueue = 0;

    // Add color format option
    JSON::Value option;
    option["arg"] = "string";
    option["long"] = "color-format";
    option["short"] = "C";
    option["help"] = "NDI color format (UYVY_BGRA, UYVY_RGBA, or BGRX_BGRA)";
    option["value"].append("UYVY_BGRA");
    config->addOption("color-format", option);
    capa["optional"]["color-format"]["name"] = "NDI color format";
    capa["optional"]["color-format"]["help"] = "NDI color format (UYVY_BGRA, UYVY_RGBA, or BGRX_BGRA)";
    capa["optional"]["color-format"]["option"] = "--color-format";
    capa["optional"]["color-format"]["short"] = "C";
    capa["optional"]["color-format"]["default"] = "UYVY_BGRA";
    capa["optional"]["color-format"]["type"] = "select";
    capa["optional"]["color-format"]["select"][0u][0u] = "UYVY_BGRA";
    capa["optional"]["color-format"]["select"][0u][1u] = "UYVY to BGRA conversion";
    capa["optional"]["color-format"]["select"][1u][0u] = "UYVY_RGBA";
    capa["optional"]["color-format"]["select"][1u][1u] = "UYVY to RGBA conversion";
    capa["optional"]["color-format"]["select"][2u][0u] = "BGRX_BGRA";
    capa["optional"]["color-format"]["select"][2u][1u] = "BGRX to BGRA conversion";

    // Add bandwidth option
    option.null();
    option["arg"] = "string";
    option["long"] = "bandwidth";
    option["short"] = "B";
    option["help"] = "NDI bandwidth mode (audio-only, lowest, or highest)";
    option["value"].append("highest");
    config->addOption("bandwidth", option);
    capa["optional"]["bandwidth"]["name"] = "NDI bandwidth mode";
    capa["optional"]["bandwidth"]["help"] = "NDI bandwidth mode (audio-only, lowest, or highest)";
    capa["optional"]["bandwidth"]["option"] = "--bandwidth";
    capa["optional"]["bandwidth"]["short"] = "B";
    capa["optional"]["bandwidth"]["default"] = "highest";
    capa["optional"]["bandwidth"]["type"] = "select";
    capa["optional"]["bandwidth"]["select"][0u][0u] = "audio-only";
    capa["optional"]["bandwidth"]["select"][0u][1u] = "Metadata and audio only";
    capa["optional"]["bandwidth"]["select"][1u][0u] = "lowest";
    capa["optional"]["bandwidth"]["select"][1u][1u] = "Audio and low-resolution video";
    capa["optional"]["bandwidth"]["select"][2u][0u] = "highest";
    capa["optional"]["bandwidth"]["select"][2u][1u] = "Full resolution audio and video";

    capa["enum_static_prefix"] = "ndi:";
    option.null();
    option["long"] = "enumerate";
    option["short"] = "e";
    option["help"] = "Output MistIn supported devices in JSON format, then exit";
    option["value"].append("");
    config->addOption("enumerate", option);

    capa["dynamic_capa"] = true;
    option.null();
    option["long"] = "getcapa";
    option["arg"] = "string";
    option["short"] = "q";
    option["help"] = "(string) Output device capabilities for given device in JSON format, then exit";
    option["value"].append("");
    config->addOption("getcapa", option);

    option.null();
    option["arg"] = "integer";
    option["long"] = "channels";
    option["short"] = "c";
    option["help"] = "Audio channels";
    option["value"].append(2);
    config->addOption("channels", option);
    capa["optional"]["channels"]["name"] = "Audio channels";
    capa["optional"]["channels"]["help"] = "Audio channel count";
    capa["optional"]["channels"]["option"] = "--channels";
    capa["optional"]["channels"]["short"] = "c";
    capa["optional"]["channels"]["default"] = 2;
    capa["optional"]["channels"]["type"] = "select";
    capa["optional"]["channels"]["select"][0u][0u] = "2";
    capa["optional"]["channels"]["select"][0u][1u] = "2 channels";
    capa["optional"]["channels"]["select"][1u][0u] = "8";
    capa["optional"]["channels"]["select"][1u][1u] = "8 channels";
    capa["optional"]["channels"]["select"][2u][0u] = "16";
    capa["optional"]["channels"]["select"][2u][1u] = "16 channels";
    capa["optional"]["color-format"]["display"] = "always";
  }

  inNDI::~inNDI() {
    dev.reset();
    sourceFinder.reset();
    NDI::deinitialize();
  }

  void inNDI::updateWebControlUrl() {
    std::string newUrl = dev->getWebControlUrl();
    if (newUrl != webControlUrl) {
      webControlUrl = newUrl;
      if (!webControlUrl.empty()) {
        INFO_MSG("NDI source web control URL: %s", webControlUrl.c_str());
        // Update capabilities to include web control URL
        capa["webcontrol"] = webControlUrl;
      } else {
        // Remove web control URL from capabilities if not available
        capa.removeMember("webcontrol");
      }
    }
  }

  void inNDI::streamMainLoop() {
    startTime = Util::bootSecs();
    Comms::Connections statComm;
    uint64_t lastMonitorTime = 0;

    // Frame buffers
    NDIlib_video_frame_v2_t video_frame = {0};
    NDIlib_audio_frame_v3_t audio_frame = {0};
    NDIlib_metadata_frame_t metadata_frame = {0};

    while (config->is_active) {
      // Connect to stats for INPUT detection
      if (!statComm) {
        statComm.reload(streamName, getConnectedBinHost(), JSON::Value(getpid()).asString(),
                        "INPUT:" + capa["name"].asStringRef(), "");
      }

      // Check for web control URL updates
      updateWebControlUrl();

      if (statComm) {
        uint64_t now = Util::bootSecs();
        statComm.setNow(now);
        statComm.setStream(streamName);
        statComm.setTime(now - startTime);
        statComm.setLastSecond(0);
        connStats(statComm);
      }

      // Monitor NDI performance every 5 seconds
      uint64_t now = time(nullptr);
      if (now - lastMonitorTime >= 5) {
        float video_fps = 0, dropped_fps = 0, audio_fps = 0;
        int video_q = 0, audio_q = 0, meta_q = 0;

        if (dev->getPerformanceInfo(video_fps, dropped_fps, audio_fps)) {
          INFO_MSG("NDI Performance - Video: %.1f fps (dropped: %.1f fps), Audio: %.1f fps", video_fps, dropped_fps, audio_fps);
          lastVideoFps = video_fps;
          lastDroppedFps = dropped_fps;
          lastAudioFps = audio_fps;
        }

        if (dev->getQueueInfo(video_q, audio_q, meta_q)) {
          INFO_MSG("NDI Queue Status - Video: %d frames, Audio: %d frames, Metadata: %d frames", video_q, audio_q, meta_q);
          lastVideoQueue = video_q;
          lastAudioQueue = audio_q;
          lastMetadataQueue = meta_q;
        }

        lastMonitorTime = now;
      }

      switch (dev->receiveAny(video_frame, audio_frame, metadata_frame)) {
        // We received video.
        case NDIlib_frame_type_video:
          bufferVideo(video_frame);
          dev->freeVideo(video_frame);
          break;

        // We received audio.
        case NDIlib_frame_type_audio:
          bufferAudio(audio_frame);
          dev->freeAudio(audio_frame);
          break;

        // We received a metadata packet
        case NDIlib_frame_type_metadata:
          bufferMetadata(metadata_frame);
          dev->freeMetadata(metadata_frame);
          break;

        // The device has changed status in some way
        case NDIlib_frame_type_status_change:
          // NDI sources may take a few seconds to report their full capabilities.
          // This status change indicates that device properties have been updated,
          // such as PTZ support, recording capabilities, or web control interface.
          //
          // In a discovery controller context, this would trigger a re-query of
          // device capabilities. For the input handler, we just log it and update
          // any immediately relevant properties like web control URL.
          INFO_MSG("NDI source status changed - device properties may have been updated");
          updateWebControlUrl(); // Check if web control URL changed
          break;

        // No audio or video has been received in the time-period.
        case NDIlib_frame_type_none: break;

        // An error occurred
        case NDIlib_frame_type_error: WARN_MSG("NDI frame receive error"); break;

        case NDIlib_frame_type_max: break;

        default:
          Util::sleep(10); // Small sleep to avoid busy loop
          break;
      }
    }
  }

  void inNDI::bufferVideo(const NDIlib_video_frame_v2_t & frame) {
    // Convert NDI timestamp (100ns units) to milliseconds
    uint64_t msTimestamp = frame.timestamp / 10000;

    if (!M.getBootMsOffset()) { meta.setBootMsOffset(Util::bootMS() - msTimestamp); }

    // Check for valid frame parameters
    if (!frame.p_data || !frame.line_stride_in_bytes || frame.xres == 0 || frame.yres == 0 || frame.frame_rate_N == 0 ||
        frame.frame_rate_D == 0) {
      if (hasFrames) {
        hasFrames = false;
        WARN_MSG("Lost video input signal! Invalid frame parameters: res=%ux%u, fps=%u/%u, "
                 "stride=%u, data=%p",
                 frame.xres, frame.yres, frame.frame_rate_N, frame.frame_rate_D, frame.line_stride_in_bytes, frame.p_data);
      }
      return;
    }

    // Check if video format has changed
    if (frame.xres != lastWidth || frame.yres != lastHeight || (frame.frame_rate_N * 1000 / frame.frame_rate_D) != lastFpks) {

      // Format changed, update track
      if (videoTrackIdx == INVALID_TRACK_ID) {
        // Create video track
        videoTrackIdx = meta.addTrack();
        meta.setID(videoTrackIdx, videoTrackIdx);
        meta.setType(videoTrackIdx, "video");
        meta.setCodec(videoTrackIdx, "UYVY");
      }

      INFO_MSG("Video format: %ux%u@%u", frame.xres, frame.yres, frame.frame_rate_N / frame.frame_rate_D);

      // Update track info
      meta.setWidth(videoTrackIdx, frame.xres);
      meta.setHeight(videoTrackIdx, frame.yres);
      meta.setFpks(videoTrackIdx, frame.frame_rate_N * 1000 / frame.frame_rate_D);
      meta.setBps(videoTrackIdx, frame.line_stride_in_bytes * 8 / frame.xres);

      lastWidth = frame.xres;
      lastHeight = frame.yres;
      lastFpks = frame.frame_rate_N * 1000 / frame.frame_rate_D;

      if (!hasFrames) {
        hasFrames = true;
        INFO_MSG("Regained video input with format %ux%u@%u", frame.xres, frame.yres, frame.frame_rate_N / frame.frame_rate_D);
      }
    }

    INSANE_MSG("Received video frame of %u bytes @ %" PRIu64, frame.line_stride_in_bytes * frame.yres, msTimestamp);
    thisIdx = videoTrackIdx;
    thisTime = msTimestamp;
    if (!userSelect.count(thisIdx)) {
      userSelect[thisIdx].reload(streamName, thisIdx, COMM_STATUS_ACTIVE | COMM_STATUS_SOURCE | COMM_STATUS_DONOTTRACK);
    }
    if (userSelect[thisIdx].getStatus() & COMM_STATUS_REQDISCONNECT) {
      Util::logExitReason(ER_CLEAN_LIVE_BUFFER_REQ, "buffer requested shutdown");
      config->is_active = false;
      return;
    }
    bufferLivePacket(thisTime, 0, thisIdx, (const char *)frame.p_data, frame.line_stride_in_bytes * frame.yres, 0, true);
  }

  void inNDI::bufferAudio(const NDIlib_audio_frame_v3_t & frame) {
    // Convert NDI timestamp (100ns units) to milliseconds
    uint64_t msTimestamp = frame.timestamp / 10000;

    if (!M.getBootMsOffset()) { meta.setBootMsOffset(Util::bootMS() - msTimestamp); }

    // Check for valid frame parameters
    if (!frame.p_data || frame.no_samples == 0 || frame.sample_rate == 0 || frame.no_channels == 0) {
      if (hasFrames) {
        hasFrames = false;
        WARN_MSG("Lost audio input signal! Invalid frame parameters: samples=%u, rate=%u Hz, "
                 "channels=%u, data=%p",
                 frame.no_samples, frame.sample_rate, frame.no_channels, frame.p_data);
      } else {
        WARN_MSG("No audio input signal detected");
      }
      return;
    }

    // Log audio frame details
    INSANE_MSG("Received audio frame: %u samples, %u channels @ %u Hz, timestamp: %" PRIu64 " ms", frame.no_samples,
               frame.no_channels, frame.sample_rate, msTimestamp);

    if (audioTrackIdx == INVALID_TRACK_ID) {
      // Create audio track
      audioTrackIdx = meta.addTrack();
      meta.setID(audioTrackIdx, audioTrackIdx);
      meta.setType(audioTrackIdx, "audio");
      meta.setCodec(audioTrackIdx, "PCM");
      INFO_MSG("Created new audio track with ID %zu", audioTrackIdx);
    }

    // Check for format changes
    if (frame.sample_rate != audioSampleRate || frame.no_channels != audioChannels) {

      INFO_MSG("Audio format changed: %u channels @ %u Hz (was: %u channels @ %u Hz)", frame.no_channels,
               frame.sample_rate, audioChannels, audioSampleRate);

      audioSampleRate = frame.sample_rate;
      audioChannels = frame.no_channels;

      meta.setRate(audioTrackIdx, audioSampleRate);
      meta.setChannels(audioTrackIdx, audioChannels);
      meta.setSize(audioTrackIdx, audioSampleDepth);
      INFO_MSG("Updated audio track configuration: %u Hz, %u channels, %u-bit", audioSampleRate, audioChannels, audioSampleDepth);

      if (!hasFrames) {
        hasFrames = true;
        INFO_MSG("Regained audio input with format: %u channels @ %u Hz", frame.no_channels, frame.sample_rate);
      }
    }

    size_t frameSize = frame.no_samples * frame.no_channels * sizeof(float);
    INSANE_MSG("Processing audio frame: %zu bytes, %u samples, %u channels @ %u Hz", frameSize, frame.no_samples,
               frame.no_channels, frame.sample_rate);
    thisIdx = audioTrackIdx;
    thisTime = msTimestamp;

    if (!userSelect.count(thisIdx)) {
      userSelect[thisIdx].reload(streamName, thisIdx, COMM_STATUS_ACTIVE | COMM_STATUS_SOURCE | COMM_STATUS_DONOTTRACK);
      HIGH_MSG("Initialized audio track selection for track %zu", thisIdx);
    }
    if (userSelect[thisIdx].getStatus() & COMM_STATUS_REQDISCONNECT) {
      Util::logExitReason(ER_CLEAN_LIVE_BUFFER_REQ, "buffer requested shutdown");
      config->is_active = false;
      return;
    }
    bufferLivePacket(thisTime, 0, thisIdx, (const char *)frame.p_data, frameSize, 0, true);
    INSANE_MSG("Buffered audio packet: %zu bytes @ %" PRIu64 " ms", frameSize, msTimestamp);
  }

  void inNDI::bufferMetadata(const NDIlib_metadata_frame_t & frame) {
    // Convert NDI timecode (100ns units) to milliseconds
    uint64_t msTimestamp = frame.timecode / 10000;

    if (!M.getBootMsOffset()) { meta.setBootMsOffset(Util::bootMS() - msTimestamp); }

    // Create metadata track if not exists
    if (metadataTrackIdx == INVALID_TRACK_ID) {
      metadataTrackIdx = meta.addTrack();
      meta.setID(metadataTrackIdx, metadataTrackIdx);
      meta.setType(metadataTrackIdx, "meta");
      meta.setCodec(metadataTrackIdx, "json");
      INFO_MSG("Created metadata track with ID %zu", metadataTrackIdx);
    }

    // Convert NDI metadata to JSON and buffer it
    if (frame.p_data && frame.length > 0) {
      std::string metaData(frame.p_data, frame.length);
      // Buffer the metadata packet with proper parameters
      thisIdx = metadataTrackIdx;
      thisTime = msTimestamp;
      bufferLivePacket(thisTime, 0, thisIdx, metaData.c_str(), metaData.length(), 0, true);
      INFO_MSG("Received NDI metadata: %s", metaData.c_str());
    }
  }

  /// @brief Writes a JSON list of connected NDI inputs to stdout
  JSON::Value inNDI::enumerateSources(const std::string & device) {
    JSON::Value result;
    // Try reading from controller's discovered cameras SHM (instant, no network scan)
    IPC::sharedPage camerasPage(SHM_CAMERAS, 0, false, false);
    if (camerasPage.mapped) {
      Util::RelAccX camAccX(camerasPage.mapped, false);
      if (camAccX.isReady()) {
        for (size_t i = 0; i < camAccX.getEndPos(); i++) {
          const char *proto = camAccX.getPointer("protocol", i);
          if (!proto || std::string(proto) != "ndi") continue;
          JSON::Value source;
          const char *n = camAccX.getPointer("name", i);
          const char *h = camAccX.getPointer("host", i);
          if (n) source["name"] = std::string(n);
          if (h) source["url"] = std::string(h);
          result.append(source);
        }
        if (result.size()) return result;
      }
    }
    // Fallback: local discovery (blocking, ~5s)
    uint32_t numSources = 0;
    const NDIlib_source_t *sources = sourceFinder->getSources(numSources);
    if (sources) {
      for (size_t i = 0; i < numSources; ++i) {
        JSON::Value source;
        source["name"] = sources[i].p_ndi_name;
        source["url"] = sources[i].p_url_address;
        result.append(source);
      }
    }
    return result;
  }

  /// @brief Writes a JSON list compatible pixel formats, resolution and FPS for a video input to stdout
  JSON::Value inNDI::getSourceCapa(const std::string & device) {
    return capa;
  }

  /// \brief Checks whether the device supports the given config. Stores refs to the NDI device for openStreamSource
  bool inNDI::checkArguments() {
    std::string device = config->getString("input");
    if (device.substr(0, 4) == "ndi:") { device = device.substr(4); }
    if (!device.size()) {
      Util::logExitReason(ER_FORMAT_SPECIFIC, "No explicit input device given. Use the 'enumerate' call to list all compatible devices");
      config->is_active = false;
      return false;
    }
    std::string colorFormat = config->getString("color-format");
    if (!colorFormat.empty()) { ndiColorFormat = colorFormat; }
    std::string bandwidth = config->getString("bandwidth");
    if (!bandwidth.empty()) { ndiBandwidth = bandwidth; }
    audioSampleDepth = 32;

    audioChannels = config->getInteger("channels");
    if (audioChannels != 2 && audioChannels != 8 && audioChannels != 16) {
      Util::logExitReason(ER_FORMAT_SPECIFIC,
                          "NDI only supports a 2, 8 or 16 channel count, but a value of %i was provided.", audioChannels);
      config->is_active = false;
      return false;
    }

    return true;
  }

  /// \brief Applies config to the video device and maps its buffer to a local pointer
  bool inNDI::openStreamSource() {
    uint32_t numSources = 0;
    const NDIlib_source_t *sources = sourceFinder->getSources(numSources);
    if (!sources || !numSources) {
      Util::logExitReason(ER_READ_START_FAILURE, "No NDI sources found");
      return false;
    }

    // Find requested source
    std::string reqSource = config->getString("input");
    if (reqSource.substr(0, 4) == "ndi:") { reqSource = reqSource.substr(4); }

    // Try to find source by name or URL
    const NDIlib_source_t *matchedSource = nullptr;
    for (size_t i = 0; i < numSources; ++i) {
      if (!sources[i].p_ndi_name || !sources[i].p_url_address) continue;

      INFO_MSG("NDI: Found source %s (%s)", sources[i].p_ndi_name, sources[i].p_url_address);

      // Match either by name or URL
      if (reqSource == sources[i].p_ndi_name || reqSource == sources[i].p_url_address) {
        matchedSource = &sources[i];
        INFO_MSG("NDI: Matched requested source %s to %s (%s)", reqSource.c_str(), sources[i].p_ndi_name, sources[i].p_url_address);
        break;
      }
    }

    if (!matchedSource) {
      std::string availableSources;
      for (size_t i = 0; i < numSources; ++i) {
        if (sources[i].p_ndi_name) {
          if (!availableSources.empty()) availableSources += ", ";
          availableSources += sources[i].p_ndi_name;
        }
      }
      Util::logExitReason(ER_READ_START_FAILURE, "Requested NDI source '%s' not found. Available sources: %s",
                          reqSource.c_str(), availableSources.c_str());
      return false;
    }

    // Set source
    if (!dev->setSource(matchedSource)) {
      Util::logExitReason(ER_READ_START_FAILURE, "Failed to set NDI source");
      return false;
    }

    // Only set config if we have explicit settings
    std::string colorFormat = config->getString("color-format");
    std::string bandwidth = config->getString("bandwidth");
    if (!colorFormat.empty() || !bandwidth.empty()) {
      NDI::InputConfig ndiConfig;
      ndiConfig.colorFormat = colorFormat;
      ndiConfig.bandwidth = bandwidth;
      ndiConfig.allowVideoFields = true;

      if (!dev->setConfig(ndiConfig)) {
        Util::logExitReason(ER_READ_START_FAILURE, "Failed to configure NDI input");
        return false;
      }
    }

    // Now connect
    if (!dev->connect()) {
      Util::logExitReason(ER_READ_START_FAILURE, "Failed to connect to NDI source");
      return false;
    }

    // Wait for first valid frame to arrive (up to 5 seconds)
    NDIlib_video_frame_v2_t video_frame = {0};
    NDIlib_audio_frame_v3_t audio_frame = {0};
    NDIlib_metadata_frame_t metadata_frame = {0};
    bool gotValidFrame = false;
    uint64_t startTime = Util::bootMS();

    INFO_MSG("Waiting for first NDI frame...");
    while (!gotValidFrame && (Util::bootMS() - startTime) < 5000) {
      switch (dev->receiveAny(video_frame, audio_frame, metadata_frame)) {
        case NDIlib_frame_type_video:
          if (video_frame.p_data && video_frame.line_stride_in_bytes && video_frame.xres > 0 && video_frame.yres > 0 &&
              video_frame.frame_rate_N > 0 && video_frame.frame_rate_D > 0) {
            bufferVideo(video_frame);
            gotValidFrame = (videoTrackIdx != INVALID_TRACK_ID);
            INFO_MSG("Received first valid video frame: %ux%u@%u", video_frame.xres, video_frame.yres,
                     video_frame.frame_rate_N / video_frame.frame_rate_D);
          }
          dev->freeVideo(video_frame);
          break;

        case NDIlib_frame_type_audio:
          if (audio_frame.p_data && audio_frame.no_samples > 0 && audio_frame.sample_rate > 0 && audio_frame.no_channels > 0) {
            bufferAudio(audio_frame);
            gotValidFrame = (audioTrackIdx != INVALID_TRACK_ID);
            INFO_MSG("Received first valid audio frame: %u channels @ %u Hz", audio_frame.no_channels, audio_frame.sample_rate);
          }
          dev->freeAudio(audio_frame);
          break;

        case NDIlib_frame_type_metadata:
          if (metadata_frame.p_data && metadata_frame.length > 0) {
            bufferMetadata(metadata_frame);
            gotValidFrame = (metadataTrackIdx != INVALID_TRACK_ID);
            INFO_MSG("Received first valid metadata frame");
          }
          dev->freeMetadata(metadata_frame);
          break;

        // No audio or video has been received in the time-period.
        case NDIlib_frame_type_none:
        // The device has changed status in some way
        case NDIlib_frame_type_status_change: break;
        // An error occurred
        case NDIlib_frame_type_error: break;
        case NDIlib_frame_type_max: break;
        default: Util::sleep(10); break;
      }
    }

    if (!gotValidFrame) {
      Util::logExitReason(ER_READ_START_FAILURE, "No valid frames received from NDI source after 5 seconds");
      return false;
    }

    meta.setLive(true);
    meta.setVod(false);
    return true;
  }

  void inNDI::closeStreamSource() {
    dev->disconnect();
  }

  bool inNDI::switchSource(const std::string & sourceName) {
    uint32_t numSources = 0;
    const NDIlib_source_t *sources = sourceFinder->getSources(numSources);
    if (!sources || !numSources) {
      WARN_MSG("NDI: No sources found");
      return false;
    }

    // Find requested source
    const NDIlib_source_t *targetSource = nullptr;
    for (size_t i = 0; i < numSources; ++i) {
      if (sourceName == sources[i].p_ndi_name) {
        targetSource = &sources[i];
        break;
      }
    }

    if (!targetSource) {
      WARN_MSG("NDI source '%s' not found", sourceName.c_str());
      return false;
    }

    // Disconnect current source
    dev->disconnect();

    // Set new source and reconnect
    if (!dev->setSource(targetSource)) {
      ERROR_MSG("Failed to set NDI source '%s'", sourceName.c_str());
      return false;
    }

    if (!dev->connect()) {
      ERROR_MSG("Failed to connect to NDI source '%s'", sourceName.c_str());
      return false;
    }

    // Reset track states for new source
    videoTrackIdx = INVALID_TRACK_ID;
    audioTrackIdx = INVALID_TRACK_ID;
    metadataTrackIdx = INVALID_TRACK_ID;

    INFO_MSG("Successfully switched to NDI source '%s'", sourceName.c_str());
    return true;
  }
} // namespace Mist

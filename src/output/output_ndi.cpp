#include "output_ndi.h"

#include <mist/defines.h>
#include <mist/stream.h>
#include <mist/timing.h>

#include <csignal>
#include <sstream>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace Mist {
  OutNDI::OutNDI(Socket::Connection & conn, Util::Config & cfg, JSON::Value & capa) : Output(conn, cfg, capa) {
    streamName = config->getString("streamname");
    parseData = true;
    wantRequest = false;

    // Initialize NDI library
    if (!NDI::initialize()) {
      Util::logExitReason(ER_READ_START_FAILURE, "Failed to initialize NDI library");
      config->is_active = false;
      return;
    }

    // Initialize metrics
    lastVideoFrame = 0;
    lastAudioFrame = 0;
    videoFramesSent = 0;
    videoFramesDropped = 0;
    audioFramesSent = 0;
    audioFramesDropped = 0;
    videoFramesSkipped = 0;
    audioFramesSkipped = 0;
    lastMetricsUpdate = Util::epoch();
    isPlaying = false;
    useAsyncVideo = config->getBool("async_video");
    asyncFramesSent = 0;
    asyncFramesSynced = 0;

    INFO_MSG("NDI output initialized with name '%s', async video %s", streamName.c_str(), useAsyncVideo ? "enabled" : "disabled");
  }

  OutNDI::~OutNDI() {
    if (isPlaying) {
      if (useAsyncVideo) {
        // Synchronize any pending async frames before shutdown
        syncVideoFrames();
      }
      dev.disconnect();
    }
    NDI::deinitialize();
  }

  void OutNDI::syncVideoFrames() {
    if (useAsyncVideo && asyncFramesSent > asyncFramesSynced) {
      // Send nullptr to synchronize pending frames
      dev.sendVideoAsync(nullptr);
      asyncFramesSynced = asyncFramesSent;
    }
  }

  void OutNDI::init(Util::Config *cfg, JSON::Value & capa) {
    Output::init(cfg, capa);
    capa["name"] = "NDI";
    capa["desc"] = "NDI output";
    capa["deps"] = "";
    capa["PUSHONLY"] = true;
    capa["required"]["streamname"]["name"] = "Stream";
    capa["required"]["streamname"]["help"] = "What streamname to serve";
    capa["required"]["streamname"]["type"] = "str";
    capa["required"]["streamname"]["option"] = "--stream";
    capa["required"]["streamname"]["short"] = "s";
    capa["codecs"][0u][0u].append("UYVY");
    capa["codecs"][0u][1u].append("PCM");
    capa["codecs"][0u][2u].append("JSON");
    capa["push_urls"].append("ndi:*");

    JSON::Value option;
    option["long"] = "json";
    option["short"] = "j";
    option["help"] = "Output connector info in JSON format, then exit.";
    option["value"].append((int64_t)0);
    cfg->addOption("json", option);

    JSON::Value opt;
    opt["arg"] = "string";
    opt["default"] = "";
    opt["arg_num"] = 1;
    opt["help"] = "Target NDI device name";
    cfg->addOption("target", opt);

    JSON::Value asyncOpt;
    asyncOpt["arg"] = "bool";
    asyncOpt["default"] = true;
    asyncOpt["help"] = "Enable asynchronous video frame sending for better performance";
    cfg->addOption("async_video", asyncOpt);

    cfg->addOption("streamname",
                   JSON::fromString("{\"arg\":\"string\",\"short\":\"s\",\"long\":"
                                    "\"stream\",\"help\":\"The name of the stream "
                                    "that this connector will transmit.\"}"));
    cfg->addOptionsFromCapabilities(capa);
  }

  void OutNDI::sendHeader() {
    // Check all tracks for video and audio
    bool hasVideo = false;
    bool hasAudio = false;
    bool hasMetadata = false;
    uint32_t videoWidth = 0;
    uint32_t videoHeight = 0;
    uint32_t videoFpks = 0;
    uint32_t audioRate = 0;
    uint32_t audioChannels = 0;
    uint32_t audioDepth = 0;

    // Scan tracks for configuration
    for (size_t i = 0; i < M.trackCount(); i++) {
      if (M.getType(i) == "video" && M.getCodec(i) == "UYVY") {
        hasVideo = true;
        videoWidth = M.getWidth(i);
        videoHeight = M.getHeight(i);
        videoFpks = M.getFpks(i);
        INFO_MSG("Found video track: %ux%u @ %u fps", videoWidth, videoHeight, videoFpks / 1000);
      }
      if (M.getType(i) == "audio" && M.getCodec(i) == "PCM") {
        hasAudio = true;
        audioRate = M.getRate(i);
        audioChannels = M.getChannels(i);
        audioDepth = M.getSize(i);
        INFO_MSG("Found audio track: %u channels @ %u Hz, %u-bit", audioChannels, audioRate, audioDepth);
      }
      if (M.getType(i) == "meta" && M.getCodec(i) == "json") {
        hasMetadata = true;
        INFO_MSG("Found metadata track");
      }
    }

    // Check if configuration changed
    bool configChanged = false;
    if (isPlaying) {
      if (hasVideo && (videoWidth != currentWidth || videoHeight != currentHeight || videoFpks != currentFpks)) {
        configChanged = true;
      }
      if (hasAudio && (audioRate != currentAudioRate || audioChannels != currentAudioChannels || audioDepth != currentAudioDepth)) {
        configChanged = true;
      }
      if (hasMetadata != currentHasMetadata) { configChanged = true; }
    }

    // If tracks changed or SIGHUP received, reinitialize
    if (!isPlaying || configChanged) {
      if (configChanged) { INFO_MSG("Track configuration changed, resetting NDI output"); }

      // Clean up existing output if any
      if (isPlaying) { dev.disconnect(); }

      if (!hasVideo && !hasAudio && !hasMetadata) {
        FAIL_MSG("No compatible video (UYVY), audio (PCM), or metadata (json) tracks found");
        config->is_active = false;
        return;
      }

      // Get target device if specified
      std::string target = config->getString("target");
      if (!target.empty()) {
        if (target.substr(0, 4) == "ndi:") { target = target.substr(4); }

        // Get NDI sources using Discovery
        NDI::Discovery finder;
        uint32_t numSources = 0;
        const NDIlib_source_t *sources = finder.getSources(numSources);
        if (!sources || !numSources) {
          INFO_MSG("No NDI sources found, proceeding with direct output");
        } else {
          // Find requested source
          bool found = false;
          for (size_t i = 0; i < numSources; ++i) {
            if (target == sources[i].p_ndi_name) {
              // Store source info for later use
              if (!dev.setSource(&sources[i])) {
                Util::logExitReason(ER_READ_START_FAILURE, "Failed to set NDI source");
                return;
              }
              found = true;
              break;
            }
          }
          if (!found) {
            INFO_MSG("Requested NDI source '%s' not found, proceeding with direct output", target.c_str());
          }
        }
      }

      // Configure output settings
      NDI::InputConfig outputConfig;
      if (hasVideo) {
        // Set video format to match input
        outputConfig.colorFormat = "UYVY_BGRA"; // NDI expects UYVY
      }
      if (hasAudio) {
        // NDI audio is always 32-bit float, stereo at 48kHz
        // The NDI SDK will handle conversion if needed
      }
      if (!dev.setConfig(outputConfig)) { WARN_MSG("Failed to set NDI output configuration, using defaults"); }

      dev.setName(streamName);
      if (!dev.openOutput()) {
        Util::logExitReason(ER_INTERNAL_ERROR, "Failed to start NDI output");
        config->is_active = false;
        return;
      }

      // Store current configuration
      currentWidth = videoWidth;
      currentHeight = videoHeight;
      currentFpks = videoFpks;
      currentAudioRate = audioRate;
      currentAudioChannels = audioChannels;
      currentAudioDepth = audioDepth;
      currentHasMetadata = hasMetadata;

      isPlaying = true;

      // Reset metrics
      videoFramesSent = 0;
      audioFramesSent = 0;
      metadataFramesSent = 0;
      videoFramesDropped = 0;
      audioFramesDropped = 0;
      metadataFramesDropped = 0;
      lastMetricsUpdate = Util::epoch();

      INFO_MSG("NDI output initialized with name '%s'", streamName.c_str());
    }
  }

  void OutNDI::sendNext() {
    // Check if we need to reset the output (from SIGHUP or track changes)
    if (selectDefaultTracks()) {
      INFO_MSG("Resetting NDI output due to configuration change or SIGHUP");

      // Clean up existing connection
      if (isPlaying) {
        dev.disconnect();
        isPlaying = false;
      }
      // Reinitialize connection
      sentHeader = false;
      return;
    }

    // Get packet data
    size_t dataLen;
    char *dataPointer;
    thisPacket.getString("data", dataPointer, dataLen);

    // Handle video frames
    if (M.getType(thisIdx) == "video") {
      NDIlib_video_frame_v2_t video_frame = {0};
      video_frame.xres = M.getWidth(thisIdx);
      video_frame.yres = M.getHeight(thisIdx);
      if (video_frame.xres == 0 || video_frame.yres == 0) {
        WARN_MSG("Invalid video dimensions %ux%u", video_frame.xres, video_frame.yres);
        return;
      }
      video_frame.FourCC = NDIlib_FourCC_type_UYVY;
      video_frame.frame_rate_N = M.getFpks(thisIdx);
      video_frame.frame_rate_D = 1000;
      video_frame.picture_aspect_ratio = (float)video_frame.xres / video_frame.yres;
      video_frame.frame_format_type = NDIlib_frame_format_type_progressive;
      video_frame.timecode = thisTime;
      video_frame.p_data = (uint8_t *)dataPointer;
      video_frame.line_stride_in_bytes = dataLen / video_frame.yres;
      video_frame.p_metadata = nullptr;
      video_frame.timestamp = thisTime;

      // Check for connected viewers before sending
      // if (NDIlib_send_get_no_connections(dev.getSender(), 0) == 0) {
      //   videoFramesSkipped++;
      //   return;
      // }

      if (useAsyncVideo) {
        // Send video frame asynchronously
        if (!dev.sendVideoAsync(&video_frame)) {
          videoFramesDropped++;
          WARN_MSG("Failed to send video frame asynchronously");
        } else {
          videoFramesSent++;
          asyncFramesSent++;
          // Sync frames periodically to avoid too much latency
          if (asyncFramesSent - asyncFramesSynced >= 5) { syncVideoFrames(); }
        }
      } else {
        // Send video frame synchronously
        if (!dev.sendVideo(video_frame)) {
          videoFramesDropped++;
          WARN_MSG("Failed to send video frame");
        } else {
          videoFramesSent++;
          lastVideoFrame = thisTime;
        }
      }
    }

    // Handle audio frames
    if (M.getType(thisIdx) == "audio") {
      NDIlib_audio_frame_v3_t audio_frame = {0};
      audio_frame.sample_rate = M.getRate(thisIdx);
      audio_frame.no_channels = M.getChannels(thisIdx);
      uint32_t bytesPerSample = M.getSize(thisIdx) / 8;
      if (audio_frame.no_channels == 0 || bytesPerSample == 0) {
        WARN_MSG("Invalid audio format: %u channels, %u-bit", audio_frame.no_channels, M.getSize(thisIdx));
        return;
      }
      audio_frame.no_samples = dataLen / (audio_frame.no_channels * bytesPerSample);
      audio_frame.timecode = thisTime;
      audio_frame.p_data = (uint8_t *)dataPointer;
      audio_frame.channel_stride_in_bytes = audio_frame.no_samples * bytesPerSample;
      audio_frame.p_metadata = nullptr;
      audio_frame.timestamp = thisTime;

      // Check for connected viewers before sending
      // if (NDIlib_send_get_no_connections(dev.getSender(), 0) == 0) {
      //   audioFramesSkipped++;
      //   return;
      // }

      if (!dev.sendAudio(audio_frame)) {
        audioFramesDropped++;
        WARN_MSG("Failed to send audio frame");
      } else {
        audioFramesSent++;
        lastAudioFrame = thisTime;
      }
    }

    // Handle metadata frames
    if (M.getType(thisIdx) == "meta" && M.getCodec(thisIdx) == "json") {
      // Get packet data
      std::string dataStr;
      thisPacket.getString("data", dataStr);

      // Prepare NDI metadata frame
      NDIlib_metadata_frame_t metadata_frame = {0};
      metadata_frame.length = dataStr.length();
      metadata_frame.timecode = thisTime;
      metadata_frame.p_data = const_cast<char *>(dataStr.c_str());

      if (!dev.sendMetadata(metadata_frame)) {
        metadataFramesDropped++;
        WARN_MSG("Failed to send metadata frame");
      } else {
        metadataFramesSent++;
      }
    }

    // Update metrics every 5 seconds
    uint64_t now = Util::epoch();
    if (now > lastMetricsUpdate + 5) {
      // Write prometheus-style metrics
      char tmpFileName[] = "/tmp/mistXXXXXX";
      int fd = mkstemp(tmpFileName);
      if (fd != -1) {
        // Sanitize streamName for filename (non-alnum to underscore)
        std::string safeName = streamName;
        for (auto & c : safeName) {
          if (!isalnum(c) && c != '_' && c != '-') c = '_';
        }
        std::string targetFileName = "/tmp/" + safeName + "_ndi_metrics.txt";
        std::stringstream contents;
        // Escape streamName for Prometheus label
        std::string escapedName;
        for (char c : streamName) {
          if (c == '\\')
            escapedName += "\\\\";
          else if (c == '"')
            escapedName += "\\\"";
          else if (c == '\n')
            escapedName += "\\n";
          else
            escapedName += c;
        }
        std::string tag = "{stream=\"" + escapedName + "\"} ";

        contents << "# NDI Output Metrics\n";

        // Frame counters
        contents << "# HELP ndi_video_frames_sent Total number of video frames sent to NDI\n";
        contents << "# TYPE ndi_video_frames_sent counter\n";
        contents << "ndi_video_frames_sent" << tag << videoFramesSent << "\n";
        contents << "# HELP ndi_video_frames_dropped Total number of video frames dropped\n";
        contents << "# TYPE ndi_video_frames_dropped counter\n";
        contents << "ndi_video_frames_dropped" << tag << videoFramesDropped << "\n";
        contents << "# HELP ndi_video_frames_skipped Number of video frames skipped\n";
        contents << "# TYPE ndi_video_frames_skipped counter\n";
        contents << "ndi_video_frames_skipped" << tag << videoFramesSkipped << "\n";

        // Async metrics
        if (useAsyncVideo) {
          contents << "# HELP ndi_async_frames_sent Total number of async video frames sent\n";
          contents << "# TYPE ndi_async_frames_sent counter\n";
          contents << "ndi_async_frames_sent" << tag << asyncFramesSent << "\n";
          contents << "# HELP ndi_async_frames_synced Total number of async video frames synced\n";
          contents << "# TYPE ndi_async_frames_synced counter\n";
          contents << "ndi_async_frames_synced" << tag << asyncFramesSynced << "\n";
          contents << "# HELP ndi_async_frames_pending Number of async video frames pending sync\n";
          contents << "# TYPE ndi_async_frames_pending gauge\n";
          contents << "ndi_async_frames_pending" << tag << (asyncFramesSent - asyncFramesSynced) << "\n";
        }

        // Audio counters
        contents << "# HELP ndi_audio_frames_sent Total number of audio frames sent to NDI\n";
        contents << "# TYPE ndi_audio_frames_sent counter\n";
        contents << "ndi_audio_frames_sent" << tag << audioFramesSent << "\n";
        contents << "# HELP ndi_audio_frames_dropped Total number of audio frames dropped\n";
        contents << "# TYPE ndi_audio_frames_dropped counter\n";
        contents << "ndi_audio_frames_dropped" << tag << audioFramesDropped << "\n";
        contents << "# HELP ndi_audio_frames_skipped Number of audio frames skipped\n";
        contents << "# TYPE ndi_audio_frames_skipped counter\n";
        contents << "ndi_audio_frames_skipped" << tag << audioFramesSkipped << "\n";

        // Metadata metrics
        contents << "# HELP ndi_metadata_frames_sent Total number of metadata frames sent to NDI\n";
        contents << "# TYPE ndi_metadata_frames_sent counter\n";
        contents << "ndi_metadata_frames_sent" << tag << metadataFramesSent << "\n";
        contents << "# HELP ndi_metadata_frames_dropped Total number of metadata frames dropped\n";
        contents << "# TYPE ndi_metadata_frames_dropped counter\n";
        contents << "ndi_metadata_frames_dropped" << tag << metadataFramesDropped << "\n";

        // Stream state
        contents << "# HELP ndi_stream_time Current stream timestamp\n";
        contents << "# TYPE ndi_stream_time gauge\n";
        contents << "ndi_stream_time" << tag << thisTime << "\n";
        contents << "# HELP ndi_width Current video width\n";
        contents << "# TYPE ndi_width gauge\n";
        contents << "ndi_width" << tag << currentWidth << "\n";
        contents << "# HELP ndi_height Current video height\n";
        contents << "# TYPE ndi_height gauge\n";
        contents << "ndi_height" << tag << currentHeight << "\n";
        contents << "# HELP ndi_framerate Current video framerate\n";
        contents << "# TYPE ndi_framerate gauge\n";
        contents << "ndi_framerate" << tag << currentFpks / 1000 << "\n";

        std::string data = contents.str();
        ssize_t written = write(fd, data.c_str(), data.size());
        close(fd);
        if (written == (ssize_t)data.size()) {
          rename(tmpFileName, targetFileName.c_str());
        } else {
          WARN_MSG("Failed to write metrics file %s", tmpFileName);
          unlink(tmpFileName);
        }
      } else {
        WARN_MSG("Unable to create temporary metrics file");
      }
      lastMetricsUpdate = now;
    }
  }
} // namespace Mist

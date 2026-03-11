#include "process_av.h"

#include "defines.h"
#include "lib/defines.h"
#include "mist/ffmpeg/node_pipeline.h"
#include "process.hpp"

#include <mist/h264.h>
#include <mist/h265.h>
#include <mist/mp4_generic.h>
#include <mist/nal.h>
#include <mist/proc_stats.h>
#include <mist/procs.h>
#include <mist/shared_memory.h>
#include <mist/socket.h>
#include <mist/triggers.h>
#include <mist/util.h>

#include <algorithm>
#include <cctype>
#include <exception>
#include <mutex>
#include <sys/stat.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>

// Global state variables
FFmpeg::PipelineConfig pipelineConfig;
FFmpeg::NodePipeline pipeline; // Initialize pipeline
                                 //
namespace Mist {
  Util::Config co; // Global config
  Util::Config conf; // Configuration instance

  // Statistics and state tracking
  JSON::Value pStat; // Process statistics
  JSON::Value & pData = pStat["proc_status_update"]["status"]; // Status data reference
  std::mutex statsMutex; // Mutex for statistics access
  uint64_t statSinkMs = 0; // Sink timestamp
  uint64_t statSourceMs = 0; // Source timestamp
  int64_t bootMsOffset = 0; // Boot time offset
  bool isRawVideo = false; // Raw video mode flag

  // Performance tracking
  uint64_t encPrevTime = 0; // Previous encode time
  uint64_t encPrevCount = 0; // Previous encode count
  uint64_t decPrevTime = 0; // Previous decode time
  uint64_t decPrevCount = 0; // Previous decode count

  // Global state
  std::atomic_uint64_t inFrames;
  std::atomic_uint64_t outFrames;
  bool getFirst = false;
  bool sendFirst = false;
  uint64_t packetTimeDiff = 0;
  uint64_t sendPacketTime = 0;
  JSON::Value opt;
  ProcessSink *sinkClass = nullptr;

  // Codec-related globals
  bool isVideo = true;
  std::string codecOut;

  // Process timing stats SHM for rate control (read by input_buffer.cpp)
  IPC::sharedPage procStatsPage;

  // Virtual segment trigger state
  uint64_t lastTriggerTime = 0;
  uint64_t processStartTime = 0;
  std::atomic_uint64_t totalOutputBytes{0};
  std::atomic_uint64_t totalInputBytes{0};
  uint64_t prevInFrames = 0;
  uint64_t prevOutFrames = 0;
  uint64_t prevOutputBytes = 0;
  uint64_t prevInputBytes = 0;
  uint64_t prevSourceMs = 0;
  uint64_t prevSinkMs = 0;

  // Runtime dimensions (tracked from source/sink threads)
  std::atomic_uint64_t lastInWidth{0};
  std::atomic_uint64_t lastInHeight{0};
  std::atomic_uint64_t lastInFpks{0};
  std::atomic_uint64_t lastOutWidth{0};
  std::atomic_uint64_t lastOutHeight{0};
  std::atomic_uint64_t lastSampleRate{0};
  std::atomic_uint64_t lastChannels{0};

  void fireVirtualSegmentTrigger(bool isFinal);

  // Method implementations for ProcessSink
  ProcessSink::ProcessSink(Util::Config *cfg) : Input(cfg) {
    capa["name"] = "AV";
    streamName = opt["sink"].asString();
    if (!streamName.size()) { streamName = opt["source"].asString(); }
    Util::streamVariables(streamName, opt["source"].asString());
    {
      std::lock_guard<std::mutex> guard(statsMutex);
      pStat["proc_status_update"]["sink"] = streamName;
      pStat["proc_status_update"]["source"] = opt["source"];
    }
    Util::setStreamName(opt["source"].asString() + "→" + streamName);
    if (opt.isMember("target_mask") && !opt["target_mask"].isNull() && opt["target_mask"].asString() != "") {
      DTSC::trackValidDefault = opt["target_mask"].asInt();
    } else {
      std::string codec = opt["codec"].asString();
      if (codec == "UYVY" || codec == "YUYV" || codec == "PCM" || codec == "NV12") {
        DTSC::trackValidDefault = TRACK_VALID_EXT_HUMAN | TRACK_VALID_INT_PROCESS;
      } else {
        DTSC::trackValidDefault = TRACK_VALID_EXT_HUMAN | TRACK_VALID_EXT_PUSH;
      }
    }
  }

  ProcessSink::~ProcessSink() = default;

  void ProcessSink::setNowMS(uint64_t t) {
    if (!userSelect.size()) { return; }
    meta.setNowms(userSelect.begin()->first, t);
  }

  void ProcessSink::parseH264(bool isKey, const char *bufIt, uint64_t bufSize) {
    const char *nextPtr;
    const char *pesEnd = bufIt + bufSize;
    uint32_t nalSize = 0;

    // Parse H264 packet data (slice data only when using AV_CODEC_FLAG_GLOBAL_HEADER)
    nextPtr = nalu::scanAnnexB(bufIt, bufSize);
    if (!nextPtr) {
      WARN_MSG("Sink: Unable to find AnnexB data in the H264 buffer");
      return;
    }
    thisPacket.null();
    while (nextPtr < pesEnd) {
      if (!nextPtr) { nextPtr = pesEnd; }
      // Calculate size of NAL unit, removing null bytes from the end
      nalSize = nalu::nalEndPosition(bufIt, nextPtr - bufIt) - bufIt;
      if (nalSize) {
        // If we don't have a packet yet, init an empty packet
        if (!thisPacket) { thisPacket.genericFill(thisTime, 0, thisIdx, 0, 0, 0, isKey); }
        // Note: With AV_CODEC_FLAG_GLOBAL_HEADER, SPS/PPS are in extradata, not packet data
        // Packets only contain slice data (NAL types 0x01, 0x05, etc.)
        thisPacket.appendNal(bufIt, nalSize);
      }
      if (((nextPtr - bufIt) + 3) >= bufSize) { break; } // end of the line
      bufSize -= ((nextPtr - bufIt) + 3); // decrease the total size
      bufIt = nextPtr + 3;
      nextPtr = nalu::scanAnnexB(bufIt, bufSize);
    }
  }

  void ProcessSink::streamMainLoop() {
    uint64_t statTimer = 0;
    Comms::Connections statComm;

    while (config->is_active) {

      Util::sleep(5000);

      {
        std::lock_guard<std::mutex> lock(sinkMutex);
        // Update stats periodically
        uint64_t now = Util::bootSecs();
        if (now > statTimer) {
          connStats(statComm);
          statTimer = now + 1;
        }
      }
    }
  }

  void ProcessSink::onData(void *data) {
    std::lock_guard<std::mutex> lock(sinkMutex);
    FFmpeg::PacketContext *packet = (FFmpeg::PacketContext *)data;

    if (!packet) { return; }

    VERYHIGH_MSG("Sink: got packet: size=%d, time=%" PRIu64 ", isKey=%d", packet->getSize(), packet->getPts(), packet->isKeyframe());

    size_t trackIdx = INVALID_TRACK_ID;
    bool newTrack = false;

    // Use encoder node ID to determine track ID
    size_t encoderNodeId = packet->getEncoderNodeId();
    auto trackIt = encoderToTrackMap.find(encoderNodeId);
    if (trackIt != encoderToTrackMap.end()) {
      trackIdx = trackIt->second;
      newTrack = false;
    } else {
      trackIdx = INVALID_TRACK_ID;
      newTrack = true;
    }

    thisTime = packet->getPts();
    if (thisTime >= statSinkMs) { statSinkMs = thisTime; }

    // Get format info from packet
    FFmpeg::PacketContext::FormatInfo formatInfo = packet->getFormatInfo();

    // Get packet data
    const char *packetData = (const char *)packet->getData();
    size_t packetSize = packet->getSize();
    bool isKey = packet->isKeyframe();

    if (!packetData || !packetSize) {
      WARN_MSG("Skipping empty packet");
      return;
    }

    // Init the track, if needed
    if (trackIdx == INVALID_TRACK_ID) {
      if (!isVideo) {
        setAudioInit(trackIdx, formatInfo.codecName, formatInfo.sampleRate, formatInfo.channels, formatInfo.bitDepth,
                     packet->getCodecData());
      } else if (isRawVideo) {
        setRawVideoInit(trackIdx, formatInfo.codecName, formatInfo.width, formatInfo.height, formatInfo.fpks);
      } else {
        setVideoInit(trackIdx, formatInfo.codecName, formatInfo.width, formatInfo.height, packet->getCodecData(),
                     formatInfo.fpks);
      }
      if (trackIdx == INVALID_TRACK_ID) { return; }
    }
    thisIdx = trackIdx;


    // Buffer the actual track data
    if (!isVideo || isRawVideo || formatInfo.codecName == "JPEG") {
      // JPEG, audio and raw video are always keyframes and have no offsets
      bufferLivePacket(thisTime, 0, thisIdx, packetData, packetSize, 0, true);
    } else if (formatInfo.codecName == "H264") {
      parseH264(isKey, packetData, packetSize);
      if (!thisPacket) {
        VERYHIGH_MSG("Sink: H264 packet parsing failed or incomplete");
        return;
      }
      bufferLivePacket(thisPacket);
    } else {
      // All other tracks use the generic handler
      bufferLivePacket(thisTime, packet->getDts() - thisTime, trackIdx, packetData, packetSize, 0, isKey);
    }

    outFrames.fetch_add(1);
    totalOutputBytes.fetch_add(packetSize);

    // Update encoder to track mapping if a new track was created
    if (trackIdx != INVALID_TRACK_ID && newTrack) {
      encoderToTrackMap[encoderNodeId] = trackIdx;
      INFO_MSG("Sink: Mapped encoder node %zu to track %zu", encoderNodeId, trackIdx);
    }
  }

  /// \brief Sets init data based on the last loaded SPS and PPS data
  void ProcessSink::setVideoInit(size_t & trackIdx, std::string codecOut, uint64_t outWidth, uint64_t outHeight,
                                 const std::string & extradata, uint64_t outFpks) {
    if (codecOut == "H265") { codecOut = "HEVC"; }
    if (codecOut == "JPEG") { outFpks = 0; }
    // For H.264, require SPS/PPS before creating track
    if (codecOut == "H264" && !extradata.size()) {
      INFO_MSG("Sink: H.264 track creation aborted - no init data available");
      trackIdx = INVALID_TRACK_ID;
      return;
    }

    if (codecOut == "HEVC" && !extradata.size()) {
      INFO_MSG("Sink: HEVC track creation aborted - no VPS (%zu bytes) or SPS (%zu bytes) or PPS "
               "(%zu bytes) available",
               vpsInfo.size(), spsInfo.size(), ppsInfo.size());
      trackIdx = INVALID_TRACK_ID;
      return;
    }

    // Create a new track
    meta.reInit(streamName, false);
    trackIdx = meta.addTrack();
    meta.setCodec(trackIdx, codecOut);
    meta.setType(trackIdx, "video");
    meta.setID(trackIdx, trackIdx);
    meta.setWidth(trackIdx, outWidth);
    meta.setHeight(trackIdx, outHeight);
    meta.setFpks(trackIdx, outFpks);
    meta.setInit(trackIdx, extradata);
    meta.markUpdated(trackIdx);
    lastOutWidth.store(outWidth, std::memory_order_relaxed);
    lastOutHeight.store(outHeight, std::memory_order_relaxed);
    INFO_MSG("Sink: %s track index is %zu", codecOut.c_str(), trackIdx);

    if (trackIdx != INVALID_TRACK_ID && !userSelect.count(trackIdx)) {
      userSelect[trackIdx].reload(streamName, trackIdx, COMM_STATUS_ACTIVE | COMM_STATUS_SOURCE | COMM_STATUS_DONOTTRACK);
    }
  }

  /// \brief Sets init data based on the last loaded SPS and PPS data
  void ProcessSink::setRawVideoInit(size_t & trackIdx, std::string codecOut, uint64_t outWidth, uint64_t outHeight, uint64_t outFpks) {
    // Create a new track
    meta.reInit(streamName, false);
    size_t staticSize = Util::pixfmtToSize(codecOut, outWidth, outHeight);
    if (staticSize) {
      // Known static frame sizes: raw track mode
      trackIdx = meta.addTrack(0, 0, 0, 0, true, staticSize);
    } else {
      // Other cases: standard track mode
      trackIdx = meta.addTrack();
    }

    // Mark track as updated - this is critical for MistServer to recognize the track
    meta.markUpdated(trackIdx);

    meta.setCodec(trackIdx, codecOut);
    meta.setType(trackIdx, "video");
    meta.setID(trackIdx, trackIdx);
    meta.setWidth(trackIdx, outWidth);
    meta.setHeight(trackIdx, outHeight);
    meta.setFpks(trackIdx, outFpks);
    if (trackIdx != INVALID_TRACK_ID && !userSelect.count(trackIdx)) {
      userSelect[trackIdx].reload(streamName, trackIdx, COMM_STATUS_ACTIVE | COMM_STATUS_SOURCE | COMM_STATUS_DONOTTRACK);
    }
    INFO_MSG("Sink: Created new %s track with index %zu", codecOut.c_str(), trackIdx);
  }

  void ProcessSink::setAudioInit(size_t & trackIdx, std::string codecOut, uint64_t outSampleRate, uint64_t outChannels,
                                 uint64_t outBitDepth, const std::string & extraData) {
    // For AAC, OPUS, FLAC and Vorbis, we need to set the init data
    // PCM and MP3 don't need it.
    if ((codecOut != "PCM" && codecOut != "MP3") && !extraData.size()) {
      INFO_MSG("Sink: No audio extradata provided for codec %s", codecOut.c_str());
      trackIdx = INVALID_TRACK_ID;
      return;
    }

    // Create a new track
    meta.reInit(streamName, false);
    trackIdx = meta.addTrack();

    // Mark track as updated - this is critical for MistServer to recognize the track
    meta.markUpdated(trackIdx);

    meta.setType(trackIdx, "audio");
    meta.setCodec(trackIdx, codecOut);
    meta.setID(trackIdx, 1);
    meta.setRate(trackIdx, outSampleRate);
    meta.setChannels(trackIdx, outChannels);
    meta.setSize(trackIdx, outBitDepth);
    meta.setInit(trackIdx, extraData);

    if (trackIdx != INVALID_TRACK_ID && !userSelect.count(trackIdx)) {
      userSelect[trackIdx].reload(streamName, trackIdx, COMM_STATUS_ACTIVE | COMM_STATUS_SOURCE | COMM_STATUS_DONOTTRACK);
    }
    INFO_MSG("Sink: Created new %s track with index %zu", codecOut.c_str(), trackIdx);
  }

  void ProcessSink::connStats(Comms::Connections & statComm) {
    for (std::map<size_t, Comms::Users>::iterator it = userSelect.begin(); it != userSelect.end(); it++) {
      if (it->second) { it->second.setStatus(COMM_STATUS_DONOTTRACK | it->second.getStatus()); }
    }
  }

  // ProcAV implementation
  ProcAV::ProcAV() : isActive(false) {
    // Empty constructor - all initialization moved to Run()
  }

  ProcAV::~ProcAV() {
    Stop();
  }

  void ProcAV::Run() {
    // Configure pipeline type based on output codec
    std::string codecOut = opt["codec"].asString();
    pipeline.setIsVideo(codecOut == "H264" || codecOut == "AV1" || codecOut == "HEVC" || codecOut == "H265" || codecOut == "VP9" ||
                        codecOut == "JPEG" || codecOut == "YUYV" || codecOut == "UYVY" || codecOut == "NV12");

    // Set raw video flag for raw formats
    if (codecOut == "YUYV" || codecOut == "UYVY" || codecOut == "NV12") {
      Mist::isRawVideo = true;
      INFO_MSG("Main: Raw video mode enabled for codec: %s", codecOut.c_str());
    }

    // Initialize ProcessSource static configuration first
    JSON::Value capa;
    Mist::ProcessSource::init(&conf, capa);
    pipeline.setIsActive(true);
    MEDIUM_MSG("Main: Pipeline activated");

    // Start sink thread first
    MEDIUM_MSG("Main: Starting sink thread...");
    sinkThread = std::thread(&ProcAV::runSinkThread, this);

    // Wait for sink to initialize
    {
      std::unique_lock<std::mutex> lock(threadMutex);
      threadCV.wait(lock, [this]() { return sink != nullptr; });
    }
    MEDIUM_MSG("Main: Sink thread initialized");

    pipeline.addCallback([](void *d) { sinkClass->onData(d); });

    // Start source thread
    MEDIUM_MSG("Main: Starting source thread...");
    sourceThread = std::thread(&ProcAV::runSourceThread, this);

    // Wait for source to initialize
    {
      std::unique_lock<std::mutex> lock(threadMutex);
      threadCV.wait(lock, [this]() { return source != nullptr; });
    }
    MEDIUM_MSG("Main: Source thread initialized");

    isActive = true;

    // Initialize stats
    {
      std::lock_guard<std::mutex> guard(statsMutex);
      pStat["proc_status_update"]["id"] = getpid();
      pStat["proc_status_update"]["proc"] = "AV";
    }

    // Initialize process timing stats SHM for rate control
    {
      char statsName[NAME_BUFFER_SIZE];
      snprintf(statsName, NAME_BUFFER_SIZE, SHM_PROC_STATS, getpid());
      procStatsPage.init(statsName, sizeof(ProcTimingStats), true, false);
    }

    // Monitor and update stats
    uint64_t startTime = Util::bootSecs();
    processStartTime = startTime;

    // Main monitoring loop - keep running while threads are active
    while (isActive && conf.is_active && co.is_active) {
      Util::sleep(200);
      updateStats(startTime);
    }

    // Wait for threads to finish
    if (sinkThread.joinable()) { sinkThread.join(); }
    if (sourceThread.joinable()) { sourceThread.join(); }

    // Wait for pipeline to finish
    pipeline.waitForInactive();
    MEDIUM_MSG("Main: Pipeline finished");

    // Fire final trigger after all frames are drained
    fireVirtualSegmentTrigger(true);
  }

  void ProcAV::Stop() {
    // Stop processing
    isActive = false;
    conf.is_active = false;
    co.is_active = false;
    pipeline.setIsActive(false);

    // Clean up source/sink
    source.reset();
    sink.reset();

    // Wait for threads
    if (sinkThread.joinable()) { sinkThread.join(); }
    if (sourceThread.joinable()) { sourceThread.join(); }
  }

  // Member thread functions
  void ProcAV::runSourceThread() {

    Util::nameThread("sourceThread");
    MEDIUM_MSG("Main: Running source thread...");

    // First initialize the ProcessSource static configuration
    JSON::Value capa;
    ProcessSource::init(&conf, capa);
    conf.is_active = true;

    // Now set the streamname option
    conf.getOption("streamname", true).append(opt["source"].c_str());

    // Add and configure target option
    JSON::Value trackOpt;
    trackOpt["arg"] = "string";
    trackOpt["default"] = "";
    trackOpt["arg_num"] = 1;
    conf.addOption("target", trackOpt);
    conf.getOption("target", true).append("-");

    // Configure track selection
    std::string video_select = "maxbps";
    if (opt.isMember("source_track") && opt["source_track"].isString() && opt["source_track"]) {
      video_select = opt["source_track"].asStringRef();
    }
    if (opt.isMember("track_select")) { conf.getOption("target", true).append("-?" + opt["track_select"].asString()); }

    // Create and run source with persistent connection
    Socket::Connection conn;
    {
      std::lock_guard<std::mutex> lock(threadMutex);
      source = std::unique_ptr<ProcessSource>(new ProcessSource(conn, conf, capa));
      threadCV.notify_all();
    }

    if (source) {
      MEDIUM_MSG("Main: Starting source thread...");
      source->run();
      MEDIUM_MSG("Main: Source thread finished");
    }

    INFO_MSG("Main: Stop source thread: %s", Util::exitReason);
    co.is_active = false;
    pipeline.setIsActive(false);
    isActive = false;
  }

  void ProcAV::runSinkThread() {
    Util::nameThread("sinkThread");
    MEDIUM_MSG("Main: Running sink thread...");

    // Initialize Output base class and sink config first
    JSON::Value capa;
    Output::init(&co, capa);
    co.is_active = true;

    // Add required options before creating sink
    JSON::Value opt;
    opt["arg"] = "string";
    opt["default"] = "";
    opt["arg_num"] = 1;
    co.addOption("output", opt);
    co.getOption("output", true).append("-");

    // Create and configure sink
    {
      std::lock_guard<std::mutex> lock(threadMutex);
      sink = std::unique_ptr<ProcessSink>(new ProcessSink(&co));
      sinkClass = sink.get();
      threadCV.notify_all();
    }

    // Run sink loop
    if (sink) { sink->run(); }

    INFO_MSG("Main: Stop sink thread...");
    conf.is_active = false;
    pipeline.setIsActive(false);
    isActive = false;
  }

  void ProcessSource::init(Util::Config *cfg, JSON::Value & capa) {
    // Initialize Output base class first
    Output::init(cfg, capa);

    // Set capabilities
    capa["name"] = "AV";

    // Add codec capabilities based on pipeline type
    if (pipeline.getIsVideo()) {
      capa["codecs"][0u][0u].append("YUYV");
      capa["codecs"][0u][0u].append("UYVY");
      capa["codecs"][0u][0u].append("NV12");
      capa["codecs"][0u][0u].append("H264");
      capa["codecs"][0u][0u].append("AV1");
      capa["codecs"][0u][0u].append("HEVC");
      capa["codecs"][0u][0u].append("VP9");
      capa["codecs"][0u][0u].append("JPEG");
    } else {
      capa["codecs"][0u][0u].append("PCM");
      capa["codecs"][0u][0u].append("opus");
      capa["codecs"][0u][0u].append("AAC");
      capa["codecs"][0u][0u].append("MP3");
      capa["codecs"][0u][0u].append("FLAC");
      capa["codecs"][0u][0u].append("vorbis");
    }

    // Add streamname option
    cfg->addOption("streamname",
                   JSON::fromString("{\"arg\":\"string\",\"short\":\"s\",\"long\":"
                                    "\"stream\",\"help\":\"The name of the stream "
                                    "that this connector will transmit.\"}"));

    // Add basic connector options
    cfg->addBasicConnectorOptions(capa);
  }

  ProcessSource::ProcessSource(Socket::Connection &c, Util::Config & _cfg, JSON::Value & _capa) : Output(c, _cfg, _capa){
    meta.ignorePid(getpid());
    // Initialize source-specific variables
    streamName = opt["source"].asString();

    // Initialize base class parameters
    targetParams["keeptimes"] = true;
    realTime = 0;
    initialize();
    wantRequest = false;
    parseData = true;
    sendFirst = false; // Reset first packet flag
  }

  bool ProcessSource::isRecording() {
    return false;
  }

  bool ProcessSource::onFinish() {
    if (opt.isMember("exit_unmask") && opt["exit_unmask"].asBool()) {
      if (userSelect.size()) {
        for (std::map<size_t, Comms::Users>::iterator it = userSelect.begin(); it != userSelect.end(); it++) {
          INFO_MSG("Main: Unmasking source track %zu" PRIu64, it->first);
          meta.validateTrack(it->first, TRACK_VALID_ALL);
        }
      }
    }
    return Output::onFinish();
  }

  void ProcessSource::dropTrack(size_t trackId, const std::string & reason, bool probablyBad) {
    if (opt.isMember("exit_unmask") && opt["exit_unmask"].asBool()) {
      INFO_MSG("Main: Unmasking source track %zu" PRIu64, trackId);
      meta.validateTrack(trackId, TRACK_VALID_ALL);
    }
    Output::dropTrack(trackId, reason, probablyBad);
  }

  void ProcessSource::sendHeader() {
    if (opt.isMember("source_mask") && opt["source_mask"]) {
      uint32_t mask = opt["source_mask"].asInt();
      for (std::map<size_t, Comms::Users>::iterator ti = userSelect.begin(); ti != userSelect.end(); ++ti) {
        if (ti->first == INVALID_TRACK_ID) { continue; }
        INFO_MSG("Main: Masking source track %zu to %u", ti->first, mask);
        meta.validateTrack(ti->first, mask);
      }
    }
    realTime = 0;
    Output::sendHeader();
  }

  void ProcessSource::connStats(uint64_t now, Comms::Connections & statComm) {
    for (std::map<size_t, Comms::Users>::iterator it = userSelect.begin(); it != userSelect.end(); it++) {
      if (it->second) { it->second.setStatus(COMM_STATUS_DONOTTRACK | it->second.getStatus()); }
    }
  }

  void ProcessSource::sendNext() {
    {
      std::lock_guard<std::mutex> guard(statsMutex);
      if (pData["source_tracks"].size() != userSelect.size()) {
        pData["source_tracks"].null();
        for (std::map<size_t, Comms::Users>::iterator it = userSelect.begin(); it != userSelect.end(); it++) {
          pData["source_tracks"].append((uint64_t)it->first);
        }
      }
      const char *codecIn = pipeline.getCodecIn();
      if (codecIn && pData["source_codec"].asStringRef() != codecIn) { pData["source_codec"] = codecIn; }
    }

    if (thisTime > statSourceMs) { statSourceMs = thisTime; }

    // Keyframe only mode for MJPEG output
    std::string codecOut = Mist::opt["codec"].asString();
    if (codecOut == "JPEG") {
      if (!thisPacket.getFlag("keyframe") ||
          (pipelineConfig.gopSize && lastJPEGSent && pipelineConfig.gopSize > thisTime - lastJPEGSent)) {
        VERYHIGH_MSG("Source: Skipping non-keyframe in keyframe-only mode (skipped=%" PRIu64 ")", thisTime - lastJPEGSent);
        sinkClass->setNowMS(thisTime);
        return;
      }
      HIGH_MSG("Source: Processing keyframe after %" PRIu64 " skipped frames", thisTime - lastJPEGSent);
      lastJPEGSent = thisTime;
    }

    needsLookAhead = 0;
    maxSkipAhead = 0;
    realTime = 0;
    if (!sendFirst) {
      sendPacketTime = thisTime;
      bootMsOffset = M.getBootMsOffset();
      sendFirst = true;
      INFO_MSG("Source: First packet: time=%" PRIu64 ", bootOffset=%" PRIu64 "", thisTime, bootMsOffset);
    }

    // Retrieve packet buffer pointers
    if (!thisData || !thisDataLen) {
      ERROR_MSG("Source: Invalid packet data: ptr=%p, size=%zu", thisData, thisDataLen);
      return;
    }
    VERYHIGH_MSG("Source: Ingest %zu bytes @ %" PRIu64 "ms", thisDataLen, thisTime);

    std::string inCodec = M.getCodec(thisIdx);
    if (inCodec.empty()) {
      ERROR_MSG("Source: Empty codec for track %zu", thisIdx);
      return;
    }

    const std::string & init = M.getInit(thisIdx);

    // Send packet and notify processing thread
    if (pipeline.getIsVideo()) {
      uint64_t inWidth = M.getWidth(thisIdx);
      uint64_t inHeight = M.getHeight(thisIdx);
      if (!inWidth || !inHeight) {
        ERROR_MSG("Source: Invalid dimensions: %" PRIu64 "x%" PRIu64 "", inWidth, inHeight);
        return;
      }
      uint64_t inFpks = M.getFpks(thisIdx);
      if (!inFpks) { inFpks = M.getEfpks(thisIdx); }
      lastInWidth.store(inWidth, std::memory_order_relaxed);
      lastInHeight.store(inHeight, std::memory_order_relaxed);
      lastInFpks.store(inFpks, std::memory_order_relaxed);
      inFrames.fetch_add(1);
      totalInputBytes.fetch_add(thisDataLen);
      pipeline.receiveVideo(sendPacketTime, thisTime, thisData, thisDataLen, inWidth, inHeight, init, inFpks,
                            inCodec.c_str(), thisPacket.getFlag("keyframe"));
    } else {
      uint64_t inDepth = M.getSize(thisIdx);
      uint64_t inChannels = M.getChannels(thisIdx);
      uint64_t inRate = M.getRate(thisIdx);
      if (!inChannels || !inRate) {
        ERROR_MSG("Source: Invalid audio parameters: channels=%" PRIu64 ", rate=%" PRIu64 "", inChannels, inRate);
        return;
      }
      lastSampleRate.store(inRate, std::memory_order_relaxed);
      lastChannels.store(inChannels, std::memory_order_relaxed);
      inFrames.fetch_add(1);
      totalInputBytes.fetch_add(thisDataLen);
      pipeline.receiveAudio(sendPacketTime, thisTime, thisData, thisDataLen, inDepth, inChannels, inRate, init, inCodec.c_str());
    }
  }

  ProcessSource::~ProcessSource() = default;

  /// cconfig pipeline and verify config params
  bool ProcAV::CheckConfig() {
    // Check generic configuration variables
    if (!opt.isMember("source") || !opt["source"] || !opt["source"].isString()) {
      FAIL_MSG("Main: invalid source in config!");
      return false;
    }

    if (!opt.isMember("sink") || !opt["sink"] || !opt["sink"].isString()) {
      INFO_MSG("Main: No sink explicitly set, using source as sink");
    }

    return true;
  }

  void ProcAV::updateStats(uint64_t startTime) {
    static uint64_t lastProcUpdate = Util::bootSecs();
    static uint64_t encPrevTime = 0;

    if (lastProcUpdate + 5 <= Util::bootSecs()) {
      std::lock_guard<std::mutex> guard(statsMutex);

      uint64_t inFr = inFrames;
      uint64_t outFr = outFrames;

      pData["active_seconds"] = (Util::bootSecs() - startTime);
      pData["ainfo"]["sourceTime"] = statSourceMs;
      pData["ainfo"]["sinkTime"] = statSinkMs;
      pData["ainfo"]["inFrames"] = inFr;
      pData["ainfo"]["outFrames"] = outFr;
      pData["ainfo"]["droppedFrames"] = pipeline.getDroppedFrameCount();

      // Calculate per-frame timing averages (convert from microseconds to milliseconds)
      if (inFr) {
        pData["ainfo"]["decodeTime"] = pipeline.getDecodeTime() / inFr / 1000;
        pData["ainfo"]["transformTime"] = pipeline.getTransformTime() / inFr / 1000;
      }

      // Calculate incremental encode time for new frames only
      if (outFr > encPrevCount) {
        uint64_t encTime = pipeline.getEncodeTime();
        pData["ainfo"]["encodeTime"] = (encTime - encPrevTime) / (outFr - encPrevCount) / 1000;
        encPrevTime = encTime;
        encPrevCount = outFr;
      }

      // Update node info using pipeline's getters
      pData["ainfo"]["decoder"] = pipeline.getDecoderName();
      pData["ainfo"]["encoder"] = pipeline.getEncoderName();
      pData["ainfo"]["scaler"] = pipeline.getTransformerName();

      // Sleep telemetry
      {
        uint64_t sinkSleep, sourceSleep;
        pipeline.getSleepTimes(sinkSleep, sourceSleep);
        pData["ainfo"]["sourceSleepTime"] = sourceSleep;
        pData["ainfo"]["sinkSleepTime"] = sinkSleep;
      }

      Util::sendUDPApi(pStat);

      // Write process timing stats to SHM for rate control
      if (procStatsPage.mapped) {
        ProcTimingStats *s = (ProcTimingStats *)procStatsPage.mapped;
        s->totalWork = pipeline.getDecodeTime() + pipeline.getEncodeTime() + pipeline.getTransformTime();
        uint64_t sinkSleep, sourceSleep;
        pipeline.getSleepTimes(sinkSleep, sourceSleep);
        s->totalSourceSleep = sourceSleep;
        s->totalSinkSleep = sinkSleep;
        s->frameCount = outFr;
        s->lastUpdateMs = Util::bootMS();
      }

      // Fire virtual segment trigger
      fireVirtualSegmentTrigger(false);

      lastProcUpdate = Util::bootSecs();
    }
  }
  void fireVirtualSegmentTrigger(bool isFinal) {
    std::string sinkName = pStat["proc_status_update"]["sink"].asString();
    if (!Triggers::shouldTrigger("PROCESS_AV_VIRTUAL_SEGMENT_COMPLETE", sinkName)) { return; }

    uint64_t now = Util::bootSecs();
    uint64_t deltaSecs = lastTriggerTime ? (now - lastTriggerTime) : (now - processStartTime);
    if (!deltaSecs && !isFinal) { return; }

    uint64_t inFr = inFrames;
    uint64_t outFr = outFrames;
    uint64_t outBytes = totalOutputBytes;
    uint64_t inBytes = totalInputBytes;

    uint64_t decodeAvg = inFr ? (pipeline.getDecodeTime() / inFr) : 0;
    uint64_t transformAvg = inFr ? (pipeline.getTransformTime() / inFr) : 0;
    uint64_t encodeAvg = outFr ? (pipeline.getEncodeTime() / outFr) : 0;

    uint64_t windowInFrames = inFr - prevInFrames;
    uint64_t windowOutFrames = outFr - prevOutFrames;
    uint64_t windowInBytes = inBytes - prevInputBytes;
    uint64_t windowOutBytes = outBytes - prevOutputBytes;
    uint64_t sourceAdvancedMs = statSourceMs - prevSourceMs;
    uint64_t sinkAdvancedMs = statSinkMs - prevSinkMs;

    double rtFactorIn = (deltaSecs && sourceAdvancedMs) ? ((double)sourceAdvancedMs / (deltaSecs * 1000.0)) : 0.0;
    double rtFactorOut = (deltaSecs && sinkAdvancedMs) ? ((double)sinkAdvancedMs / (deltaSecs * 1000.0)) : 0.0;
    int64_t lagMs = (int64_t)statSourceMs - (int64_t)statSinkMs;
    uint64_t outBitrate = deltaSecs ? (windowOutBytes * 8 / deltaSecs) : 0;

    const char *codecIn = pipeline.getCodecIn();

    std::string payload =
      sinkName + "\n" +
      std::string(isVideo ? "video" : "audio") + "\n" +
      std::to_string(deltaSecs) + "\n" +
      std::to_string(inFr) + "\n" +
      std::to_string(outFr) + "\n" +
      std::to_string(windowInFrames) + "\n" +
      std::to_string(windowOutFrames) + "\n" +
      std::to_string(windowInBytes) + "\n" +
      std::to_string(windowOutBytes) + "\n" +
      std::to_string(decodeAvg) + "\n" +
      std::to_string(transformAvg) + "\n" +
      std::to_string(encodeAvg) + "\n" +
      std::string(codecIn ? codecIn : "unknown") + "\n" +
      codecOut + "\n" +
      std::to_string(lastInWidth.load(std::memory_order_relaxed)) + "\n" +
      std::to_string(lastInHeight.load(std::memory_order_relaxed)) + "\n" +
      std::to_string(lastOutWidth.load(std::memory_order_relaxed)) + "\n" +
      std::to_string(lastOutHeight.load(std::memory_order_relaxed)) + "\n" +
      std::to_string(lastInFpks.load(std::memory_order_relaxed)) + "\n" +
      std::to_string(deltaSecs ? ((double)windowOutFrames / deltaSecs) : 0.0) + "\n" +
      std::to_string(lastSampleRate.load(std::memory_order_relaxed)) + "\n" +
      std::to_string(lastChannels.load(std::memory_order_relaxed)) + "\n" +
      std::to_string(statSourceMs) + "\n" +
      std::to_string(statSinkMs) + "\n" +
      std::to_string(sourceAdvancedMs) + "\n" +
      std::to_string(sinkAdvancedMs) + "\n" +
      std::to_string(rtFactorIn) + "\n" +
      std::to_string(rtFactorOut) + "\n" +
      std::to_string(lagMs) + "\n" +
      std::to_string(outBitrate) + "\n" +
      (isFinal ? "1" : "0");

    Triggers::doTrigger("PROCESS_AV_VIRTUAL_SEGMENT_COMPLETE", payload, sinkName);

    lastTriggerTime = now;
    prevInFrames = inFr;
    prevOutFrames = outFr;
    prevOutputBytes = outBytes;
    prevInputBytes = inBytes;
    prevSourceMs = statSourceMs;
    prevSinkMs = statSinkMs;
  }
} // namespace Mist

int main(int argc, char *argv[]) {
  DTSC::trackValidMask = TRACK_VALID_INT_PROCESS;
  Util::Config config(argv[0]);
  Util::Config::binaryType = Util::PROCESS;
  JSON::Value capa;

  {
    JSON::Value opt;
    opt["arg"] = "string";
    opt["default"] = "-";
    opt["arg_num"] = 1;
    opt["help"] = "JSON configuration, or - (default) to read from stdin";
    config.addOption("configuration", opt);
    opt.null();
    opt["long"] = "json";
    opt["short"] = "j";
    opt["help"] = "Output connector info in JSON format, then exit.";
    opt["value"].append(0);
    config.addOption("json", opt);
  }

  capa["codecs"][0u][0u].append("YUYV");
  capa["codecs"][0u][0u].append("NV12");
  capa["codecs"][0u][0u].append("UYVY");
  capa["codecs"][0u][0u].append("H264");
  capa["codecs"][0u][0u].append("AV1");
  capa["codecs"][0u][0u].append("HEVC");
  capa["codecs"][0u][0u].append("VP9");
  capa["codecs"][0u][0u].append("JPEG");
  capa["codecs"][0u][0u].append("PCM");
  capa["codecs"][0u][0u].append("opus");
  capa["codecs"][0u][0u].append("AAC");
  capa["codecs"][0u][0u].append("MP3");
  capa["codecs"][0u][0u].append("FLAC");
  capa["codecs"][0u][0u].append("vorbis");

  capa["ainfo"]["sinkTime"]["name"] = "Sink timestamp";
  capa["ainfo"]["sourceTime"]["name"] = "Source timestamp";
  capa["ainfo"]["child_pid"]["name"] = "Child process PID";
  capa["ainfo"]["cmd"]["name"] = "Child process command";
  capa["ainfo"]["inFrames"]["name"] = "Frames ingested";
  capa["ainfo"]["outFrames"]["name"] = "Frames outputted";
  capa["ainfo"]["droppedFrames"]["name"] = "Frames dropped";
  capa["ainfo"]["decodeTime"]["name"] = "Decode time";
  capa["ainfo"]["decodeTime"]["unit"] = "ms / frame";
  capa["ainfo"]["transformTime"]["name"] = "Transform time";
  capa["ainfo"]["transformTime"]["unit"] = "ms / frame";
  capa["ainfo"]["encodeTime"]["name"] = "Encode time";
  capa["ainfo"]["encodeTime"]["unit"] = "ms / frame";
  capa["ainfo"]["decoder"]["name"] = "Decoder";
  capa["ainfo"]["encoder"]["name"] = "Encoder";
  capa["ainfo"]["scaler"]["name"] = "Transformer";
  capa["ainfo"]["sourceSleepTime"]["name"] = "Source sleep time";
  capa["ainfo"]["sourceSleepTime"]["unit"] = "us";
  capa["ainfo"]["sinkSleepTime"]["name"] = "Sink sleep time";
  capa["ainfo"]["sinkSleepTime"]["unit"] = "us";

  if (!(config.parseArgs(argc, argv))) { return 1; }
  if (config.getBool("json")) {
    capa["name"] = "AV";
    capa["hrn"] = "Encoder: libav (ffmpeg library)";
    capa["desc"] = "Generic video encoder that directly integrates with the "
                   "ffmpeg library rather "
                   "than calling a binary and transmuxing twice";
    capa["sort"] = "sort"; // sort the parameters by this key
    addGenericProcessOptions(capa);

    { 
      //////////////////////
      // required options //
      //////////////////////

      JSON::Value &required = capa["required"];

      {
        JSON::Value &o = required["x-LSP-kind"];

        o["name"] = "Input type"; // human readable name of option
        o["help"] = "The type of input to use"; // extra information
        o["type"] = "select";          // type of input field to use
        o["select"][0u][0u] = "video"; // value of first select field
        o["select"][0u][1u] = "Video"; // label of first select field
        o["select"][1u][0u] = "audio";
        o["select"][1u][1u] = "Audio";
        o["sort"] = "aaaa"; // sorting index
        o["influences"].append("codec");
        o["influences"].append("accel");
        o["influences"].append("resolution");
        o["influences"].append("gopsize");
        o["influences"].append("bitrate");
        o["influences"].append("split_audio");
        o["influences"].append("rate_control");
        o["influences"].append("preset");
        o["influences"].append("tune");
        o["influences"].append("max_bitrate");
        o["influences"].append("quality");
        o["value"] = "video"; // preselect this value
      }
      {
        JSON::Value &o = required["codec"][0u];

        o["name"] = "Target codec";
        o["help"] = "Wanted output codec";
        o["type"] = "select";
        o["select"].append("H264");

        JSON::Value entry;
        entry[0u] = "HEVC";
        entry[1u] = "HEVC (H.265)";
        o["select"].append(entry);

        o["select"].append("AV1");
        o["select"].append("VP9");
        o["select"].append("JPEG");

        entry[0u] = "UYVY";
        entry[1u] = "UYVY: Raw YUV 4:2:2 pixels";
        o["select"].append(entry);

        entry[0u] = "YUYV";
        entry[1u] = "YUYV: Raw YUV 4:2:2 pixels";
        o["select"].append(entry);

        o["dependent"]["x-LSP-kind"] = "video"; // this field is only shown if x-LSP-kind is set to "video"
        o["influences"].append("gopsize");
      }
      {
        JSON::Value &o = required["codec"][1u];
        o = required["codec"][0u];
        o.removeMember("select");
        o["select"].append("AAC");
        o["select"].append("opus");
        o["select"].append("PCM");
        o["select"].append("MP3");
        o["select"].append("FLAC");
        o["select"].append("vorbis");
        o["dependent"]["x-LSP-kind"] = "audio"; // this field is only shown if x-LSP-kind is set to "audio"
      }

    }

    {
      //////////////////////
      // optional options //
      //////////////////////

      JSON::Value &optional = capa["optional"];

      //group for options that we consider common
      optional["common"]["name"] = "Commonly used options";
      optional["common"]["type"] = "group";
      optional["common"]["expand"] = true;
      optional["common"]["sort"] = "aaa";
      JSON::Value &common = optional["common"]["options"]; 

      //group for options that we consider advanced
      optional["advanced"]["name"] = "Advanced options";
      optional["advanced"]["type"] = "group";
      optional["advanced"]["expand"] = false;
      optional["advanced"]["sort"] = "xxx";
      JSON::Value &advanced = optional["advanced"]["options"];

      // video options
      {
        JSON::Value &o = common["resolution"];
        o["name"] = "resolution";
        o["help"] = "Resolution of the output stream, e.g. 1920x1080";
        o["type"] = "str";
        o["name"] = "resolution";
        o["help"] = "Resolution of the output stream, e.g. 1920x1080";
        o["type"] = "str";
        o["default"] = "keep source resolution";
        o["dependent"]["x-LSP-kind"] = "video";
        o["sort"] = "b";
      }
      {
        //TODO for backend: when gopsize is 0, add keyframe when the source has a keyframe. Otherwise, obey time interval
        JSON::Value &o = common["gopsize"][0u];
        o["name"] = "Keyframe interval";
        o["help"] = "Amount of time before a new keyframe is sent. Defaults to whenever there is a keyframe in the source.";
        o["type"] = "selectinput";
        o["default"] = 0;

        JSON::Value entry;
        entry[0u] = "0";
        entry[1u] = "Match source";
        o["selectinput"].append(entry);
        
        JSON::Value &i = entry[0u];
        i["name"] = "Interval";
        i["type"] = "uint";
        i["value"] = 2000;
        i["unit"][0u][0u] = "1";
        i["unit"][0u][1u] = "ms";
        i["unit"][1u][0u] = "1000";
        i["unit"][1u][1u] = "s";
        entry[1u] = "Specify";
        o["selectinput"].append(entry);

        o["dependent"]["x-LSP-kind"] = "video";
        o["dependent_not"]["codec"].append("JPEG"); //do not show this field if the codec is JPEG
        o["dependent_not"]["codec"].append("UYVY"); //do not show this field if the codec is raw
        o["dependent_not"]["codec"].append("YUYV"); //do not show this field if the codec is raw
        o["sort"] = "c";
      }
      {
        JSON::Value &o = common["gopsize"][1u];
        o = common["gopsize"][0u];
        o["name"] = "Time between images";
        o["help"] = "Amount of milliseconds between images. Defaults to whenever there is a keyframe in the source.";
        o["dependent_not"]["codec"].shrink(2); //throw out all but the last 2 elements
        o["dependent"]["codec"] = "JPEG"; //*do* show this field if the codec is jpeg
      }
      {
        JSON::Value &o = common["bitrate"][0u];
        o["name"] = "Target bitrate";
        o["help"] = "Set the target bitrate in bits per second. When set to 0, the bitrate will be automatically determined based on the codec.";
        o["type"] = "uint";
        o["default"] = 0;
        o["unit"][0u][0u] = "1";
        o["unit"][0u][1u] = "bit/s";
        o["unit"][1u][0u] = "1000";
        o["unit"][1u][1u] = "kbit/s";
        o["unit"][2u][0u] = "1000000";
        o["unit"][2u][1u] = "Mbit/s";
        o["unit"][3u][0u] = "1000000000";
        o["unit"][3u][1u] = "Gbit/s";
        o["value"] = 7000000;
        o["dependent"]["x-LSP-kind"] = "video";
        o["dependent_not"]["codec"].append("PCM");
        o["dependent_not"]["codec"].append("UYVY");
        o["dependent_not"]["codec"].append("YUYV");
        o["sort"] = "d";
      }

      // audio options
      {
        JSON::Value &o = common["bitrate"][1u];
        o = common["bitrate"][0u];
        o["value"] = 128000;
        o["dependent"]["x-LSP-kind"] = "audio";
        o["sort"] = "a";
      }
      {
        JSON::Value &o = advanced["split_audio"];
        o["name"] = "Split audio channels";
        o["help"] = "List of channels to split the audio into. Defaults to all channels. IE: \"4, 2, 2, 4\"";
        o["type"] = "inputlist";
        o["input"]["type"] = "uint";
        o["input"]["min"] = 1;
        o["default"] = "";
        o["sort"] = "b";
        o["dependent"]["x-LSP-kind"] = "audio";
      }

      // selection options (secretly part of generic options)
      {
        JSON::Value &opts = capa["optional"]["general_process_options"]["options"];

        JSON::Value &g = opts["selection"];
        g["name"] = "Selection";
        g["desc"] = "Choose what tracks to insert into the process, and where process output should go.";
        g["type"] = "group";
        g["expand"] = true;
        g["sort"] = "aaa";
        {
          JSON::Value &opts = g["options"];
          {
            JSON::Value &o = opts["track_select"];
            o["name"] = "Source selector(s)";
            o["help"] = "What tracks to select for the input. Defaults to audio=all&video=all.";
            o["type"] = "string";
            o["validate"][0u] = "track_selector";
            o["default"] = "audio=all&video=all";
            o["sort"] = "a";
          }
          {
            JSON::Value &o = opts["exit_unmask"];
            o["name"] = "Undo masks on process exit/fail";
            o["help"] = "If/when the process exits or fails, the masks for input tracks will be reset to defaults. (NOT to previous value, but to defaults!)";
            o["default"] = false;
            o["sort"] = "b";
          }
          {
            JSON::Value &o = opts["source_mask"];
            o["name"] = "Source track mask";
            o["help"] = "What internal processes should have access to the source track(s)";
            o["type"] = "select";
            o["select"][0u][0u] = 255;
            o["select"][0u][1u] = "Everything";
            o["select"][1u][0u] = 4;
            o["select"][1u][1u] = "Processing tasks (not viewers, not pushes)";
            o["select"][2u][0u] = 6;
            o["select"][2u][1u] = "Processing and pushing tasks (not viewers)";
            o["select"][3u][0u] = 5;
            o["select"][3u][1u] = "Processing and viewer tasks (not pushes)";
            o["default"] = "Keep original value";
            o["sort"] = "c";
          }
          {
            JSON::Value &o = opts["target_mask"];
            o["name"] = "Output track mask";
            o["help"] = "What internal processes should have access to the ouput track(s)";
            o["type"] = "select";
            o["select"][0u][0u] = "";
            o["select"][0u][1u] = "Keep original value";
            o["select"][1u][0u] = 255;
            o["select"][1u][1u] = "Everything";
            o["select"][2u][0u] = 1;
            o["select"][2u][1u] = "Viewer tasks (not processing, not pushes)";
            o["select"][3u][0u] = 2;
            o["select"][3u][1u] = "Pushing tasks (not processing, not viewers)";
            o["select"][4u][0u] = 4;
            o["select"][4u][1u] = "Processing tasks (not pushes, not viewers)";
            o["select"][5u][0u] = 3;
            o["select"][5u][1u] = "Viewer and pushing tasks (not processing)";
            o["select"][6u][0u] = 5;
            o["select"][6u][1u] = "Viewer and processing tasks (not pushes)";
            o["select"][7u][0u] = 6;
            o["select"][7u][1u] = "Pushing and processing tasks (not viewers)";
            o["select"][8u][0u] = 0;
            o["select"][8u][1u] = "Nothing";
            o["default"] = "";
            o["sort"] = "d";
          }
          {
            JSON::Value &o = opts["sink"];
            o["name"] = "Target stream";
            o["help"] = "What stream the encoded track should be added to. Defaults to source stream. May contain variables.";
            o["type"] = "string";
            o["validate"][0u] = "streamname_with_wildcard_and_variables";
            o["sort"] = "e";
          }
        }
      }

      {
        JSON::Value &opts = advanced;
        {
          JSON::Value &o = opts["accel"];
          o["name"] = "Acceleration";
          o["help"] = "Control whether hardware acceleration is used or not.";
          o["type"] = "bitmask";
          JSON::Value entry;
          entry[0u] = 1;
          entry[1u] = "Software";
          o["bitmask"].append(entry);
          entry[0u] = 2;
          entry[1u] = "NVidia";
          o["bitmask"].append(entry);
          entry[0u] = 4;
          entry[1u] = "Intel";
          o["bitmask"].append(entry);
          entry[0u] = 8;
          entry[1u] = "VAAPI";
          o["bitmask"].append(entry);
          entry[0u] = 16;
          entry[1u] = "Videotoolbox";
          o["bitmask"].append(entry);
          o["default"] = 0xff;  // when calculating the field value, we start with this value, then check or uncheck the bits for the displayed fields
          o["value"] = 0xff;    // these fields are checked when configuring a new process
          o["dependent"]["x-LSP-kind"] = "video";
          o["sort"] = "a";
        }
        {
          JSON::Value &o = opts["preset"];
          o["name"] = "Transcode preset";
          o["help"] = "Preset for encoding speed and compression ratio";
          o["type"] = "select";
          o["select"][0u][0u] = "ultrafast";
          o["select"][0u][1u] = "ultrafast";
          o["select"][1u][0u] = "superfast";
          o["select"][1u][1u] = "superfast";
          o["select"][2u][0u] = "veryfast";
          o["select"][2u][1u] = "veryfast";
          o["select"][3u][0u] = "faster";
          o["select"][3u][1u] = "faster";
          o["select"][4u][0u] = "fast";
          o["select"][4u][1u] = "fast";
          o["select"][5u][0u] = "medium";
          o["select"][5u][1u] = "medium";
          o["select"][6u][0u] = "slow";
          o["select"][6u][1u] = "slow";
          o["select"][7u][0u] = "slower";
          o["select"][7u][1u] = "slower";
          o["select"][8u][0u] = "veryslow";
          o["select"][8u][1u] = "veryslow";
          o["default"] = "faster";
          o["sort"] = "a";
          o["dependent"]["x-LSP-kind"] = "video";
        }
        {
          JSON::Value &o = opts["tune"];
          o["name"] = "Encode tuning";
          o["help"] = "Set the encode tuning";
          o["type"] = "select";
          o["select"][0u][0u] = "zerolatency";
          o["select"][0u][1u] = "Low latency";
          o["select"][1u][0u] = "zerolatency-lq";
          o["select"][1u][1u] = "Low latency (high speed / low quality)";
          o["select"][2u][0u] = "zerolatency-hq";
          o["select"][2u][1u] = "Low latency (low speed / high quality)";
          o["select"][3u][0u] = "animation";
          o["select"][3u][1u] = "Cartoon-like content";
          o["select"][4u][0u] = "film";
          o["select"][4u][1u] = "Movie-like content";
          o["select"][5u][0u] = "";
          o["select"][5u][1u] = "No tuning (generic)";
          o["default"] = "zerolatency";
          o["dependent"]["x-LSP-kind"] = "video";
          o["sort"] = "b";
        }
        {
          JSON::Value &o = opts["rate_control"];
          o["name"] = "Rate control";
          o["help"] = "Select the encoder rate control strategy";
          o["type"] = "select";

          JSON::Value entry;
          entry[0u] = "cbr";
          entry[1u] = "Constant bitrate";
          o["select"].append(entry);
          entry[0u] = "vbr";
          entry[1u] = "Variable bitrate";
          o["select"].append(entry);
          entry[0u] = "cq";
          entry[1u] = "Constant quality";
          o["select"].append(entry);

          o["default"] = "cbr";
          o["sort"] = "y";
          o["influences"].append("max_bitrate");
          o["influences"].append("quality");
          o["dependent"]["x-LSP-kind"] = "video";
        }
        {
          JSON::Value &o = opts["max_bitrate"];
          o["name"] = "Maximum bitrate";
          o["help"] = "Optional maximum bitrate cap when operating in bitrate/VBR modes (bits per second).";
          o["type"] = "uint";
          o["default"] = 0;
          o["unit"][0u][0u] = "1";
          o["unit"][0u][1u] = "bit/s";
          o["unit"][1u][0u] = "1000";
          o["unit"][1u][1u] = "kbit/s";
          o["unit"][2u][0u] = "1000000";
          o["unit"][2u][1u] = "Mbit/s";
          o["unit"][3u][0u] = "1000000000";
          o["unit"][3u][1u] = "Gbit/s";
          o["sort"] = "z";
          o["value"] = 7000000;
          o["dependent"]["x-LSP-kind"] = "video";
          o["dependent"]["rate_control"] = "vbr";
        }
        { //TODO for backend: convert this general "quality" to the value required for encoders (cq, vbr, qscale etc)
          JSON::Value &o = opts["quality"];
          o["name"] = "Quality";
          o["help"] = "Select the desired quality";
          o["type"] = "select";

          JSON::Value entry;
          entry[0u] = 1;
          entry[1u] = "very low"; 
          o["select"].append(entry);
          entry[0u] = 6;
          entry[1u] = "low"; 
          o["select"].append(entry);
          entry[0u] = 11;
          entry[1u] = "medium"; 
          o["select"].append(entry);
          entry[0u] = 21;
          entry[1u] = "high"; 
          o["select"].append(entry);
          entry[0u] = 31;
          entry[1u] = "very high"; 
          o["select"].append(entry);

          o["default"] = 21;
          o["dependent"]["x-LSP-kind"] = "video";
          o["dependent"]["rate_control"] = "cq";
          o["sort"] = "z";
        }
      }
    }

    std::cout << capa.toString() << std::endl;
    return -1;
  }

  Util::redirectLogsIfNeeded();

  // Read configuration
  if (config.getString("configuration") != "-") {
    Mist::opt = JSON::fromString(config.getString("configuration"));
  } else {
    std::string json, line;
    INFO_MSG("Main: Reading configuration from standard input");
    while (std::getline(std::cin, line)) { json.append(line); }
    Mist::opt = JSON::fromString(json.c_str());
  }

  // Configure pipeline

  // Set pipeline type based on output codec
  std::string codecOut = Mist::opt["codec"].asString();
  Mist::isVideo = (codecOut == "H264" || codecOut == "AV1" || codecOut == "HEVC" || codecOut == "H265" || codecOut == "VP9" ||
                   codecOut == "JPEG" || codecOut == "YUYV" || codecOut == "UYVY" || codecOut == "NV12");
  pipelineConfig.isVideo = Mist::isVideo;
  pipelineConfig.codecOut = codecOut;

  // Set raw video flag for raw formats
  if (codecOut == "YUYV" || codecOut == "UYVY" || codecOut == "NV12") {
    Mist::isRawVideo = true;
    INFO_MSG("Main: Raw video mode enabled for codec: %s", codecOut.c_str());
  }

  // Set quality
  if (Mist::opt.isMember("quality")) {
    pipelineConfig.quality = Mist::opt["quality"].asInt();
  } else {
    pipelineConfig.quality = 0; // Default quality
  }

  // Set hardware acceleration preferences
  uint8_t hwAccel = 0xFF;
  if (Mist::opt.isMember("accel")) { hwAccel = Mist::opt["accel"].asInt(); }

#if defined(__APPLE__)
  pipelineConfig.allowMediaToolbox = hwAccel & 16;
#else
  pipelineConfig.allowNvidia = hwAccel & 2;
  pipelineConfig.allowQsv = hwAccel & 4;
#endif
  pipelineConfig.allowVaapi = hwAccel & 8;
  pipelineConfig.allowSW = hwAccel & 1;

  if (Mist::opt.isMember("device") && Mist::opt["device"].isString()) {
    std::string requestedDevice = Mist::opt["device"].asString();
    if (!requestedDevice.empty() && requestedDevice != "auto") {
      pipelineConfig.hwDevicePath = requestedDevice;
      INFO_MSG("Main: Using hardware device path %s", pipelineConfig.hwDevicePath.c_str());
    }
  }

  // Set bitrate with codec-specific defaults
  std::string bitrateStr = Mist::opt["bitrate"].asString();
  if (!bitrateStr.empty()) {
    pipelineConfig.bitrate = std::stoul(bitrateStr);
  } else {
    // Set sane default bitrates based on codec type
    if (pipelineConfig.isVideo) {
      // Video codec defaults
      if (codecOut == "H264" || codecOut == "HEVC" || codecOut == "H265") {
        pipelineConfig.bitrate = 2000000; // 2 Mbps for H.264/HEVC
      } else if (codecOut == "AV1") {
        pipelineConfig.bitrate = 1500000; // 1.5 Mbps for AV1 (more efficient)
      } else if (codecOut == "VP9") {
        pipelineConfig.bitrate = 1800000; // 1.8 Mbps for VP9
      } else if (codecOut == "JPEG") {
        pipelineConfig.bitrate = 5000000; // 5 Mbps for MJPEG (higher for quality)
      } else {
        // Raw formats (YUYV, UYVY, NV12) don't use bitrate
        pipelineConfig.bitrate = 0;
      }
    } else {
      // Audio codec defaults
      if (codecOut == "AAC") {
        pipelineConfig.bitrate = 128000; // 128 kbps for AAC
      } else if (codecOut == "opus") {
        pipelineConfig.bitrate = 96000; // 96 kbps for Opus
      } else if (codecOut == "MP3") {
        pipelineConfig.bitrate = 192000; // 192 kbps for MP3
      } else if (codecOut == "FLAC") {
        pipelineConfig.bitrate = 0; // FLAC is lossless, no bitrate limit
      } else if (codecOut == "vorbis") {
        pipelineConfig.bitrate = 160000; // 160 kbps for Vorbis
      } else if (codecOut == "PCM") {
        pipelineConfig.bitrate = 0; // PCM is uncompressed, no bitrate limit
      } else {
        pipelineConfig.bitrate = 128000; // Default audio bitrate
      }
    }

    if (pipelineConfig.bitrate > 0) {
      INFO_MSG("Main: Using default bitrate for %s: %d bps", codecOut.c_str(), pipelineConfig.bitrate);
    }
  }

  auto toLower = [](std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
  };

  std::string rcMode = "cbr";
  if (Mist::opt.isMember("rate_control") && Mist::opt["rate_control"].isString()) {
    std::string rawMode = Mist::opt["rate_control"].asString();
    if (!rawMode.empty()) { rcMode = toLower(rawMode); }
  }
  pipelineConfig.rateControlMode = rcMode;

  if (Mist::opt.isMember("max_bitrate")) {
    std::string maxRateStr = Mist::opt["max_bitrate"].asString();
    if (!maxRateStr.empty()) {
      try {
        pipelineConfig.maxBitrate = std::stoull(maxRateStr);
      } catch (const std::exception &e) {
        WARN_MSG("Main: Invalid max_bitrate value '%s': %s", maxRateStr.c_str(), e.what());
      }
    }
  }

  if (Mist::opt.isMember("vbv_buffer")) {
    std::string vbvStr = Mist::opt["vbv_buffer"].asString();
    if (!vbvStr.empty()) {
      try {
        pipelineConfig.vbvBufferSize = std::stoull(vbvStr);
      } catch (const std::exception &e) {
        WARN_MSG("Main: Invalid vbv_buffer value '%s': %s", vbvStr.c_str(), e.what());
      }
    }
  }

  std::string rcOptionSuffix;
  if (!pipelineConfig.rateControlRcOption.empty()) {
    rcOptionSuffix = " (" + pipelineConfig.rateControlRcOption + ")";
  }
  INFO_MSG("Main: Rate control mode: %s%s", pipelineConfig.rateControlMode.c_str(), rcOptionSuffix.c_str());

  // Set GOP size
  if (Mist::opt.isMember("gopsize")) {
    pipelineConfig.gopSize = Mist::opt["gopsize"].asInt();
  } else {
    pipelineConfig.gopSize = 0;
  }

  // Set tune and preset
  if (Mist::opt.isMember("tune")) {
    pipelineConfig.tune = Mist::opt["tune"].asString();
  } else {
    pipelineConfig.tune = "zerolatency"; // Default to match capabilities
  }
  if (Mist::opt.isMember("preset")) {
    pipelineConfig.preset = Mist::opt["preset"].asString();
  } else {
    pipelineConfig.preset = "faster"; // Default to match capabilities
  }

  // Set resolution for video
  if (pipelineConfig.isVideo && Mist::opt.isMember("resolution")) {
    std::string resolution = Mist::opt["resolution"].asString();
    if (!resolution.empty() && resolution != "keep source resolution") {
      pipelineConfig.resolution = resolution;
      size_t xPos = resolution.find('x');
      if (xPos != std::string::npos) {
        pipelineConfig.targetWidth = std::stoul(resolution.substr(0, xPos));
        pipelineConfig.targetHeight = std::stoul(resolution.substr(xPos + 1));
      }
    }
  }

  // Set audio parameters
  if (!pipelineConfig.isVideo) {

    // Channel configuration
    if (Mist::opt.isMember("split_audio")) {
      std::vector<int> channelCounts;

      if (Mist::opt["split_audio"].isString() && Mist::opt["split_audio"]) {
        if (Mist::opt["split_audio"].asStringRef()[0] == '[') {
          Mist::opt["split_audio"] = JSON::fromString(Mist::opt["split_audio"].asStringRef());
        } else {
          std::string splitAudio = Mist::opt["split_audio"].asStringRef();
          size_t pos = 0;
          while ((pos = splitAudio.find(',')) != std::string::npos) {
            std::string token = splitAudio.substr(0, pos);
            while (token.length() > 0 && std::isspace(token.front())) token.erase(0, 1);
            while (token.length() > 0 && std::isspace(token.back())) token.pop_back();
            channelCounts.push_back(std::stoi(token));
            splitAudio.erase(0, pos + 1);
          }
          channelCounts.push_back(std::stoi(splitAudio));
        }
      }

      if (Mist::opt["split_audio"].isArray()) {
        jsonForEachConst (Mist::opt["split_audio"], it) { channelCounts.push_back(it->asInt()); }
      }

      // Set split channels configuration
      pipelineConfig.splitChannels = channelCounts;

      // Set total output channels
      uint32_t totalChannels = 0;
      for (int count : channelCounts) { totalChannels += count; }
      INFO_MSG("Main: Split audio configuration: %zu groups, total %u channels", channelCounts.size(), totalChannels);
    }
  }

  // Apply configuration to pipeline
  if (!pipeline.configure(pipelineConfig)) {
    FAIL_MSG("Main: Failed to configure pipeline");
    return 1;
  }

  // Create and run ProcAV
  Mist::ProcAV procAV;
  if (!procAV.CheckConfig()) {
    FAIL_MSG("Main: Error config syntax error!");
    return 1;
  }

  Mist::co.is_active = true;
  Mist::conf.is_active = true;

  procAV.Run();

  // Wait for pipeline to finish
  pipeline.waitForInactive();
}

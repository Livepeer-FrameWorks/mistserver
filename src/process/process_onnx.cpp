// MistProcONNX.cpp
#include "process_onnx.h"

#include "../output/output.h"
#include "process.hpp"
#include "process_onnx_audio.h"

#include <mist/defines.h>
#include <mist/onnx.h>
#include <mist/util.h>

#include <atomic>
#include <deque>
#include <mutex>
#include <thread>

// System headers for CPU optimization
#include <sys/resource.h>
#ifdef __linux__
#include <sched.h>
#endif
#ifdef __APPLE__
#include <pthread.h>
#endif
#include <cstring>

Util::Config co;
Util::Config conf;

// Stat related stuff
JSON::Value pStat;
JSON::Value & pData = pStat["proc_status_update"]["status"];
std::mutex statsMutex;
uint64_t statSinkMs = 0;
uint64_t statSourceMs = 0;
int64_t bootMsOffset = 0;
std::atomic<size_t> sourceTrackIdx{INVALID_TRACK_ID};

static std::string packagedONNXProfile() {
#ifdef MIST_ONNX_PROFILE
  return MIST_ONNX_PROFILE;
#else
  return "cpu";
#endif
}

// ONNX processing with new library
std::unique_ptr<ONNX::DetectionModel> onnxModel;
std::unique_ptr<ONNX::DetectionModel> secondaryModel;
ONNX::TemporalTracker tracker;
ONNX::ProcessingStats onnxStats;
bool onnxInitialized = false;
std::mutex onnxInitMutex;

// Modality selected at init from the chosen model. VISION drives the frame pipeline;
// AUDIO drives the transcription pipeline (asrModel + audioWindower) instead. Decided
// once here, not branched per packet.
ONNX::ModelModality activeModality = ONNX::ModelModality::VISION;
std::unique_ptr<ONNX::ASRModel> asrModel;
std::unique_ptr<ONNX::AudioModel> audioModel; // generic single-file audio model (VAD/classification/tagging/embedding)
std::unique_ptr<ONNX::SessionRunner> tensorModel; // modality-neutral ONNXTENSOR input/output mode
ONNX::EventSmoother vadSmoother;              // speech started/ended events for AUDIO_VAD
Mist::AudioWindower audioWindower;

// Scene change detection
ONNX::Utils::SceneChangeDetector sceneChangeDetector;

// Configuration parameters. All options are read from `opt` exactly once, in parseConfig()
// (called at startup before CheckConfig/Run), which validates + clamps + stores here. Every
// other function CONSUMES these globals and never re-parses `opt`. Defaults below double as
// the fallback when an option is absent or invalid.
// -- Detection / preprocessing (vision) --
float confThreshold = 0.50f;
float nmsThreshold = 0.40f;
float softNmsSigma = 0.5f;
int inputSize = 640;
bool enhanceImage = false;
bool annotatedVideo = false;        // metadata-only by default; drawing/JPEG is opt-in
int jpegQuality = 80;               // MJPEG output quality [1,100]
int processEveryNth = 1;
double maxInferenceFps = 5.0;        // timestamp-based rate cap; 0 means unlimited
int letterboxOpt = -1;              // tri-state: -1 unset, 0 off, 1 on
std::string normalizationMode;      // "" = model default; imagenet/scrfd/scale01
std::string resizeMode;             // "" = model default; letterbox/direct
std::string secondaryModelPath;     // "" = none
// -- Runtime / performance (both modalities) --
int numThreads = 1;                 // intra-op threads [1,32]
std::string epChoice;               // execution provider: "" = auto, cpu/cuda/tensorrt/coreml/openvino
bool lowLatency = false;            // intra-op hot-spinning (one busy core/session)
bool realtimePriority = false;      // elevated scheduling is dangerous on shared hosts
std::string outputId = "default";   // stable identity for resumable output tracks
std::string inputMode = "auto";      // auto (curated adapter) or tensor (raw ONNXTENSOR)
const char *const defaultVisionTrackSelector =
  "video=YUYV,UYVY,NV12,I420,JPEG&audio=none";
// -- Transcription / audio chunking --
double windowTargetSec = 5.0;       // preferred chunk length [1,120]
double windowMaxSec = 10.0;         // hard cut ceiling
double windowMinSec = 1.5;          // shortest pause-cut chunk
double maxBufferSec = 30.0;         // backpressure ceiling (drop-oldest beyond this)
double eventEnter = 0.5;            // event enter threshold (VAD speech / vision event_class)
double eventExit = 0.35;            // event exit threshold (< enter = hysteresis)
double eventEma = 0.3;              // EMA weight of each NEW score (1 = no smoothing)
uint64_t eventMinMs = 250;          // minimum active duration before an event may end
std::string eventClass;             // vision: classification class to eventize ("" = off)
unsigned classifierTopK = 5;        // ranked classes in classification metadata ("top" array)
std::string classifierMode;         // classifier output mode: "" / "softmax" | "sigmoid" | "raw"
std::string secondaryModelType;     // secondary model type override ("" = registry/auto)
bool emitEmbeddings = false;        // include full embedding vectors in per-detection metadata
std::string genderHighLabel = "female"; // age-gender: label when gender_prob >= 0.5 (UTKFace default)
std::string genderLowLabel = "male";    // ... and when < 0.5
ONNX::EventSmoother visionSmoother; // nsfw/violence started/ended events on classification
// -- Temporal tracking / scene-change / Kalman (vision) --
int trackMinConsecutiveMs = 200;
int trackMaxMissingMs = 800;
float trackingIoU = 0.3f;
bool trackingEnabled = false;
bool sceneChangeEnabled = false;
float sceneChangeThreshold = 0.85f;
bool kalmanEnabled = false;
float kalmanProcessNoise = 0.01f;
float kalmanMeasurementNoise = 0.1f;

// Processing pipeline - video packets from ProcessSource to processing thread
std::mutex latestVideoMutex;
ONNX::VideoPacket latestVideo;
bool hasLatestVideo = false;

struct TensorPacket {
  std::vector<uint8_t> data;
  uint64_t timestamp = 0;
};
std::mutex tensorInputMutex;
std::deque<TensorPacket> tensorInputQueue;
std::mutex tensorOutputMutex;
std::deque<TensorPacket> tensorOutputQueue;
size_t maxTensorQueueDepth = 8;
std::atomic<uint64_t> tensorRuns{0};
std::atomic<uint64_t> tensorErrors{0};
std::atomic<uint64_t> tensorInputDrops{0};
std::atomic<uint64_t> tensorOutputDrops{0};
std::atomic<size_t> tensorInputDepth{0};
std::atomic<size_t> tensorOutputDepth{0};

// Metadata pipeline - processed results from processing thread to ProcessSink.
// Video detections use a latest-only slot (dropping stale frames is fine). Transcription
// windows are ordered and must NOT be dropped, so audio uses an explicit FIFO queue.
std::mutex latestMetadataMutex;
JSON::Value latestMetadata;
bool hasLatestMetadata = false;

std::mutex transcriptMutex;
std::deque<JSON::Value> transcriptQueue;
// Set true by the audio process thread once it has produced its final (tail) transcript,
// so the sink drain knows the queue won't grow anymore and can stop deterministically.
std::atomic<bool> audioProcessingDone{false};

// ASR observability, accumulated by the audio process thread and reported by ProcONNX::Run.
std::atomic<uint64_t> asrChunks{0};       // chunks transcribed
std::atomic<uint64_t> asrAudioMs{0};      // total audio duration fed to the model
std::atomic<uint64_t> asrInferMs{0};      // total wall time spent in transcribe()
std::atomic<uint64_t> visionReceivedFrames{0};
std::atomic<uint64_t> visionDroppedFrames{0};
std::atomic<uint64_t> visionRateSkippedFrames{0};
std::atomic<uint64_t> visionInferenceFrames{0};
std::atomic<uint64_t> visionProcessingMs{0};

// Processed video pipeline - frames with bounding boxes from processing thread to ProcessSink
std::mutex latestProcessedVideoMutex;
ONNX::ProcessedVideoFrame latestProcessedVideo;
bool hasLatestProcessedVideo = false;

std::atomic<bool> isActive{false};

JSON::Value opt; /// Options

namespace Mist {
  ProcessSource::ProcessSource(Socket::Connection & c, Util::Config & cfg, JSON::Value & _capa) : Output(c, cfg, _capa) {
    meta.ignorePid(getpid());
    closeMyConn();
    capa["name"] = "ONNXSource";
    targetParams["keeptimes"] = true;
    realTime = 0;
    initialize();
    wantRequest = false;
    parseData = true;
  }

  void ProcessSource::init(Util::Config *cfg, JSON::Value & capa) {
    Output::init(cfg, capa);
    capa["name"] = "ONNX";
    capa["codecs"][0u][0u].append("YUYV");
    capa["codecs"][0u][0u].append("UYVY");
    capa["codecs"][0u][0u].append("NV12");
    capa["codecs"][0u][0u].append("I420");
    capa["codecs"][0u][0u].append("JPEG");
    // Raw PCM audio, for transcription models (supplied upstream by MistProcAV).
    capa["codecs"][1u][0u].append("PCM");
    // Modality-neutral binary tensor packets for arbitrary ONNX graphs.
    capa["codecs"][2u][0u].append("ONNXTENSOR");
    cfg->addOption("streamname",
                   JSON::fromString("{\"arg\":\"string\",\"short\":\"s\",\"long\":"
                                    "\"stream\",\"help\":\"The name of the stream "
                                    "that this connector will transmit.\"}"));
    cfg->addBasicConnectorOptions(capa);
  }

  bool ProcessSource::onFinish() {
    if (opt.isMember("exit_unmask") && opt["exit_unmask"].asBool()) {
      if (userSelect.size()) {
        for (std::map<size_t, Comms::Users>::iterator it = userSelect.begin(); it != userSelect.end(); it++) {
          INFO_MSG("Unmasking source track %zu", it->first);
          meta.validateTrack(it->first, TRACK_VALID_ALL);
        }
      }
    }
    return Output::onFinish();
  }

  void ProcessSource::dropTrack(size_t trackId, const std::string & reason, bool probablyBad) {
    if (opt.isMember("exit_unmask") && opt["exit_unmask"].asBool()) {
      INFO_MSG("Unmasking source track %zu", trackId);
      meta.validateTrack(trackId, TRACK_VALID_ALL);
    }
    Output::dropTrack(trackId, reason, probablyBad);
  }

  void ProcessSource::sendHeader() {
    if (opt["source_mask"].asBool()) {
      for (std::map<size_t, Comms::Users>::iterator ti = userSelect.begin(); ti != userSelect.end(); ++ti) {
        if (ti->first == INVALID_TRACK_ID) { continue; }
        INFO_MSG("Masking source track %zu", ti->first);
        meta.validateTrack(ti->first, meta.trackValid(ti->first) & ~(TRACK_VALID_EXT_HUMAN | TRACK_VALID_EXT_PUSH));
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
    }
    static uint64_t packetCount = 0;
    static uint64_t lastPacketTime = 0;
    static uint64_t sendNextCallCount = 0;
    static uint64_t lastSendNextTime = 0;

    sendNextCallCount++;
    uint64_t sendNextStartTime = Util::bootMS();

    // Measure how often sendNext is called
    if (lastSendNextTime > 0) {
      uint64_t sendNextInterval = sendNextStartTime - lastSendNextTime;
      if (sendNextInterval > 1000) {
        WARN_MSG("SOURCE THREAD: sendNext() not called for %" PRIu64 "ms! Input stream may be stalled!", (uint64_t)sendNextInterval);
      }
    }
    lastSendNextTime = sendNextStartTime;

    if (activeModality == ONNX::ModelModality::TENSOR) {
      if (thisPacket && meta.getType(thisIdx) == "meta" && meta.getCodec(thisIdx) == "ONNXTENSOR" &&
          thisData && thisDataLen > 0) {
        sourceTrackIdx = thisIdx;
        TensorPacket packet;
        packet.data.assign((const uint8_t *)thisData, (const uint8_t *)thisData + thisDataLen);
        packet.timestamp = thisPacket.getTime();
        {
          std::lock_guard<std::mutex> lock(tensorInputMutex);
          if (tensorInputQueue.size() >= maxTensorQueueDepth) {
            tensorInputQueue.pop_front();
            tensorInputDrops++;
          }
          tensorInputQueue.push_back(std::move(packet));
          tensorInputDepth = tensorInputQueue.size();
        }
        if (thisTime > statSourceMs) { statSourceMs = thisTime; }
      }
      needsLookAhead = 0;
      maxSkipAhead = 0;
      return;
    }

    // Audio (transcription) modality: accumulate raw PCM into the windower instead of
    // the drop-old video slot. All decode/resample happened upstream in MistProcAV.
    if (activeModality == ONNX::ModelModality::AUDIO) {
      static uint64_t audioFirstMs = 0;
      static uint64_t audioFed = 0;
      static bool audioNoInputWarned = false;
      static bool audioRateChecked = false;
      static bool audioRateBad = false;
      uint64_t nowMs = Util::bootMS();
      if (!audioFirstMs) { audioFirstMs = nowMs; }
      const int wantRate = asrModel ? asrModel->sampleRate()
                                    : (audioModel ? audioModel->sampleRate() : 16000);

      if (thisPacket && meta.getType(thisIdx) == "audio" && meta.getCodec(thisIdx) == "PCM") {
        sourceTrackIdx = thisIdx;
        if (thisData && thisDataLen > 0) {
          // One-shot: verify the PCM rate matches what the model expects. This binary has
          // no resampler (by design — MistProcAV does DSP), so a wrong rate would produce a
          // plausible-but-wrong transcript. Fail closed: refuse to feed mis-rated audio.
          if (!audioRateChecked) {
            audioRateChecked = true;
            uint32_t rate = M.getRate(thisIdx);
            uint16_t depth = meta.getSize(thisIdx);
            uint16_t chans = meta.getChannels(thisIdx);
            // Fail closed on anything the reader can't handle: this binary has no resampler
            // (MistProcAV does DSP), and the PCM reader only supports 8/16/24/32-bit depths.
            if ((int)rate != wantRate) {
              audioRateBad = true;
              FAIL_MSG("PCM audio is %" PRIu32 " Hz but the transcription model needs %d Hz. Refusing to "
                       "transcribe mis-rated audio — set MistProcAV sample_rate=%d.", rate, wantRate, wantRate);
            } else if (depth != 8 && depth != 16 && depth != 24 && depth != 32) {
              audioRateBad = true;
              FAIL_MSG("PCM audio has unsupported bit depth %" PRIu16 " (need 8/16/24/32). Refusing to transcribe.", depth);
            } else if (chans < 1) {
              audioRateBad = true;
              FAIL_MSG("PCM audio reports %" PRIu16 " channels. Refusing to transcribe.", chans);
            } else {
              INFO_MSG("Transcription input OK: PCM %" PRIu32 " Hz, %" PRIu16 "-bit, %" PRIu16 " channel(s)",
                       rate, depth, chans);
            }
          }
          if (!audioRateBad) {
            if (!sendFirst) {
              sendPacketTime = thisTime;
              bootMsOffset = M.getBootMsOffset();
              sendFirst = true;
            }
            needsLookAhead = 0;
            maxSkipAhead = 0;
            realTime = 0;
            audioWindower.feed(thisData, thisDataLen, (int)meta.getSize(thisIdx),
                               (int)meta.getChannels(thisIdx), thisPacket.getTime());
            audioFed++;
          }
        }
      }

      // Watchdog: if no PCM has arrived a few seconds in, explain exactly what's missing
      // (no audio at all / audio present but not PCM / PCM track but no packets) instead
      // of sitting silently idle. Time-based so a late-joining chain isn't false-flagged.
      if (!audioNoInputWarned && audioFed == 0 && nowMs - audioFirstMs > 5000) {
        audioNoInputWarned = true;
        bool anyAudio = false, anyPCM = false;
        std::string codecs;
        for (size_t tid : M.getValidTracks()) {
          if (M.getType(tid) != "audio") { continue; }
          anyAudio = true;
          if (!codecs.empty()) { codecs += ", "; }
          codecs += M.getCodec(tid);
          if (M.getCodec(tid) == "PCM") { anyPCM = true; }
        }
        if (!anyAudio) {
          FAIL_MSG("Transcription model selected but the source stream has no audio track. Feed audio and "
                   "decode it to PCM via MistProcAV (Input type=audio, codec=PCM, sample_rate=%d).", wantRate);
        } else if (!anyPCM) {
          FAIL_MSG("Transcription needs a raw PCM audio track; the source only has: %s. Chain MistProcAV in "
                   "front (Input type=audio, codec=PCM, sample_rate=%d) to produce one.", codecs.c_str(), wantRate);
        } else {
          WARN_MSG("A PCM audio track exists but no packets have arrived after 5s — check the upstream "
                   "MistProcAV chain is running and producing data.");
        }
      }

      if (thisTime > statSourceMs) { statSourceMs = thisTime; }
      needsLookAhead = 0;
      maxSkipAhead = 0;
      return;
    }

    // Override sendNext to intercept packets and feed them to processing thread
    if (thisPacket && meta.getType(thisIdx) == "video") {
      packetCount++;
      uint64_t currentPacketTime = Util::bootMS();

      // Measure source packet timing
      if (lastPacketTime > 0) {
        uint64_t packetInterval = currentPacketTime - lastPacketTime;
        if (packetInterval > 1000) {
          WARN_MSG("SOURCE THREAD STUTTER: %" PRIu64 "ms between video packets! Source is starving processing thread!",
                   (uint64_t)packetInterval);
        } else if (packetInterval > 500) {
          WARN_MSG("Source delay: %" PRIu64 "ms between video packets", (uint64_t)packetInterval);
        } else if (packetInterval > 100) {
          INFO_MSG("Minor source delay: %" PRIu64 "ms between video packets", (uint64_t)packetInterval);
        }
      }
      lastPacketTime = currentPacketTime;

      std::string codec = meta.getCodec(thisIdx);
      uint64_t width = meta.getWidth(thisIdx);
      uint64_t height = meta.getHeight(thisIdx);

      if (width > 0 && height > 0) {
        sourceTrackIdx = thisIdx;
        ONNX::VideoPacket vp;

        if (thisData && thisDataLen > 0) {
          vp.packetData.assign(thisData, thisDataLen);
          vp.timestamp = thisPacket.getTime();
          vp.trackIdx = thisIdx;
          vp.codec = codec;
          vp.width = width;
          vp.height = height;

          needsLookAhead = 0;
          maxSkipAhead = 0;
          realTime = 0;
          if (!sendFirst) {
            sendPacketTime = thisTime;
            bootMsOffset = M.getBootMsOffset();
            sendFirst = true;
          }

          // Store as latest video packet for processing thread
          {
            std::lock_guard<std::mutex> lock(latestVideoMutex);
            visionReceivedFrames++;
            if (hasLatestVideo) {
              uint64_t droppedFrames = ++visionDroppedFrames;
              if (droppedFrames % 100 == 1) {
                WARN_MSG("Dropped %" PRIu64 " video frames (processing too slow)", droppedFrames);
              }
            }
            latestVideo = vp;
            hasLatestVideo = true;
          }

          VERYHIGH_MSG("ProcessSource fed video packet #%" PRIu64 ": %s %" PRIu64 "x%" PRIu64 " at time %" PRIu64
                       " (%zu bytes)",
                       (uint64_t)packetCount, codec.c_str(), (uint64_t)width, (uint64_t)height, (uint64_t)vp.timestamp, (size_t)thisDataLen);
        } else {
          WARN_MSG("ProcessSource got video packet with no data");
        }
      } else {
        WARN_MSG("ProcessSource got video packet with invalid dimensions: %" PRIu64 "x%" PRIu64, (uint64_t)width, (uint64_t)height);
      }
    } else if (thisPacket) {
      static uint64_t nonVideoCount = 0;
      nonVideoCount++;
      if (nonVideoCount % 100 == 1) {
        INFO_MSG("ProcessSource got non-video packet #%" PRIu64 ": track %zu type %s", (uint64_t)nonVideoCount, thisIdx,
                 meta.getType(thisIdx).c_str());
      }
    } else {
      static uint64_t nullPacketCount = 0;
      nullPacketCount++;
      uint64_t nullPacketTime = Util::bootMS();
      if (nullPacketCount % 100 == 1) {
        WARN_MSG("ProcessSource got null packet #%" PRIu64 " at time %" PRIu64 " - input stream may be stalled!",
                 (uint64_t)nullPacketCount, (uint64_t)nullPacketTime);
      }
    }

    // Update stats
    if (thisTime > statSourceMs) { statSourceMs = thisTime; }
    needsLookAhead = 0;
    maxSkipAhead = 0;

    // Periodic diagnostics every 1000 sendNext calls
    if (sendNextCallCount % 1000 == 0) {
      INFO_MSG("SOURCE DIAGNOSTICS: sendNext called %" PRIu64 " times, got %" PRIu64 " video packets (%.1f%% "
               "video rate)",
               (uint64_t)sendNextCallCount, (uint64_t)packetCount,
               (packetCount * 100.0) / (sendNextCallCount ? sendNextCallCount : 1));
    }
  }

  // End of ProcessSource class

  class ProcessSink : public Input {
    private:
      size_t metadataTrackIdx;
      std::string metadataTrackName;
      size_t videoTrackIdx;
      std::string videoTrackName;

      // Dynamic FPS detection
      uint64_t lastVideoTimestamp = 0;
      std::vector<uint64_t> frameDurations; // Rolling window of frame durations
      uint64_t estimatedFpks = 25000; // Default 25 FPS (fpks = fps * 1000)
      uint64_t lastFpsUpdateTime = 0;
      static const size_t FPS_WINDOW_SIZE = 150; // 6 seconds at 25 FPS

      // Create the JSON metadata output track lazily (on first actual output) and resume an
      // existing unclaimed onnx track rather than adding a new one — matches the proc
      // standard (process_av setAudioInit/setVideoInit via meta.addOrResumeTrack). This is
      // what stops empty tracks piling up across process restarts.
      void ensureMetaTrack() {
        if (metadataTrackIdx != INVALID_TRACK_ID) { return; }
        DTSC::TrackMetadata trkDta{};
        trkDta.type = "meta";
        trkDta.codec = activeModality == ONNX::ModelModality::TENSOR ? "ONNXTENSOR" : "JSON";
        JSON::Value identity;
        identity["schema"] = "mist.onnx.track/v1";
        identity["role"] = activeModality == ONNX::ModelModality::TENSOR ? "tensors" : "results";
        identity["output_id"] = outputId;
        identity["model"] = opt["model"].asString();
        trkDta.init = identity.toString();
        metadataTrackIdx = meta.addOrResumeTrack(trkDta);
        if (metadataTrackIdx == INVALID_TRACK_ID) { FAIL_MSG("ProcessSink: could not add metadata track"); return; }
        meta.setID(metadataTrackIdx, metadataTrackIdx);
        if (sourceTrackIdx != INVALID_TRACK_ID && streamName == opt["source"].asString()) {
          meta.setSourceTrack(metadataTrackIdx, sourceTrackIdx);
        }
        INFO_MSG("ProcessSink metadata track %zu ('%s')", metadataTrackIdx, metadataTrackName.c_str());
      }

      // Same for the MJPEG video track (vision only), created on the first frame so its
      // dimensions are known and it can resume an existing matching track.
      void ensureVideoTrack(uint64_t w, uint64_t h) {
        if (videoTrackIdx != INVALID_TRACK_ID) { return; }
        DTSC::TrackMetadata trkDta{};
        trkDta.type = "video";
        trkDta.codec = "JPEG";
        JSON::Value identity;
        identity["schema"] = "mist.onnx.track/v1";
        identity["role"] = "annotations";
        identity["output_id"] = outputId;
        identity["model"] = opt["model"].asString();
        trkDta.init = identity.toString();
        trkDta.width = w;
        trkDta.height = h;
        trkDta.fpks = estimatedFpks;
        videoTrackIdx = meta.addOrResumeTrack(trkDta);
        if (videoTrackIdx == INVALID_TRACK_ID) { FAIL_MSG("ProcessSink: could not add video track"); return; }
        meta.setID(videoTrackIdx, videoTrackIdx);
        if (sourceTrackIdx != INVALID_TRACK_ID && streamName == opt["source"].asString()) {
          meta.setSourceTrack(videoTrackIdx, sourceTrackIdx);
        }
        INFO_MSG("ProcessSink video track %zu ('%s') %" PRIu64 "x%" PRIu64, videoTrackIdx, videoTrackName.c_str(), w, h);
      }

    public:
      ProcessSink(Util::Config *cfg) : Input(cfg) {
        capa["name"] = "ONNXSink";
        streamName = opt["sink"].asString();
        if (!streamName.size()) { streamName = opt["source"].asString(); }
        Util::streamVariables(streamName, opt["source"].asString());
        {
          std::lock_guard<std::mutex> guard(statsMutex);
          pStat["proc_status_update"]["sink"] = streamName;
          pStat["proc_status_update"]["source"] = opt["source"];
        }
        metadataTrackIdx = INVALID_TRACK_ID;
        metadataTrackName = "onnx_ai";
        if (opt.isMember("trackname") && !opt["trackname"].asString().empty()) {
          metadataTrackName = opt["trackname"].asString();
        }

        videoTrackIdx = INVALID_TRACK_ID;
        videoTrackName = "onnx_video";
        if (opt.isMember("video_trackname") && !opt["video_trackname"].asString().empty()) {
          videoTrackName = opt["video_trackname"].asString();
        }

        Util::setStreamName(opt["source"].asString() + "→" + streamName);
        if (opt.isMember("target_mask") && !opt["target_mask"].isNull() && opt["target_mask"].asString() != "") {
          DTSC::trackValidDefault = opt["target_mask"].asInt();
        }
      }

      bool checkArguments() { return true; }
      bool needHeader() { return false; }
      bool readHeader() { return true; }
      bool openStreamSource() { return true; }
      void parseStreamHeader() {}
      bool needsLock() { return false; }
      bool isSingular() { return false; }
      virtual bool publishesTracks() { return false; }
      void connStats(Comms::Connections & statComm) {}

      void streamMainLoop() {
        uint64_t statTimer = 0;
        Comms::Connections statComm;
        uint64_t metadataCount = 0;
        uint64_t videoFrameCount = 0;

        // Output tracks are created lazily, on first actual output (see ensureMetaTrack /
        // ensureVideoTrack) — not eagerly here, so a restarted instance that produces
        // nothing does not leak an empty track.

        // Main loop - wait for metadata and video frames from processing thread and ingest them
        while (config->is_active && isActive) {
          JSON::Value metadata;
          bool hasMetadata = false;
          ONNX::ProcessedVideoFrame videoFrame;
          bool hasVideoFrame = false;
          TensorPacket tensorOutput;
          bool hasTensorOutput = false;

          if (activeModality == ONNX::ModelModality::TENSOR) {
            std::lock_guard<std::mutex> lock(tensorOutputMutex);
            if (!tensorOutputQueue.empty()) {
              tensorOutput = std::move(tensorOutputQueue.front());
              tensorOutputQueue.pop_front();
              tensorOutputDepth = tensorOutputQueue.size();
              hasTensorOutput = true;
            }
          }

          // Check for metadata from processing thread. The ordered queue is drained
          // first for BOTH modalities (audio results, and vision event transitions
          // that must never be dropped); vision then falls back to the latest-only
          // detection slot.
          {
            std::lock_guard<std::mutex> lock(transcriptMutex);
            if (!transcriptQueue.empty()) {
              metadata = transcriptQueue.front();
              transcriptQueue.pop_front();
              hasMetadata = true;
            }
          }
          if (!hasMetadata && activeModality != ONNX::ModelModality::AUDIO) {
            std::lock_guard<std::mutex> lock(latestMetadataMutex);
            if (hasLatestMetadata) {
              metadata = latestMetadata;
              hasLatestMetadata = false;
              hasMetadata = true;
            }
          }

          // Check for video frame from processing thread
          {
            std::lock_guard<std::mutex> lock(latestProcessedVideoMutex);
            if (hasLatestProcessedVideo) {
              videoFrame = latestProcessedVideo;
              hasLatestProcessedVideo = false;
              hasVideoFrame = true;
            }
          }

          if (hasMetadata) {
            ensureMetaTrack();
            if (metadataTrackIdx == INVALID_TRACK_ID) { continue; }
            metadataCount++;
            uint64_t timestamp = metadata["timestamp_ms"].asInt();

            VERYHIGH_MSG("ProcessSink ingesting metadata #%" PRIu64 " at timestamp %" PRIu64 " with %d detections",
                         (uint64_t)metadataCount, (uint64_t)timestamp, metadata["detections"].size());

            // Create and buffer metadata packet into MistServer
            JSON::Value thisPack;
            thisPack.null();
            thisPack["trackid"] = metadataTrackIdx;
            thisPack["data"] = metadata.toString();
            thisPack["time"] = timestamp;
            thisPack["duration"] = 0;

            std::string tmpStr = thisPack.toNetPacked();
            thisPacket.reInit(tmpStr.data(), tmpStr.size());
            thisIdx = metadataTrackIdx;

            // Ingest into MistServer
            bufferLivePacket(thisPacket);
            if (timestamp > statSinkMs) { statSinkMs = timestamp; }
          }

          if (hasVideoFrame) {
            ensureVideoTrack(videoFrame.width, videoFrame.height);
            if (videoTrackIdx == INVALID_TRACK_ID) { continue; }
            videoFrameCount++;

            // Dynamic FPS detection
            uint64_t currentTimestamp = videoFrame.timestamp;
            uint64_t frameDuration = 1000000 / estimatedFpks; // Default duration in ms

            if (lastVideoTimestamp > 0 && currentTimestamp > lastVideoTimestamp) {
              uint64_t actualDuration = currentTimestamp - lastVideoTimestamp;

              // Only use reasonable durations (10-200ms = 5-100 FPS)
              if (actualDuration >= 10 && actualDuration <= 200) {
                frameDurations.push_back(actualDuration);

                // Keep rolling window
                if (frameDurations.size() > FPS_WINDOW_SIZE) { frameDurations.erase(frameDurations.begin()); }

                // Update FPS estimate every 5 seconds
                uint64_t currentTime = Util::bootSecs();
                if (currentTime - lastFpsUpdateTime >= 5 && frameDurations.size() >= 30) {
                  // Calculate median duration for stability
                  std::vector<uint64_t> sortedDurations = frameDurations;
                  std::sort(sortedDurations.begin(), sortedDurations.end());
                  uint64_t medianDuration = sortedDurations[sortedDurations.size() / 2];

                  uint64_t newFpks = 1000000 / medianDuration; // ms duration to fpks: 1000 * (1000/ms)
                  if (newFpks != estimatedFpks) {
                    estimatedFpks = newFpks;
                    meta.setFpks(videoTrackIdx, estimatedFpks);
                    double fps = estimatedFpks / 1000.0;
                    INFO_MSG("Updated video track FPS: %.2f (fpks: %" PRIu64 ", median duration: %" PRIu64 "ms)", fps, estimatedFpks, medianDuration);
                  }
                  lastFpsUpdateTime = currentTime;
                }

                frameDuration = actualDuration; // Use actual duration for this frame
              }
            }
            lastVideoTimestamp = currentTimestamp;

            VERYHIGH_MSG("ProcessSink ingesting video frame #%" PRIu64 " at timestamp %" PRIu64
                         " (%d detections, %zu bytes, duration: %" PRIu64 "ms)",
                         (uint64_t)videoFrameCount, (uint64_t)videoFrame.timestamp, videoFrame.detectionCount,
                         videoFrame.jpegData.size(), (uint64_t)frameDuration);

            // Create and buffer video packet into MistServer
            JSON::Value thisPack;
            thisPack.null();
            thisPack["trackid"] = videoTrackIdx;
            thisPack["data"] = std::string((const char *)videoFrame.jpegData, videoFrame.jpegData.size());
            thisPack["time"] = videoFrame.timestamp;
            thisPack["duration"] = frameDuration; // Use actual calculated duration
            thisPack["keyframe"] = 1; // JPEG frames are always keyframes

            std::string tmpStr = thisPack.toNetPacked();
            thisPacket.reInit(tmpStr.data(), tmpStr.size());
            thisIdx = videoTrackIdx;

            // Ingest into MistServer
            bufferLivePacket(thisPacket);
            if (videoFrame.timestamp > statSinkMs) { statSinkMs = videoFrame.timestamp; }

            VERYHIGH_MSG("Buffered video frame into MistServer: %zu bytes", tmpStr.size());
          }

          if (hasTensorOutput) {
            ensureMetaTrack();
            if (metadataTrackIdx != INVALID_TRACK_ID) {
              JSON::Value thisPack;
              thisPack["trackid"] = metadataTrackIdx;
              thisPack["data"] = std::string((const char *)tensorOutput.data.data(), tensorOutput.data.size());
              thisPack["time"] = tensorOutput.timestamp;
              thisPack["duration"] = 0;
              std::string tmpStr = thisPack.toNetPacked();
              thisPacket.reInit(tmpStr.data(), tmpStr.size());
              thisIdx = metadataTrackIdx;
              bufferLivePacket(thisPacket);
              if (tensorOutput.timestamp > statSinkMs) { statSinkMs = tensorOutput.timestamp; }
            }
          }

          if (!hasMetadata && !hasVideoFrame && !hasTensorOutput) {
            // No data available, sleep briefly
            Util::sleep(100);
          }

          // Statistics
          if (Util::bootSecs() - statTimer > 1) {
            {
              std::lock_guard<std::mutex> guard(statsMutex);
              // Only report tracks that actually exist yet (created lazily on first output).
              size_t wantTracks = (metadataTrackIdx != INVALID_TRACK_ID ? 1 : 0) +
                                  (videoTrackIdx != INVALID_TRACK_ID ? 1 : 0);
              if (pData["sink_tracks"].size() != wantTracks) {
                pData["sink_tracks"].null();
                if (metadataTrackIdx != INVALID_TRACK_ID) { pData["sink_tracks"].append((uint64_t)metadataTrackIdx); }
                if (videoTrackIdx != INVALID_TRACK_ID) { pData["sink_tracks"].append((uint64_t)videoTrackIdx); }
              }
            }
            statTimer = Util::bootSecs();
          }
        }

        // Shutdown drain: deliver every transcript still queued, including the process
        // thread's tail flush. We wait for the producer to signal it's done (so a slow tail
        // ASR inference isn't cut off) AND the queue to empty. A generous hard cap keeps
        // shutdown bounded if the producer never signals.
        if (activeModality == ONNX::ModelModality::TENSOR) {
          pData["tensor"]["runs"] = tensorRuns.load();
          pData["tensor"]["errors"] = tensorErrors.load();
          pData["tensor"]["input_drops"] = tensorInputDrops.load();
          pData["tensor"]["output_drops"] = tensorOutputDrops.load();
          pData["tensor"]["input_queue_depth"] = (uint64_t)tensorInputDepth.load();
          pData["tensor"]["output_queue_depth"] = (uint64_t)tensorOutputDepth.load();
          pData["tensor"]["max_packet_bytes"] = (uint64_t)ONNX::TensorWire::DEFAULT_MAX_PACKET_BYTES;
        } else if (activeModality == ONNX::ModelModality::AUDIO) {
          uint64_t hardDeadline = Util::bootMS() + 30000;
          size_t drained = 0;
          while (Util::bootMS() < hardDeadline) {
            JSON::Value md;
            bool has = false;
            {
              std::lock_guard<std::mutex> lock(transcriptMutex);
              if (!transcriptQueue.empty()) { md = transcriptQueue.front(); transcriptQueue.pop_front(); has = true; }
            }
            if (!has) {
              // Nothing queued right now: stop only once the producer has finished.
              if (audioProcessingDone) { break; }
              Util::sleep(20);
              continue;
            }
            // A short run can produce its FIRST result here (tail flush), so the meta
            // track may not exist yet — create it like the normal path does.
            ensureMetaTrack();
            if (metadataTrackIdx == INVALID_TRACK_ID) {
              WARN_MSG("Cannot create metadata track at shutdown; dropping trailing result(s)");
              break;
            }
            JSON::Value thisPack;
            thisPack.null();
            thisPack["trackid"] = metadataTrackIdx;
            thisPack["data"] = md.toString();
            thisPack["time"] = md["timestamp_ms"].asInt();
            thisPack["duration"] = 0;
            std::string tmpStr = thisPack.toNetPacked();
            thisPacket.reInit(tmpStr.data(), tmpStr.size());
            thisIdx = metadataTrackIdx;
            bufferLivePacket(thisPacket);
            uint64_t drainedTimestamp = md["timestamp_ms"].asInt();
            if (drainedTimestamp > statSinkMs) { statSinkMs = drainedTimestamp; }
            drained++;
          }
          if (drained) { INFO_MSG("ProcessSink drained %zu trailing transcript(s) at shutdown", (size_t)drained); }
        }

        INFO_MSG("ProcessSink thread ended, ingested %" PRIu64 " metadata packets and %" PRIu64 " video frames",
                 (uint64_t)metadataCount, (uint64_t)videoFrameCount);
      }
  };

  /// Check source, sink, model and other ONNX-specific options
  bool ProcONNX::CheckConfig() {
    // Model preflight. A known curated model (vision single-file OR audio bundle) may not
    // be on disk yet — MistProcONNX resolves and auto-provisions it at startup — so we do
    // NOT resolve/validate it here (audio bundles never resolve to a single path anyway).
    // Only an explicit path (custom model, model_path, or a non-registry id) is validated.
    std::string explicitPath;
    if (opt.isMember("model") && opt["model"].isString() && !opt["model"].asString().empty()) {
      std::string modelChoice = opt["model"].asString();
      if (modelChoice == "custom") {
        if (!opt.isMember("model_path") || opt["model_path"].asString().empty()) {
          FAIL_MSG("Custom model selected but no model_path provided");
          return false;
        }
        explicitPath = opt["model_path"].asString();
      } else if (ONNX::ModelRegistry::isKnownModelId(modelChoice)) {
        // Known curated model — resolved (and auto-provisioned if missing) at startup.
        // Capture its modality now so vision-only setup below is skipped for audio models.
        const ONNX::ModelRegistryEntry *e = ONNX::ModelRegistry::findModel(modelChoice);
        if (activeModality == ONNX::ModelModality::TENSOR && e && (!e->filename || !e->filename[0])) {
          FAIL_MSG("Raw tensor mode requires a single-file ONNX model; '%s' is a curated multi-file bundle",
                   modelChoice.c_str());
          return false;
        }
        if (e && activeModality != ONNX::ModelModality::TENSOR) { activeModality = e->modality; }
      } else {
        // Unknown id: treat as a bare path / cache filename and require it to resolve now.
        explicitPath = ONNX::ModelRegistry::resolveModelPath(modelChoice);
        if (explicitPath.empty()) {
          FAIL_MSG("Model '%s' is not a known model id and no matching file was found", modelChoice.c_str());
          return false;
        }
      }
    } else if (opt.isMember("model_path") && !opt["model_path"].asString().empty()) {
      explicitPath = opt["model_path"].asString();
    } else {
      FAIL_MSG("No model specified");
      return false;
    }

    if (!explicitPath.empty() && !ONNX::Utils::validateModelPath(explicitPath)) {
      FAIL_MSG("Model file not readable: %s", explicitPath.c_str());
      return false;
    }

    // Vision-only configuration. Options were already parsed + validated by parseConfig();
    // here we only APPLY the vision-tracking pieces (temporal tracker, scene-change detector,
    // Kalman filter), which are meaningless for audio/transcription models. Detection
    // thresholds and preprocessing are applied to the model in main().
    if (activeModality == ONNX::ModelModality::VISION) {
      tracker.setParameters(trackingIoU, trackMinConsecutiveMs, trackMaxMissingMs);
      sceneChangeDetector.enabled = sceneChangeEnabled;
      sceneChangeDetector.threshold = sceneChangeThreshold;
      tracker.enableKalmanFilter(kalmanEnabled);
      tracker.setKalmanProcessNoise(kalmanProcessNoise);
      tracker.setKalmanMeasurementNoise(kalmanMeasurementNoise);
      INFO_MSG("Vision config: model=%s conf=%.3f nms=%.3f input=%d | tracker IoU=%.3f min=%dms "
               "maxMissing=%dms | sceneChange=%s kalman=%s",
               explicitPath.empty() ? opt["model"].asStringRef().c_str() : explicitPath.c_str(),
               confThreshold, nmsThreshold, inputSize, trackingIoU, trackMinConsecutiveMs,
               trackMaxMissingMs, sceneChangeEnabled ? "on" : "off", kalmanEnabled ? "on" : "off");
    }

    return true;
  }

  void ProcONNX::Run() {
    uint64_t lastProcUpdate = Util::bootSecs();
    {
      std::lock_guard<std::mutex> guard(statsMutex);
      pStat["proc_status_update"]["id"] = getpid();
      pStat["proc_status_update"]["proc"] = "ONNX";
    }
    uint64_t startTime = Util::bootSecs();
    while (conf.is_active && co.is_active) {
      Util::sleep(200);
      if (lastProcUpdate + 5 <= Util::bootSecs()) {
        std::lock_guard<std::mutex> guard(statsMutex);
        pData["active_seconds"] = (Util::bootSecs() - startTime);
        pData["ainfo"]["sourceTime"] = statSourceMs;
        pData["ainfo"]["sinkTime"] = statSinkMs;
        if (activeModality == ONNX::ModelModality::TENSOR) {
          pData["tensor"]["runs"] = tensorRuns.load();
          pData["tensor"]["errors"] = tensorErrors.load();
          pData["tensor"]["input_drops"] = tensorInputDrops.load();
          pData["tensor"]["output_drops"] = tensorOutputDrops.load();
          pData["tensor"]["input_queue_depth"] = (uint64_t)tensorInputDepth.load();
          pData["tensor"]["output_queue_depth"] = (uint64_t)tensorOutputDepth.load();
          pData["tensor"]["max_packet_bytes"] = (uint64_t)ONNX::TensorWire::DEFAULT_MAX_PACKET_BYTES;
        } else if (activeModality == ONNX::ModelModality::AUDIO) {
          uint64_t chunks = asrChunks.load();
          uint64_t audioMs = asrAudioMs.load();
          uint64_t inferMs = asrInferMs.load();
          pData["asr"]["chunks"] = chunks;
          pData["asr"]["audio_seconds"] = audioMs / 1000.0;
          pData["asr"]["infer_seconds"] = inferMs / 1000.0;
          // Speed relative to realtime: audio duration / processing time. >1 = faster than
          // realtime (healthy), <1 = falling behind. (Named "speedup_x" to avoid confusion
          // with the conventional RTF = processing/audio, which is its reciprocal.)
          pData["asr"]["speedup_x"] = inferMs > 0 ? (double)audioMs / (double)inferMs : 0.0;
          pData["asr"]["buffered_seconds"] = audioWindower.bufferedSeconds();
          int asrRate = asrModel ? asrModel->sampleRate() : 0;
          pData["asr"]["dropped_seconds"] = asrRate ? audioWindower.droppedSamples() / (double)asrRate : 0.0;
        } else {
          uint64_t inferred = visionInferenceFrames.load();
          uint64_t processMs = visionProcessingMs.load();
          pData["vision"]["received_frames"] = visionReceivedFrames.load();
          pData["vision"]["queue_dropped_frames"] = visionDroppedFrames.load();
          pData["vision"]["rate_skipped_frames"] = visionRateSkippedFrames.load();
          pData["vision"]["inference_frames"] = inferred;
          pData["vision"]["average_processing_ms"] = inferred ? (double)processMs / inferred : 0.0;
          pData["vision"]["configured_max_fps"] = maxInferenceFps;
          pData["vision"]["annotated_video"] = annotatedVideo;
        }
        Util::sendUDPApi(pStat);
        lastProcUpdate = Util::bootSecs();
      }
    }
  }

  void sourceThread() {
    Util::nameThread("sourceThread");
    JSON::Value capa;
    Mist::ProcessSource::init(&conf, capa);
    conf.getOption("streamname", true).append(opt["source"].asString());
    JSON::Value targetOpt;
    targetOpt["arg"] = "string";
    targetOpt["default"] = "";
    targetOpt["arg_num"] = 1;
    conf.addOption("target", targetOpt);
    conf.getOption("target", true).append("-");
    std::string trackSel;
    if (opt.isMember("track_select") && opt["track_select"].isString() && !opt["track_select"].asString().empty()) {
      trackSel = opt["track_select"].asString();
    }
    if (activeModality == ONNX::ModelModality::AUDIO) {
      // Transcription consumes the raw PCM audio track. Force it unless the operator gave
      // an explicit audio selector — otherwise the controller's default (video=all) would
      // select video and starve the ASR pipeline.
      if (trackSel.empty() || trackSel == defaultVisionTrackSelector ||
          trackSel.find("audio=") == std::string::npos) {
        trackSel = "audio=PCM&video=none";
      }
    } else if (activeModality == ONNX::ModelModality::VISION) {
      // The inference process deliberately does not carry a second compressed-video
      // decoder. Restrict legacy `video=all` configurations to the formats its media
      // adapter can actually consume; this avoids a load/exit/restart loop on H264/AV1.
      if (trackSel.empty() || trackSel == "video=all") {
        trackSel = defaultVisionTrackSelector;
      }
    } else if (activeModality == ONNX::ModelModality::TENSOR) {
      trackSel = "meta=ONNXTENSOR&audio=none&video=none";
    }
    if (!trackSel.empty()) { conf.getOption("target", true).append("-?" + trackSel); }
    Socket::Connection S;
    Mist::ProcessSource out(S, conf, capa);
    MEDIUM_MSG("Running source thread...");
    out.run();
    INFO_MSG("Stop source thread...");
    co.is_active = false;
    isActive = false;
  }

  void sinkThread() {
    Util::nameThread("sinkThread");
    ProcessSink sink(&co);
    co.getOption("output", true).append("-");
    INFO_MSG("Running sink thread...");
    sink.run();
    INFO_MSG("Stop sink thread...");
    conf.is_active = false;
    isActive = false;
  }

  JSON::Value processVideoFrame(const ONNX::VideoPacket & vp) {
    JSON::Value result;
    result.null();
    result["schema"] = "mist.onnx.result/v1";
    result["timestamp_ms"] = vp.timestamp;
    result["detections"].append(); result["detections"].shrink(0);
    result["model"]["name"] = "auto-detected";
    result["kind"] = "object_detection";
    result["status"] = "skipped";

    // Check if ONNX is initialized
    {
      std::lock_guard<std::mutex> lock(onnxInitMutex);
      if (!onnxInitialized || !onnxModel) {
        WARN_MSG("ONNX not initialized, skipping frame processing");
        return result;
      }
    }

    try {
      auto pipelineResult = ONNX::Utils::processVideoPacketAuto(
        vp, *onnxModel, tracker, onnxStats, sceneChangeDetector, confThreshold, nmsThreshold, enhanceImage,
        jpegQuality, annotatedVideo, trackingEnabled, sceneChangeEnabled);
      JSON::Value metadata = pipelineResult.first;
      ONNX::ProcessedVideoFrame processedFrame = pipelineResult.second;

      // Model chaining: run secondary model on each primary detection crop
      if (secondaryModel && metadata.isMember("detections") && metadata["detections"].size() > 0) {
        cv::Mat frame = ONNX::Utils::decodeVideoFrame(
          (const char *)vp.packetData, vp.packetData.size(), vp.codec, vp.width, vp.height);

        if (!frame.empty()) {
          ONNX::ModelType secType = secondaryModel->getModelType();
          JSON::Value secResults;

          for (uint64_t i = 0; i < metadata["detections"].size(); ++i) {
            JSON::Value & det = metadata["detections"][(unsigned int)i];
            float dx = det["bbox"]["x"].asDouble();
            float dy = det["bbox"]["y"].asDouble();
            float dw = det["bbox"]["w"].asDouble();
            float dh = det["bbox"]["h"].asDouble();

            // Crop detection region from original frame
            int cx = std::max(0, (int)(dx * frame.cols));
            int cy = std::max(0, (int)(dy * frame.rows));
            int cw = std::min(frame.cols - cx, (int)(dw * frame.cols));
            int ch = std::min(frame.rows - cy, (int)(dh * frame.rows));
            if (cw <= 0 || ch <= 0) continue;

            cv::Mat crop = frame(cv::Rect(cx, cy, cw, ch));

            if (secType == ONNX::ModelType::FACE_RECOGNITION_ARCFACE ||
                secType == ONNX::ModelType::IMAGE_EMBEDDING) {
              ONNX::EmbeddingModel *embedder = dynamic_cast<ONNX::EmbeddingModel *>(secondaryModel.get());
              if (embedder) {
                ONNX::FaceEmbedding emb = embedder->processEmbeddingFrame(crop);
                det["embedding_dim"] = (uint64_t)emb.embedding.size();
                det["embedding_confidence"] = emb.confidence;
                if (emitEmbeddings) {
                  // ~4-8 KB of JSON per detection at 512-d; emit_embeddings=false
                  // keeps dim/confidence only for consumers that never match vectors
                  for (size_t ei = 0; ei < emb.embedding.size(); ++ei) {
                    det["embedding"].append(emb.embedding[ei]);
                  }
                }
              }
            } else if (secType == ONNX::ModelType::FACE_ATTRIBUTE) {
              ONNX::YOLOv8ClassificationModel *fa =
                dynamic_cast<ONNX::YOLOv8ClassificationModel *>(secondaryModel.get());
              if (fa) {
                // RAW top-2: class 0 = age (years), class 1 = gender probability.
                // The high-probability label is operator-set (genderHighLabel, default
                // "female" per UTKFace) so the polarity can be flipped without a rebuild
                // if a labeled sample shows it inverted.
                ONNX::ClassificationResult cr = fa->processClassificationFrame(crop);
                float age = 0.0f, genderProb = 0.0f;
                for (const auto & s : cr.top) {
                  if (s.class_id == 0) { age = s.confidence; }
                  else if (s.class_id == 1) { genderProb = s.confidence; }
                }
                det["age"] = (uint64_t)(age < 0 ? 0 : age + 0.5f);
                det["gender"] = genderProb >= 0.5f ? genderHighLabel : genderLowLabel;
                det["gender_prob"] = genderProb;
              }
            } else if (secType == ONNX::ModelType::YOLOV8_CLASSIFICATION ||
                       secType == ONNX::ModelType::YOLO11_CLASSIFICATION ||
                       secType == ONNX::ModelType::GENERIC_CLASSIFICATION) {
              ONNX::YOLOv8ClassificationModel *cls =
                dynamic_cast<ONNX::YOLOv8ClassificationModel *>(secondaryModel.get());
              if (cls) {
                ONNX::ClassificationResult cr = cls->processClassificationFrame(crop);
                det["secondary_class"] = cr.class_name;
                det["secondary_class_id"] = cr.class_id;
                det["secondary_confidence"] = cr.confidence;
                if (cr.top.size() > 1) {
                  for (size_t ti = 0; ti < cr.top.size(); ++ti) {
                    JSON::Value e;
                    e["class_id"] = cr.top[ti].class_id;
                    e["class_name"] = cr.top[ti].class_name;
                    e["confidence"] = cr.top[ti].confidence;
                    det["secondary_top"].append(e);
                  }
                }
              }
            } else {
              // Generic: run secondary as detector on the crop
              ONNX::InferenceMetrics secMetrics;
              std::vector<ONNX::Detection> secDets = secondaryModel->processFrame(crop, confThreshold, nmsThreshold, &secMetrics);
              JSON::Value subDets;
              for (const auto & sd : secDets) {
                JSON::Value sj;
                sj["class_name"] = sd.class_name;
                sj["confidence"] = sd.confidence;
                sj["bbox"]["x"] = sd.x;
                sj["bbox"]["y"] = sd.y;
                sj["bbox"]["w"] = sd.w;
                sj["bbox"]["h"] = sd.h;
                subDets.append(sj);
              }
              if (subDets.size() > 0) { det["secondary_detections"] = subDets; }
            }
          }
        }
      }

      // Store processed video frame for ProcessSink
      if (processedFrame.jpegData.size() > 0) {
        std::lock_guard<std::mutex> lock(latestProcessedVideoMutex);
        latestProcessedVideo = processedFrame;
        hasLatestProcessedVideo = true;
      }

      return metadata;

    } catch (const std::exception & e) {
      ERROR_MSG("ONNX inference error: %s", e.what());
      result["status"] = "error";
      result["error"]["message"] = e.what();
      return result;
    }
  }

  void processThread() {
    INFO_MSG("Running processing thread...");

    uint64_t processedFrames = 0;
    uint64_t lastStatsTime = 0;
    uint64_t lastFrameTime = 0; // For frame-to-frame timing
    uint64_t lastInferenceTimestamp = 0;
    uint64_t noVideoCount = 0; // Count how often we wait for video

    // Wait for ONNX to be initialized
    if (!onnxInitialized) { INFO_MSG("Processing thread waiting for ONNX initialization..."); }
    while (isActive) {
      {
        std::lock_guard<std::mutex> lock(onnxInitMutex);
        if (onnxInitialized) break;
      }
      Util::sleep(100);
    }

    if (!isActive) {
      INFO_MSG("Processing thread exiting - not active");
      return;
    }

    INFO_MSG("Processing thread starting - ONNX is ready");

    if (activeModality == ONNX::ModelModality::TENSOR) {
      while (isActive) {
        TensorPacket packet;
        bool havePacket = false;
        {
          std::lock_guard<std::mutex> lock(tensorInputMutex);
          if (!tensorInputQueue.empty()) {
            packet = std::move(tensorInputQueue.front());
            tensorInputQueue.pop_front();
            tensorInputDepth = tensorInputQueue.size();
            havePacket = true;
          }
        }
        if (!havePacket) { Util::sleep(5); continue; }
        std::vector<ONNX::TensorData> inputs, outputs;
        std::string err;
        if (!ONNX::TensorWire::decode(packet.data.data(), packet.data.size(), inputs, err) ||
            !tensorModel || !tensorModel->runTensors(inputs, outputs, err)) {
          tensorErrors++;
          WARN_MSG("ONNXTENSOR inference failed at %" PRIu64 "ms: %s", packet.timestamp, err.c_str());
          continue;
        }
        TensorPacket result;
        result.timestamp = packet.timestamp;
        if (!ONNX::TensorWire::encode(outputs, result.data, err)) {
          tensorErrors++;
          WARN_MSG("ONNXTENSOR output encoding failed: %s", err.c_str());
          continue;
        }
        {
          std::lock_guard<std::mutex> lock(tensorOutputMutex);
          if (tensorOutputQueue.size() >= maxTensorQueueDepth) {
            tensorOutputQueue.pop_front();
            tensorOutputDrops++;
          }
          tensorOutputQueue.push_back(std::move(result));
          tensorOutputDepth = tensorOutputQueue.size();
        }
        tensorRuns++;
      }
      INFO_MSG("Tensor processing thread exiting after %" PRIu64 " runs", tensorRuns.load());
      return;
    }

    // Audio modality, fixed-chunk streaming models (VAD): pop exact chunks, smooth the
    // per-chunk speech probability into started/ended events plus a periodic score.
    if (activeModality == ONNX::ModelModality::AUDIO && audioModel && audioModel->chunkSamples() > 0) {
      std::vector<float> chunk;
      uint64_t baseMs = 0;
      const size_t chunkN = (size_t)audioModel->chunkSamples();
      const std::string audioName = opt.isMember("model") ? opt["model"].asString() : "audio";
      uint64_t lastScoreMs = 0;
      bool scoredOnce = false;
      const uint64_t chunkMs = chunkN * 1000ull / (uint64_t)audioModel->sampleRate();
      uint64_t expectedMs = 0;
      bool haveExpected = false;
      uint64_t lastDisc = audioWindower.discontinuities();
      while (isActive) {
        if (!audioWindower.takeFixed(chunkN, chunk, baseMs)) {
          Util::sleep(10);
          continue;
        }
        // Stream discontinuity (seek / source restart): the recurrent VAD state and an
        // active speech phase are stale — close the event and reset before processing.
        // Detected two ways: the windower's own feed()-side jump counter (catches jumps
        // while audio is buffered), and the emitted chunk timestamps (catches
        // backpressure drop-oldest jumps).
        bool discontinuity = false;
        uint64_t disc = audioWindower.discontinuities();
        if (disc != lastDisc) {
          lastDisc = disc;
          discontinuity = true;
        } else if (haveExpected) {
          // Same threshold as the windower's feed-side check: a seek landing while
          // the FIFO happens to be empty resyncs there without bumping the counter,
          // and must still be caught here.
          uint64_t gap = baseMs > expectedMs ? baseMs - expectedMs : expectedMs - baseMs;
          discontinuity = gap > 2000;
        }
        if (haveExpected && discontinuity) {
          INFO_MSG("Audio discontinuity at %" PRIu64 "ms: resetting VAD state", baseMs);
          if (vadSmoother.active()) {
            JSON::Value md = ONNX::Utils::eventToJSON("speech", false, vadSmoother.value(),
                                                      expectedMs, audioName);
            std::lock_guard<std::mutex> lock(transcriptMutex);
            transcriptQueue.push_back(md);
          }
          audioModel->reset();
          vadSmoother.configure((float)eventEnter, (float)eventExit, eventEma, eventMinMs);
        }
        expectedMs = baseMs + chunkMs;
        haveExpected = true;
        ONNX::AudioResult r = audioModel->process(chunk.data(), chunk.size(), baseMs);
        if (!r.ok || r.scores.empty()) { continue; }
        ONNX::EventSmoother::Event ev = vadSmoother.update(r.scores[0].confidence, r.startMs);
        if (ev != ONNX::EventSmoother::NONE) {
          JSON::Value md = ONNX::Utils::eventToJSON("speech", ev == ONNX::EventSmoother::STARTED,
                                                    vadSmoother.value(), r.startMs, audioName);
          std::lock_guard<std::mutex> lock(transcriptMutex);
          transcriptQueue.push_back(md);
        }
        // Periodic smoothed score (1/s) so consumers see liveness between events
        if (!scoredOnce || r.startMs - lastScoreMs >= 1000) {
          scoredOnce = true;
          lastScoreMs = r.startMs;
          r.scores[0].confidence = vadSmoother.value();
          JSON::Value md = ONNX::Utils::audioResultToJSON(r, audioName, "vad");
          std::lock_guard<std::mutex> lock(transcriptMutex);
          transcriptQueue.push_back(md);
        }
      }
      // End of stream while speech is active: close the event so consumers pairing
      // started/ended never see a dangling open interval. The sink's shutdown drain
      // delivers this.
      if (vadSmoother.active()) {
        JSON::Value md = ONNX::Utils::eventToJSON("speech", false, vadSmoother.value(),
                                                  expectedMs, audioName);
        std::lock_guard<std::mutex> lock(transcriptMutex);
        transcriptQueue.push_back(md);
      }
      audioProcessingDone = true;
      INFO_MSG("Processing thread (audio streaming) exiting");
      return;
    }

    // Audio modality, pause-windowed models: pull complete PCM windows and run the ASR
    // model (transcription JSON) or a generic windowed audio model (scores/embedding
    // JSON) on each. No video frames are involved.
    if (activeModality == ONNX::ModelModality::AUDIO) {
      std::vector<float> window;
      uint64_t baseMs = 0;
      const int audioRate = asrModel ? asrModel->sampleRate()
                                     : (audioModel ? audioModel->sampleRate() : 16000);
      const std::string audioName = opt.isMember("model") ? opt["model"].asString() : "audio";
      // emitTranscription: run the model on one window and publish JSON to the sink.
      auto emitTranscription = [&](const std::vector<float> & pcm, uint64_t ts) {
        if (!asrModel) {
          // Generic windowed audio model (classification/tagging/embedding)
          ONNX::AudioResult r = audioModel->process(pcm.data(), pcm.size(), ts);
          if (!r.ok) { return; }
          const char *kind = "audio_classification";
          if (audioModel->task() == ONNX::ModelType::AUDIO_TAGGING) { kind = "audio_tagging"; }
          else if (audioModel->task() == ONNX::ModelType::AUDIO_EMBEDDING) { kind = "audio_embedding"; }
          JSON::Value metadata = ONNX::Utils::audioResultToJSON(r, audioName, kind);
          std::lock_guard<std::mutex> lock(transcriptMutex);
          transcriptQueue.push_back(metadata);
          return;
        }
        uint64_t t0 = Util::getMicros();
        ONNX::TranscriptResult tr = asrModel->transcribe(pcm.data(), pcm.size(), ts);
        uint64_t inferMs = (Util::getMicros() - t0) / 1000;
        uint64_t audioMs = asrModel->sampleRate() ? (pcm.size() * 1000ull / (uint64_t)asrModel->sampleRate()) : 0;
        asrChunks++;
        asrAudioMs += audioMs;
        asrInferMs += inferMs;
        if (!tr.ok || tr.text.empty()) { return; }
        JSON::Value metadata;
        metadata["schema"] = "mist.onnx.result/v1";
        metadata["timestamp_ms"] = ts;
        metadata["window"]["start_ms"] = ts;
        metadata["window"]["end_ms"] = ts + audioMs;
        metadata["model"]["name"] = opt.isMember("model") ? opt["model"] : JSON::Value("parakeet");
        metadata["kind"] = "transcription";
        metadata["status"] = "ok";
        metadata["transcription"] = tr.text;
        for (const ONNX::TranscriptSegment & s : tr.segments) {
          JSON::Value seg;
          seg["start_ms"] = s.startMs;
          seg["end_ms"] = s.endMs;
          seg["text"] = s.text;
          seg["confidence"] = s.confidence;
          metadata["segments"].append(seg);
        }
        {
          std::lock_guard<std::mutex> lock(transcriptMutex);
          transcriptQueue.push_back(metadata);
        }
        VERYHIGH_MSG("ASR window @%" PRIu64 "ms: %s", ts, tr.text.c_str());
      };
      uint64_t audioStartMs = Util::bootMS();
      uint64_t windowsDone = 0;
      bool warnedNoWindows = false;
      // Idle-flush: emit buffered speech when the feed goes quiet before a chunk boundary is
      // reached, so trailing words aren't stuck until max-chunk or shutdown on a live stream.
      double lastBufSec = -1.0;
      uint64_t lastBufChangeMs = Util::bootMS();
      const uint64_t ASR_IDLE_FLUSH_MS = 1500;
      uint64_t lastDropWarnMs = 0;
      while (isActive) {
        if (!audioWindower.takeWindow(window, baseMs)) {
          // Guaranteed fallback (runs even if the source selected no tracks and never
          // called sendNext): flag a silent no-audio situation instead of hanging. Checks
          // whether any audio has been RECEIVED (buffered), not whether a window completed —
          // the first window legitimately takes chunk-target seconds to fill, so a
          // windows-completed check here would false-positive on a healthy feed.
          if (!warnedNoWindows && windowsDone == 0 && audioWindower.bufferedSeconds() == 0.0 &&
              Util::bootMS() - audioStartMs > 8000) {
            warnedNoWindows = true;
            WARN_MSG("Audio processing: no audio received after 8s. The source needs a %d Hz PCM audio track — "
                     "chain MistProcAV in front (Input type=audio, codec=PCM, sample_rate=%d).",
                     audioRate, audioRate);
          }
          // Track buffer growth; if it has been static (feed stalled) for ASR_IDLE_FLUSH_MS
          // and holds >= 0.3s, flush it so the tail is transcribed instead of waiting.
          double bufSec = audioWindower.bufferedSeconds();
          if (bufSec != lastBufSec) { lastBufSec = bufSec; lastBufChangeMs = Util::bootMS(); }
          else if (bufSec > 0.0 && Util::bootMS() - lastBufChangeMs > ASR_IDLE_FLUSH_MS) {
            if (audioWindower.flush(window, baseMs, (size_t)(audioRate * 0.3))) {
              windowsDone++;
              emitTranscription(window, baseMs);
            }
            lastBufSec = -1.0;
            lastBufChangeMs = Util::bootMS();
          }
          Util::sleep(50);
          continue;
        }
        windowsDone++;
        emitTranscription(window, baseMs);
        // Surface sustained backpressure (dropped audio) as a rate-limited warning.
        uint64_t dropped = audioWindower.droppedSamples();
        if (dropped > 0 && audioRate > 0 && Util::bootMS() - lastDropWarnMs > 5000) {
          lastDropWarnMs = Util::bootMS();
          WARN_MSG("Audio processing falling behind realtime: dropped %.1fs of audio so far (inference slower "
                   "than input; raise threads / use a faster model / raise max_buffer_seconds).",
                   (double)dropped / (double)audioRate);
        }
      }
      // Process the trailing tail (>= 0.3s). Best-effort on shutdown: the sink does a
      // bounded post-loop drain to deliver this, but delivery isn't guaranteed if the
      // stream tears down first.
      if ((asrModel || audioModel) && audioWindower.flush(window, baseMs, (size_t)(audioRate * 0.3))) {
        emitTranscription(window, baseMs);
      }
      audioProcessingDone = true; // signal the sink drain that no more transcripts will arrive
      INFO_MSG("Processing thread (audio) exiting");
      return;
    }

    while (isActive) {
      ONNX::VideoPacket vp;
      bool hasVideo = false;

      // Measure mutex contention for video queue
      {
        std::lock_guard<std::mutex> lock(latestVideoMutex);
        if (hasLatestVideo) {
          vp = latestVideo;
          hasLatestVideo = false;
          hasVideo = true;
        }
      }

      uint64_t currentTime = Util::bootSecs();
      if (hasVideo) {
        processedFrames++;
        if (processEveryNth > 1 && (processedFrames % processEveryNth) != 1) {
          visionRateSkippedFrames++;
          continue;
        }
        if (maxInferenceFps > 0.0 && lastInferenceTimestamp && vp.timestamp > lastInferenceTimestamp) {
          const double minimumIntervalMs = 1000.0 / maxInferenceFps;
          if ((double)(vp.timestamp - lastInferenceTimestamp) < minimumIntervalMs) {
            visionRateSkippedFrames++;
            continue;
          }
        }
        lastInferenceTimestamp = vp.timestamp;
        uint64_t frameStartTime = Util::bootMS();

        // Calculate frame-to-frame delay (pipeline timing)
        if (lastFrameTime > 0) {
          uint64_t frameToFrameDelay = frameStartTime - lastFrameTime;

          // Log significant pipeline delays
          if (frameToFrameDelay > 1000) {
            WARN_MSG("MAJOR PIPELINE STUTTER: %" PRIu64 "ms between frames! This is the real stutter source!",
                     (uint64_t)frameToFrameDelay);
          } else if (frameToFrameDelay > 500) {
            WARN_MSG("Pipeline delay: %" PRIu64 "ms between frames", (uint64_t)frameToFrameDelay);
          }
        }
        lastFrameTime = frameStartTime;

        // Process video frame with ONNX using library function
        JSON::Value metadata = Mist::processVideoFrame(vp);
        uint64_t frameEndTime = Util::bootMS();

        // Calculate total frame processing time
        int64_t totalFrameTimeMs = frameEndTime - frameStartTime;
        visionInferenceFrames++;
        visionProcessingMs += totalFrameTimeMs > 0 ? (uint64_t)totalFrameTimeMs : 0;

        // Every result kind reaches the sink — detections, classification, embedding,
        // depth, SAM2 and generic alike.
        bool hasResult = metadata.isMember("detections") || metadata.isMember("classification") ||
                         metadata.isMember("embedding_dim") || metadata.isMember("depth_map_width") ||
                         metadata.isMember("iou_scores") || metadata.isMember("raw_output") ||
                         metadata.isMember("lines");
        if (hasResult) {
          // Vision event smoothing: track one class's score with hysteresis. The state
          // rides EVERY metadata packet (drop-tolerant with the latest-only slot);
          // transitions additionally carry changed=started/ended.
          if (!eventClass.empty() && metadata.isMember("classification")) {
            float score = 0.0f;
            bool found = false;
            if (metadata["classification"].isMember("top")) {
              jsonForEachConst(metadata["classification"]["top"], it) {
                if ((*it)["class_name"].asString() == eventClass) {
                  score = (float)(*it)["confidence"].asDouble();
                  found = true;
                  break;
                }
              }
            }
            if (!found) {
              // Not in the reported top-K. For a 2-class head both classes are always
              // present in top, so this only fires for multi-class / cosine-tag models —
              // where the tracked class is genuinely absent this frame (score 0), NOT its
              // complement (1-conf would turn a low cosine into a spurious high score).
              if (metadata["classification"]["class_name"].asString() == eventClass) {
                score = (float)metadata["classification"]["confidence"].asDouble();
              } else {
                score = 0.0f;
              }
            }
            ONNX::EventSmoother::Event ev = visionSmoother.update(score, vp.timestamp);
            metadata["event_state"]["label"] = eventClass;
            metadata["event_state"]["active"] = visionSmoother.active();
            metadata["event_state"]["score"] = visionSmoother.value();
            if (visionSmoother.active()) { metadata["event_state"]["since"] = visionSmoother.activeSinceMs(); }
            if (ev != ONNX::EventSmoother::NONE) {
              metadata["event_state"]["changed"] = (ev == ONNX::EventSmoother::STARTED) ? "started" : "ended";
              INFO_MSG("Event '%s' %s @%" PRIu64 "ms (score %.3f)", eventClass.c_str(),
                       ev == ONNX::EventSmoother::STARTED ? "started" : "ended",
                       (uint64_t)vp.timestamp, visionSmoother.value());
              // Transitions also go through the ordered queue: the latest-only slot
              // may be overwritten by the next frame before the sink reads it.
              JSON::Value evPacket = ONNX::Utils::eventToJSON(
                eventClass, ev == ONNX::EventSmoother::STARTED, visionSmoother.value(),
                vp.timestamp, opt.isMember("model") ? opt["model"].asString() : "vision");
              std::lock_guard<std::mutex> lock(transcriptMutex);
              transcriptQueue.push_back(evPacket);
            }
          }
          {
            std::lock_guard<std::mutex> lock(latestMetadataMutex);
            latestMetadata = metadata;
            hasLatestMetadata = true;
          }
          if (metadata.isMember("detections") && metadata["detections"].size() > 0) {
            VERYHIGH_MSG("Metadata JSON being sent: %s", metadata.toString().c_str());
          }
        }

        // Log warnings for performance issues
        if (totalFrameTimeMs > 1000) {
          WARN_MSG("Slow frame processing: %" PRId64 "ms total time", (int64_t)totalFrameTimeMs);
        }

      } else {
        // No video available, sleep briefly
        Util::sleep(100);
        noVideoCount++;
        if (noVideoCount % 100 == 1) {
          INFO_MSG("Processing thread waiting for video - count: %" PRIu64, (uint64_t)noVideoCount);
        }
      }

      // Log stats every 30 seconds even if no frames
      if (currentTime - lastStatsTime > 30) {
        onnxStats.logStats();
        lastStatsTime = currentTime;
      }
    }

    // Final stats when thread ends
    INFO_MSG("Processing thread ended, processed %" PRIu64 " frames", (uint64_t)processedFrames);
    onnxStats.logStats();
    isActive = false;
  }

}; // namespace Mist

// Single source of truth for option parsing: read every option from `opt` exactly once,
// validate, clamp, and store into the config globals. Called at startup after `opt` is
// populated and before CheckConfig/Run. No other function reads `opt` for these keys — they
// all consume the globals. (Model selection/resolution and the model_type->enum mapping stay
// in main(): they are intertwined with registry lookup and auto-provisioning.)
static void parseConfig() {
  auto has = [](const char *k) { return opt.isMember(k); };

  if (has("input_mode") && opt["input_mode"].isString()) { inputMode = opt["input_mode"].asString(); }
  if (inputMode == "tensor") { activeModality = ONNX::ModelModality::TENSOR; }
  else if (inputMode != "auto") {
    WARN_MSG("Unknown input_mode '%s'; using auto", inputMode.c_str());
    inputMode = "auto";
  }
  if (has("tensor_queue_depth") && opt["tensor_queue_depth"].isInt()) {
    int configuredDepth = opt["tensor_queue_depth"].asInt();
    maxTensorQueueDepth = (size_t)(configuredDepth < 1 ? 1 : (configuredDepth > 64 ? 64 : configuredDepth));
  }

  // -- Runtime / performance (both modalities) --
  if (has("threads") && opt["threads"].isInt()) {
    numThreads = opt["threads"].asInt();
    if (!ONNX::Utils::validateThreadCount(numThreads)) {
      numThreads = numThreads < 1 ? 1 : (numThreads > 32 ? 32 : numThreads);
    }
  }
  if (has("execution_provider") && opt["execution_provider"].isString()) { epChoice = opt["execution_provider"].asString(); }
  if (has("low_latency") && opt["low_latency"].isBool()) { lowLatency = opt["low_latency"].asBool(); }
  if (has("realtime_priority") && opt["realtime_priority"].isBool()) { realtimePriority = opt["realtime_priority"].asBool(); }
  if (has("output_id") && opt["output_id"].isString() && !opt["output_id"].asString().empty()) {
    outputId = opt["output_id"].asString();
  }

  // -- Detection thresholds (vision) --
  if (has("conf_threshold") && (opt["conf_threshold"].isDouble() || opt["conf_threshold"].isInt())) {
    confThreshold = (float)opt["conf_threshold"].asDouble();
    if (!ONNX::Utils::validateThreshold(confThreshold, "conf_threshold")) { confThreshold = 0.5f; }
  }
  if (has("nms_threshold") && (opt["nms_threshold"].isDouble() || opt["nms_threshold"].isInt())) {
    nmsThreshold = (float)opt["nms_threshold"].asDouble();
    if (!ONNX::Utils::validateThreshold(nmsThreshold, "nms_threshold")) { nmsThreshold = 0.4f; }
  }
  if (has("soft_nms_sigma")) {
    softNmsSigma = (float)opt["soft_nms_sigma"].asDouble();
    if (softNmsSigma < 0.1f || softNmsSigma > 1.0f) {
      WARN_MSG("soft_nms_sigma should be between 0.1 and 1.0, using default 0.5");
      softNmsSigma = 0.5f;
    }
  }

  // -- Preprocessing / output (vision) --
  if (has("input_size") && opt["input_size"].isInt()) {
    inputSize = opt["input_size"].asInt();
    if (!ONNX::Utils::validateInputSize(inputSize)) { inputSize = 640; }
  }
  if (has("process_every_nth") && opt["process_every_nth"].isInt()) {
    processEveryNth = opt["process_every_nth"].asInt();
    if (processEveryNth < 1) { processEveryNth = 1; }
  }
  if (has("max_inference_fps")) {
    maxInferenceFps = opt["max_inference_fps"].asDouble();
    if (maxInferenceFps < 0.0 || maxInferenceFps > 120.0) {
      WARN_MSG("max_inference_fps %.2f out of range [0,120]; using 5", maxInferenceFps);
      maxInferenceFps = 5.0;
    }
  }
  if (has("enhance_image") && opt["enhance_image"].isBool()) { enhanceImage = opt["enhance_image"].asBool(); }
  if (has("annotated_video") && opt["annotated_video"].isBool()) { annotatedVideo = opt["annotated_video"].asBool(); }
  if (has("jpeg_quality") && opt["jpeg_quality"].isInt()) {
    jpegQuality = opt["jpeg_quality"].asInt();
    jpegQuality = jpegQuality < 1 ? 1 : (jpegQuality > 100 ? 100 : jpegQuality);
  }
  if (has("letterbox") && opt["letterbox"].isBool()) { letterboxOpt = opt["letterbox"].asBool() ? 1 : 0; }
  if (has("normalization") && opt["normalization"].isString()) { normalizationMode = opt["normalization"].asString(); }
  if (has("resize_mode") && opt["resize_mode"].isString()) { resizeMode = opt["resize_mode"].asString(); }
  if (has("secondary_model_path") && opt["secondary_model_path"].isString()) { secondaryModelPath = opt["secondary_model_path"].asString(); }
  if (has("secondary_model_type") && opt["secondary_model_type"].isString()) { secondaryModelType = opt["secondary_model_type"].asString(); }
  if (has("emit_embeddings") && opt["emit_embeddings"].isBool()) { emitEmbeddings = opt["emit_embeddings"].asBool(); }
  if (has("gender_high_label") && opt["gender_high_label"].isString()) { genderHighLabel = opt["gender_high_label"].asString(); }
  if (has("gender_low_label") && opt["gender_low_label"].isString()) { genderLowLabel = opt["gender_low_label"].asString(); }

  // -- Transcription / audio chunking --
  if (has("window_seconds")) {
    windowTargetSec = opt["window_seconds"].asDouble();
    if (!(windowTargetSec >= 1.0 && windowTargetSec <= 120.0)) {
      WARN_MSG("window_seconds %.2f out of range [1,120]; using 5", windowTargetSec);
      windowTargetSec = 5.0;
    }
  }
  windowMaxSec = has("max_window_seconds") ? opt["max_window_seconds"].asDouble() : 10.0;
  windowMinSec = has("min_window_seconds") ? opt["min_window_seconds"].asDouble() : 1.5;
  if (!(windowMaxSec >= windowTargetSec && windowMaxSec <= 180.0)) { windowMaxSec = std::max(10.0, windowTargetSec); }
  if (!(windowMinSec > 0.0 && windowMinSec <= windowTargetSec)) { windowMinSec = std::min(1.5, windowTargetSec); }
  maxBufferSec = has("max_buffer_seconds") ? opt["max_buffer_seconds"].asDouble() : 30.0;
  if (maxBufferSec < windowMaxSec) { maxBufferSec = std::max(30.0, windowMaxSec); }
  if (has("event_enter")) { eventEnter = opt["event_enter"].asDouble(); }
  if (has("event_exit")) { eventExit = opt["event_exit"].asDouble(); }
  if (has("event_ema")) { eventEma = opt["event_ema"].asDouble(); }
  if (!(eventEma > 0.0 && eventEma <= 1.0)) { eventEma = 0.3; }
  if (has("event_min_ms")) {
    int64_t v = opt["event_min_ms"].asInt();
    eventMinMs = v > 0 ? (uint64_t)v : 0;
  }
  if (has("top_k") && opt["top_k"].isInt()) {
    int64_t v = opt["top_k"].asInt();
    classifierTopK = (v < 1) ? 1 : (v > 100 ? 100 : (unsigned)v);
  }
  if (has("classifier_mode") && opt["classifier_mode"].isString()) { classifierMode = opt["classifier_mode"].asString(); }
  if (has("event_class") && opt["event_class"].isString()) { eventClass = opt["event_class"].asString(); }
  if (!(eventEnter > 0.0 && eventEnter <= 1.0)) { eventEnter = 0.5; }
  if (!(eventExit > 0.0 && eventExit < eventEnter)) { eventExit = eventEnter * 0.7; }

  // -- Temporal tracking / scene-change / Kalman (vision) --
  if (has("enable_tracking") && opt["enable_tracking"].isBool()) { trackingEnabled = opt["enable_tracking"].asBool(); }
  if (has("min_consecutive_ms") && opt["min_consecutive_ms"].isInt()) { trackMinConsecutiveMs = opt["min_consecutive_ms"].asInt(); }
  if (has("max_missing_ms") && opt["max_missing_ms"].isInt()) { trackMaxMissingMs = opt["max_missing_ms"].asInt(); }
  if (has("tracking_iou_threshold") && opt["tracking_iou_threshold"].isDouble()) { trackingIoU = (float)opt["tracking_iou_threshold"].asDouble(); }
  if (has("enable_scene_change_detection") && opt["enable_scene_change_detection"].isBool()) { sceneChangeEnabled = opt["enable_scene_change_detection"].asBool(); }
  if (has("scene_change_threshold") && opt["scene_change_threshold"].isDouble()) { sceneChangeThreshold = (float)opt["scene_change_threshold"].asDouble(); }
  if (has("enable_kalman_filter") && opt["enable_kalman_filter"].isBool()) { kalmanEnabled = opt["enable_kalman_filter"].asBool(); }
  if (has("kalman_process_noise") && opt["kalman_process_noise"].isDouble()) {
    kalmanProcessNoise = (float)opt["kalman_process_noise"].asDouble();
    if (kalmanProcessNoise < 0.001f || kalmanProcessNoise > 0.1f) { kalmanProcessNoise = 0.01f; }
  }
  if (has("kalman_measurement_noise") && opt["kalman_measurement_noise"].isDouble()) {
    kalmanMeasurementNoise = (float)opt["kalman_measurement_noise"].asDouble();
    if (kalmanMeasurementNoise < 0.01f || kalmanMeasurementNoise > 1.0f) { kalmanMeasurementNoise = 0.1f; }
  }
}

int main(int argc, char *argv[]) {
  DTSC::trackValidMask = TRACK_VALID_INT_PROCESS;
  Util::Config config(argv[0]);
  Util::Config::binaryType = Util::PROCESS;
  JSON::Value capa;

  // Standard JSON options
  {
    JSON::Value opt;
    opt["arg"] = "string";
    opt["default"] = "-";
    opt["arg_num"] = 1;
    opt["help"] = "JSON config or - for stdin";
    config.addOption("configuration", opt);
    opt.null();
    opt["long"] = "json";
    opt["short"] = "j";
    opt["help"] = "Show connector JSON and exit";
    opt["value"].append(0);
    config.addOption("json", opt);
  }

  // Input codecs. Vision models take a raw/JPEG video track; transcription models take a
  // raw PCM audio track (as a separate alternative codec-set for the input).
  capa["codecs"][0u][0u].append("YUYV");
  capa["codecs"][0u][0u].append("NV12");
  capa["codecs"][0u][0u].append("UYVY");
  capa["codecs"][0u][0u].append("I420");
  capa["codecs"][0u][0u].append("JPEG");
  capa["codecs"][0u][1u].append("PCM"); // audio input for transcription models
  capa["codecs"][0u][2u].append("ONNXTENSOR"); // arbitrary typed tensor input

  // Output codecs - metadata and video with bounding boxes
  capa["codecs"][1u][0u].append("JSON"); // Metadata track
  capa["codecs"][1u][1u].append("JPEG"); // Video track with bounding boxes
  capa["codecs"][1u][2u].append("ONNXTENSOR"); // arbitrary typed tensor output

  capa["ainfo"]["sinkTime"]["name"] = "Sink timestamp";
  capa["ainfo"]["sourceTime"]["name"] = "Source timestamp";

  if (!config.parseArgs(argc, argv)) return 1;
  if (config.getBool("json")) {
    capa["name"] = "ONNX-AI";
    capa["hrn"] = "ONNX AI Processing";
    capa["desc"] = "Run arbitrary typed tensors or curated vision/audio adapters via ONNX Runtime: "
                   "raw ONNXTENSOR, video detection/pose/etc., or speech-to-text from PCM.";
    addGenericProcessOptions(capa);

    capa["optional"]["source_mask"]["name"] = "Source track mask";
    capa["optional"]["source_mask"]["help"] = "What internal processes should have access to the source track(s)";
    capa["optional"]["source_mask"]["type"] = "select";
    capa["optional"]["source_mask"]["select"][0u][0u] = "";
    capa["optional"]["source_mask"]["select"][0u][1u] = "Keep original value";
    capa["optional"]["source_mask"]["select"][1u][0u] = 255;
    capa["optional"]["source_mask"]["select"][1u][1u] = "Everything";
    capa["optional"]["source_mask"]["select"][2u][0u] = 4;
    capa["optional"]["source_mask"]["select"][2u][1u] = "Processing tasks (not viewers, not pushes)";
    capa["optional"]["source_mask"]["select"][3u][0u] = 6;
    capa["optional"]["source_mask"]["select"][3u][1u] = "Processing and pushing tasks (not viewers)";
    capa["optional"]["source_mask"]["select"][4u][0u] = 5;
    capa["optional"]["source_mask"]["select"][4u][1u] = "Processing and viewer tasks (not pushes)";
    capa["optional"]["source_mask"]["default"] = "";

    capa["optional"]["target_mask"]["name"] = "Output track mask";
    capa["optional"]["target_mask"]["help"] = "What internal processes should have access to the ouput track(s)";
    capa["optional"]["target_mask"]["type"] = "select";
    capa["optional"]["target_mask"]["select"][0u][0u] = "";
    capa["optional"]["target_mask"]["select"][0u][1u] = "Keep original value";
    capa["optional"]["target_mask"]["select"][1u][0u] = 255;
    capa["optional"]["target_mask"]["select"][1u][1u] = "Everything";
    capa["optional"]["target_mask"]["select"][2u][0u] = 1;
    capa["optional"]["target_mask"]["select"][2u][1u] = "Viewer tasks (not processing, not pushes)";
    capa["optional"]["target_mask"]["select"][3u][0u] = 2;
    capa["optional"]["target_mask"]["select"][3u][1u] = "Pushing tasks (not processing, not viewers)";
    capa["optional"]["target_mask"]["select"][4u][0u] = 4;
    capa["optional"]["target_mask"]["select"][4u][1u] = "Processing tasks (not pushes, not viewers)";
    capa["optional"]["target_mask"]["select"][5u][0u] = 3;
    capa["optional"]["target_mask"]["select"][5u][1u] = "Viewer and pushing tasks (not processing)";
    capa["optional"]["target_mask"]["select"][6u][0u] = 5;
    capa["optional"]["target_mask"]["select"][6u][1u] = "Viewer and processing tasks (not pushes)";
    capa["optional"]["target_mask"]["select"][7u][0u] = 6;
    capa["optional"]["target_mask"]["select"][7u][1u] = "Pushing and processing tasks (not viewers)";
    capa["optional"]["target_mask"]["select"][8u][0u] = 0;
    capa["optional"]["target_mask"]["select"][8u][1u] = "Nothing";
    capa["optional"]["target_mask"]["default"] = "";

    capa["optional"]["exit_unmask"]["name"] = "Undo masks on process exit/fail";
    capa["optional"]["exit_unmask"]["help"] = "If/when the process exits or fails, the masks for input tracks will "
                                              "be reset to defaults. "
                                              "(NOT to previous value, but to defaults!)";
    capa["optional"]["exit_unmask"]["default"] = false;

    capa["optional"]["sink"]["name"] = "Target stream";
    capa["optional"]["sink"]["help"] = "What stream the encoded track should be added to. Defaults "
                                       "to source stream. May contain variables.";
    capa["optional"]["sink"]["type"] = "string";
    capa["optional"]["sink"]["validate"][0u] = "streamname_with_wildcard_and_variables";

    capa["optional"]["track_select"]["name"] = "Source selector(s)";
    capa["optional"]["track_select"]["help"] = "What tracks to select for the input. Vision models consume raw/JPEG video "
                                               "and default to YUYV/UYVY/NV12/I420/JPEG only; compressed H264/AV1 must be "
                                               "decoded upstream with MistProcAV (NV12 recommended). Transcription consumes "
                                               "raw PCM from MistProcAV; tensor mode consumes meta/ONNXTENSOR.";
    capa["optional"]["track_select"]["type"] = "string";
    capa["optional"]["track_select"]["validate"][0u] = "track_selector";
    capa["optional"]["track_select"]["default"] = defaultVisionTrackSelector;

    capa["optional"]["track_inhibit"]["name"] = "Track inhibitor(s)";
    capa["optional"]["track_inhibit"]["help"] = "What tracks to use as inhibitors. If this track selector is able to "
                                                "select a track, the "
                                                "process does not start. Defaults to none.";
    capa["optional"]["track_inhibit"]["type"] = "string";
    capa["optional"]["track_inhibit"]["validate"][0u] = "track_selector";
    capa["optional"]["track_inhibit"]["default"] = "audio=none&video=none&subtitle=none";

    capa["required"]["model"]["name"] = "AI Model";
    capa["required"]["model"]["type"] = "select";
    capa["required"]["model"]["default"] = "yolo26n";
    capa["required"]["model"]["help"] =
      "Select a pre-trained model. Missing verified model packs are downloaded automatically on first use; "
      "the release default is YOLO26n and does not require Python on the server.";
    {
      const auto & models = ONNX::ModelRegistry::getAvailableModels();
      size_t idx = 0;
      for (const auto & m : models) {
        capa["required"]["model"]["select"][idx][0u] = m.id;
        capa["required"]["model"]["select"][idx][1u] = m.label;
        idx++;
      }
      capa["required"]["model"]["select"][idx][0u] = "custom";
      capa["required"]["model"]["select"][idx][1u] = "Custom model path";
    }

    // ---- Options are grouped for the web UI (native capa "group" type; groups are flattened
    // on save so the stored keys are unchanged). Sort keys ("a".."f" on groups, "a".. within)
    // order them ahead of the generic mask/selector fields and the "general process options"
    // group. Vision-only and audio-only groups are named accordingly (a static capabilities
    // dump can't know which modality the chosen model is).

    // -- Group: Model --
    {
      JSON::Value & g = capa["optional"]["grp_model"];
      g["name"] = "Model";
      g["help"] = "Model source and type. The main model is selected above.";
      g["type"] = "group";
      g["sort"] = "a";
      g["expand"] = true;
      JSON::Value & o = g["options"];

      o["input_mode"]["name"] = "Input mode";
      o["input_mode"]["type"] = "select";
      o["input_mode"]["default"] = "auto";
      o["input_mode"]["sort"] = "0";
      o["input_mode"]["select"][0u][0u] = "auto";
      o["input_mode"]["select"][0u][1u] = "Curated adapter (automatic)";
      o["input_mode"]["select"][1u][0u] = "tensor";
      o["input_mode"]["select"][1u][1u] = "Raw ONNXTENSOR";
      o["input_mode"]["help"] =
        "Auto uses vision/audio preprocessing and result adapters. Tensor runs arbitrary model I/O without media assumptions.";

      o["tensor_queue_depth"]["name"] = "Tensor queue depth";
      o["tensor_queue_depth"]["type"] = "uint";
      o["tensor_queue_depth"]["default"] = 8;
      o["tensor_queue_depth"]["sort"] = "01";
      o["tensor_queue_depth"]["help"] =
        "Bounded per-direction FIFO depth for raw ONNXTENSOR mode (1-64). Oldest packets are dropped when inference falls behind.";

      o["model_path"]["name"] = "Custom model path";
      o["model_path"]["type"] = "string";
      o["model_path"]["default"] = "";
      o["model_path"]["sort"] = "a";
      o["model_path"]["help"] =
        "Filesystem path to a custom ONNX model. Only used when 'Custom model path' is selected above.";

      o["model_type"]["name"] = "Model type override";
      o["model_type"]["type"] = "select";
      o["model_type"]["sort"] = "b";
      o["model_type"]["help"] = "Override auto-detected model type. Use 'auto' for auto-detection. (Vision models only.)";
      o["model_type"]["default"] = "auto";
      o["model_type"]["select"][0u][0u] = "auto";
      o["model_type"]["select"][0u][1u] = "Auto-detect";
      o["model_type"]["select"][1u][0u] = "detection";
      o["model_type"]["select"][1u][1u] = "Detection (YOLOv8/YOLO11/YOLO26)";
      o["model_type"]["select"][2u][0u] = "pose";
      o["model_type"]["select"][2u][1u] = "Pose estimation";
      o["model_type"]["select"][3u][0u] = "segmentation";
      o["model_type"]["select"][3u][1u] = "Instance segmentation";
      o["model_type"]["select"][4u][0u] = "classification";
      o["model_type"]["select"][4u][1u] = "Classification";
      o["model_type"]["select"][5u][0u] = "obb";
      o["model_type"]["select"][5u][1u] = "Oriented bounding box";
      o["model_type"]["select"][6u][0u] = "rt-detr";
      o["model_type"]["select"][6u][1u] = "RT-DETR (NMS-free detection)";
      o["model_type"]["select"][7u][0u] = "depth";
      o["model_type"]["select"][7u][1u] = "Depth estimation";
      o["model_type"]["select"][8u][0u] = "scrfd";
      o["model_type"]["select"][8u][1u] = "Face detection (SCRFD)";
      o["model_type"]["select"][9u][0u] = "arcface";
      o["model_type"]["select"][9u][1u] = "Face recognition (ArcFace)";
      o["model_type"]["select"][10u][0u] = "rtmo";
      o["model_type"]["select"][10u][1u] = "RTMO pose estimation";
      o["model_type"]["select"][11u][0u] = "sam2-encoder";
      o["model_type"]["select"][11u][1u] = "SAM2 image encoder";
      o["model_type"]["select"][12u][0u] = "sam2-decoder";
      o["model_type"]["select"][12u][1u] = "SAM2 mask decoder";
      o["model_type"]["select"][13u][0u] = "yolo-nms";
      o["model_type"]["select"][13u][1u] = "YOLO with baked-in NMS";
      o["model_type"]["select"][14u][0u] = "embedding";
      o["model_type"]["select"][14u][1u] = "Image embedding (CLIP etc)";

      o["input_size"]["name"] = "Input image size";
      o["input_size"]["type"] = "uint";
      o["input_size"]["default"] = 640;
      o["input_size"]["sort"] = "c";
      o["input_size"]["help"] =
        "Input image size for vision models (64-4096). YOLO-family models want a multiple of 32 "
        "(usually 640/960/1280); ViT/CLIP-style models have a fixed native size (e.g. 224). Leave "
        "unset to use the model's registry or preprocessor-sidecar native size.";

      o["top_k"]["name"] = "Classification top-K";
      o["top_k"]["type"] = "uint";
      o["top_k"]["default"] = 5;
      o["top_k"]["sort"] = "ca";
      o["top_k"]["help"] =
        "Classification models: number of ranked classes reported in the metadata 'top' array "
        "(1 = winner only).";

      o["classifier_mode"]["name"] = "Classifier output mode";
      o["classifier_mode"]["type"] = "select";
      o["classifier_mode"]["default"] = "softmax";
      o["classifier_mode"]["sort"] = "cb";
      o["classifier_mode"]["select"][0u][0u] = "softmax";
      o["classifier_mode"]["select"][0u][1u] = "Softmax (mutually exclusive classes)";
      o["classifier_mode"]["select"][1u][0u] = "sigmoid";
      o["classifier_mode"]["select"][1u][1u] = "Sigmoid (independent multi-label scores)";
      o["classifier_mode"]["select"][2u][0u] = "raw";
      o["classifier_mode"]["select"][2u][1u] = "Raw values (regression heads)";
      o["classifier_mode"]["help"] =
        "How a classification model's output values become confidences. Softmax fits normal "
        "classifiers; sigmoid fits multi-label taggers; raw reports head values unchanged.";
    }

    // -- Group: Runtime / performance --
    {
      JSON::Value & g = capa["optional"]["grp_runtime"];
      g["name"] = "Runtime / performance";
      g["help"] = "Execution backend and threading. Applies to both vision and audio models.";
      g["type"] = "group";
      g["sort"] = "b";
      g["expand"] = true;
      JSON::Value & o = g["options"];

      o["execution_provider"]["name"] = "Execution provider";
      o["execution_provider"]["type"] = "select";
      o["execution_provider"]["sort"] = "a";
      const std::string buildProfile = packagedONNXProfile();
      o["execution_provider"]["help"] = "This binary packages the '" + buildProfile +
        "' profile. Auto uses its accelerator and falls back to CPU; explicit choices are limited to providers in the package.";
      o["execution_provider"]["default"] = "";
      o["execution_provider"]["select"][0u][0u] = "";
      o["execution_provider"]["select"][0u][1u] = "Auto (best available)";
      o["execution_provider"]["select"][1u][0u] = "cpu";
      o["execution_provider"]["select"][1u][1u] = "CPU only";
      size_t epIndex = 2;
      if (buildProfile == "cuda" || buildProfile == "tensorrt") {
        o["execution_provider"]["select"][(unsigned int)epIndex][0u] = "cuda";
        o["execution_provider"]["select"][(unsigned int)epIndex++][1u] = "NVIDIA CUDA";
      }
      if (buildProfile == "tensorrt") {
        o["execution_provider"]["select"][(unsigned int)epIndex][0u] = "tensorrt";
        o["execution_provider"]["select"][(unsigned int)epIndex++][1u] = "NVIDIA TensorRT";
      } else if (buildProfile == "coreml") {
        o["execution_provider"]["select"][(unsigned int)epIndex][0u] = "coreml";
        o["execution_provider"]["select"][(unsigned int)epIndex++][1u] = "Apple CoreML";
      } else if (buildProfile == "openvino") {
        o["execution_provider"]["select"][(unsigned int)epIndex][0u] = "openvino";
        o["execution_provider"]["select"][(unsigned int)epIndex++][1u] = "Intel OpenVINO";
      }

      o["threads"]["name"] = "Inference threads";
      o["threads"]["type"] = "uint";
      o["threads"]["default"] = 1;
      o["threads"]["sort"] = "b";
      o["threads"]["help"] =
        "Intra-op threads per model (1-32). One process runs per stream, so keep this modest to "
        "avoid oversubscribing a host that runs many streams.";

      o["low_latency"]["name"] = "Low-latency mode";
      o["low_latency"]["type"] = "bool";
      o["low_latency"]["default"] = false;
      o["low_latency"]["sort"] = "c";
      o["low_latency"]["help"] =
        "Keep inference threads hot-spinning for lower per-call latency. Costs a busy CPU core per "
        "model — only worth it for a single latency-critical stream, not a busy multi-stream host.";

      o["realtime_priority"]["name"] = "Elevated process priority";
      o["realtime_priority"]["type"] = "bool";
      o["realtime_priority"]["default"] = false;
      o["realtime_priority"]["sort"] = "d";
      o["realtime_priority"]["help"] =
        "Opt in to elevated OS scheduling priority. Leave disabled on shared and multi-stream hosts.";
    }

    // -- Group: Detection & preprocessing (vision models) --
    {
      JSON::Value & g = capa["optional"]["grp_detection"];
      g["name"] = "Detection & preprocessing (vision models)";
      g["help"] = "Thresholds and image preprocessing for vision models. Ignored by audio models.";
      g["type"] = "group";
      g["sort"] = "c";
      JSON::Value & o = g["options"];

      o["conf_threshold"]["name"] = "Confidence threshold";
      o["conf_threshold"]["type"] = "float";
      o["conf_threshold"]["default"] = "0.5";
      o["conf_threshold"]["sort"] = "a";
      o["conf_threshold"]["help"] = "Minimum confidence score for object detections (0.0-1.0)";

      o["nms_threshold"]["name"] = "Soft-NMS DIoU threshold";
      o["nms_threshold"]["type"] = "float";
      o["nms_threshold"]["default"] = "0.4";
      o["nms_threshold"]["sort"] = "b";
      o["nms_threshold"]["help"] =
        "DIoU threshold for Soft-NMS to decay overlapping boxes (0.0-0.8). Uses distance-aware IoU "
        "and Gaussian decay instead of hard suppression. Ignored by native NMS-free YOLO26 and "
        "models with baked-in NMS.";

      o["soft_nms_sigma"]["name"] = "Soft-NMS sigma parameter";
      o["soft_nms_sigma"]["type"] = "float";
      o["soft_nms_sigma"]["default"] = "0.5";
      o["soft_nms_sigma"]["sort"] = "c";
      o["soft_nms_sigma"]["help"] =
        "Controls Gaussian decay rate in Soft-NMS (0.1-1.0). Lower values = more aggressive decay, "
        "higher values = gentler decay. Ignored by native NMS-free YOLO26 and models with baked-in NMS.";

      o["letterbox"]["name"] = "Letterbox preprocessing";
      o["letterbox"]["type"] = "bool";
      o["letterbox"]["default"] = true;
      o["letterbox"]["sort"] = "d";
      o["letterbox"]["help"] =
        "Preserve aspect ratio during resize by padding with gray. Disable for legacy stretch "
        "behavior. Overridden by 'Resize mode override' when that is set.";

      o["resize_mode"]["name"] = "Resize mode override";
      o["resize_mode"]["type"] = "select";
      o["resize_mode"]["sort"] = "e";
      o["resize_mode"]["help"] =
        "Override the model's default resize mode. Leave empty for auto. Takes precedence over the "
        "'Letterbox preprocessing' toggle above.";
      o["resize_mode"]["default"] = "";
      o["resize_mode"]["select"][0u][0u] = "";
      o["resize_mode"]["select"][0u][1u] = "Auto (model default)";
      o["resize_mode"]["select"][1u][0u] = "letterbox";
      o["resize_mode"]["select"][1u][1u] = "Letterbox (aspect-preserving)";
      o["resize_mode"]["select"][2u][0u] = "direct";
      o["resize_mode"]["select"][2u][1u] = "Direct resize (stretch)";

      o["normalization"]["name"] = "Normalization override";
      o["normalization"]["type"] = "select";
      o["normalization"]["sort"] = "f";
      o["normalization"]["help"] = "Override the model's default normalization. Leave empty for auto.";
      o["normalization"]["default"] = "";
      o["normalization"]["select"][0u][0u] = "";
      o["normalization"]["select"][0u][1u] = "Auto (model default)";
      o["normalization"]["select"][1u][0u] = "scale01";
      o["normalization"]["select"][1u][1u] = "Scale to [0,1] (/255)";
      o["normalization"]["select"][2u][0u] = "imagenet";
      o["normalization"]["select"][2u][1u] = "ImageNet mean/std";
      o["normalization"]["select"][3u][0u] = "scrfd";
      o["normalization"]["select"][3u][1u] = "SCRFD ((x-127.5)/128)";

      o["enhance_image"]["name"] = "Enable image enhancement";
      o["enhance_image"]["type"] = "bool";
      o["enhance_image"]["default"] = false;
      o["enhance_image"]["sort"] = "g";
      o["enhance_image"]["help"] = "Apply histogram equalization for better contrast before inference";

      o["process_every_nth"]["name"] = "Process every Nth frame";
      o["process_every_nth"]["type"] = "uint";
      o["process_every_nth"]["default"] = 1;
      o["process_every_nth"]["sort"] = "h";
      o["process_every_nth"]["help"] =
        "Only run inference on every Nth frame (1 = every frame, 2 = every other, etc). "
        "Reduces CPU load at the cost of detection latency";

      o["max_inference_fps"]["name"] = "Maximum inference FPS";
      o["max_inference_fps"]["type"] = "float";
      o["max_inference_fps"]["default"] = 5.0;
      o["max_inference_fps"]["sort"] = "ha";
      o["max_inference_fps"]["help"] =
        "Timestamp-based inference rate cap (0 = unlimited). Five FPS is a useful CPU default for analytics.";

      o["annotated_video"]["name"] = "Emit annotated video";
      o["annotated_video"]["type"] = "bool";
      o["annotated_video"]["default"] = false;
      o["annotated_video"]["sort"] = "hb";
      o["annotated_video"]["help"] =
        "Draw results and emit an MJPEG track. Disabled by default to avoid drawing and JPEG overhead.";

      o["jpeg_quality"]["name"] = "Output JPEG quality";
      o["jpeg_quality"]["type"] = "uint";
      o["jpeg_quality"]["default"] = 80;
      o["jpeg_quality"]["sort"] = "i";
      o["jpeg_quality"]["help"] = "Quality (1-100) of the annotated MJPEG output track.";

      o["secondary_model_path"]["name"] = "Secondary model path";
      o["secondary_model_path"]["type"] = "string";
      o["secondary_model_path"]["default"] = "";
      o["secondary_model_path"]["sort"] = "j";
      o["secondary_model_path"]["help"] =
        "Secondary ONNX model for chaining (e.g., ArcFace after SCRFD face detection), run on each "
        "primary detection crop. Accepts a registry model id (auto-provisioned, carries its type) "
        "or a file path.";

      o["secondary_model_type"]["name"] = "Secondary model type";
      o["secondary_model_type"]["type"] = "select";
      o["secondary_model_type"]["default"] = "auto";
      o["secondary_model_type"]["sort"] = "ja";
      o["secondary_model_type"]["select"][0u][0u] = "auto";
      o["secondary_model_type"]["select"][0u][1u] = "Auto (registry type / shape detection)";
      o["secondary_model_type"]["select"][1u][0u] = "classification";
      o["secondary_model_type"]["select"][1u][1u] = "Classification";
      o["secondary_model_type"]["select"][2u][0u] = "embedding";
      o["secondary_model_type"]["select"][2u][1u] = "Image embedding (CLIP etc)";
      o["secondary_model_type"]["select"][3u][0u] = "arcface";
      o["secondary_model_type"]["select"][3u][1u] = "Face recognition (ArcFace)";
      o["secondary_model_type"]["select"][4u][0u] = "detection";
      o["secondary_model_type"]["select"][4u][1u] = "Detection";
      o["secondary_model_type"]["select"][5u][0u] = "age-gender";
      o["secondary_model_type"]["select"][5u][1u] = "Face age/gender";
      o["secondary_model_type"]["help"] =
        "Type override for a bare-path secondary model. Embedding heads ([1,D]) cannot be "
        "shape-detected and would misroute to classification without this (or a registry id).";

      o["emit_embeddings"]["name"] = "Embed vectors in detections";
      o["emit_embeddings"]["type"] = "bool";
      o["emit_embeddings"]["default"] = false;
      o["emit_embeddings"]["sort"] = "jb";
      o["emit_embeddings"]["help"] =
        "Include the full embedding vector in each chained detection's metadata (needed for "
        "face/image matching downstream; ~4-8 KB of JSON per detection). Disable to emit only "
        "embedding_dim and confidence.";

      o["gender_high_label"]["name"] = "Gender label (prob >= 0.5)";
      o["gender_high_label"]["type"] = "string";
      o["gender_high_label"]["default"] = "female";
      o["gender_high_label"]["sort"] = "jc";
      o["gender_high_label"]["help"] =
        "age-gender model only: label emitted when the gender probability is >= 0.5 (UTKFace "
        "convention makes this 'female'). Swap with the low label if a labeled sample shows the "
        "polarity inverted.";

      o["gender_low_label"]["name"] = "Gender label (prob < 0.5)";
      o["gender_low_label"]["type"] = "string";
      o["gender_low_label"]["default"] = "male";
      o["gender_low_label"]["sort"] = "jd";
      o["gender_low_label"]["help"] = "age-gender model only: label emitted when gender probability is < 0.5.";
    }

    // -- Group: Tracking (vision models) --
    {
      JSON::Value & g = capa["optional"]["grp_tracking"];
      g["name"] = "Tracking (vision models)";
      g["help"] = "Temporal tracking, scene-change reset and Kalman smoothing. Ignored by audio models.";
      g["type"] = "group";
      g["sort"] = "d";
      JSON::Value & o = g["options"];

      o["enable_tracking"]["name"] = "Enable temporal tracking";
      o["enable_tracking"]["type"] = "bool";
      o["enable_tracking"]["default"] = false;
      o["enable_tracking"]["sort"] = "0";
      o["enable_tracking"]["help"] = "Assign stable object IDs across frames. Disabled for stateless metadata by default.";

      o["min_consecutive_ms"]["name"] = "Minimum consecutive milliseconds";
      o["min_consecutive_ms"]["type"] = "uint";
      o["min_consecutive_ms"]["default"] = 200;
      o["min_consecutive_ms"]["sort"] = "a";
      o["min_consecutive_ms"]["help"] = "Minimum milliseconds before a detection appears (reduces flickering)";

      o["max_missing_ms"]["name"] = "Maximum missing milliseconds";
      o["max_missing_ms"]["type"] = "uint";
      o["max_missing_ms"]["default"] = 800;
      o["max_missing_ms"]["sort"] = "b";
      o["max_missing_ms"]["help"] =
        "Maximum milliseconds a track can be missing before being removed (increases persistence)";

      o["tracking_iou_threshold"]["name"] = "Tracking IoU threshold";
      o["tracking_iou_threshold"]["type"] = "float";
      o["tracking_iou_threshold"]["default"] = "0.3";
      o["tracking_iou_threshold"]["sort"] = "c";
      o["tracking_iou_threshold"]["help"] = "IoU threshold for matching detections across frames (0.1-0.8)";

      o["enable_scene_change_detection"]["name"] = "Enable scene change detection";
      o["enable_scene_change_detection"]["type"] = "bool";
      o["enable_scene_change_detection"]["default"] = false;
      o["enable_scene_change_detection"]["sort"] = "d";
      o["enable_scene_change_detection"]["help"] =
        "Automatically reset tracker on major scene changes (camera movement, cuts)";

      o["scene_change_threshold"]["name"] = "Scene change threshold";
      o["scene_change_threshold"]["type"] = "float";
      o["scene_change_threshold"]["default"] = "0.85";
      o["scene_change_threshold"]["sort"] = "e";
      o["scene_change_threshold"]["help"] = "Threshold for scene change detection (0.0-1.0)";

      o["enable_kalman_filter"]["name"] = "Enable Kalman filter tracking";
      o["enable_kalman_filter"]["type"] = "bool";
      o["enable_kalman_filter"]["default"] = false;
      o["enable_kalman_filter"]["sort"] = "f";
      o["enable_kalman_filter"]["help"] =
        "Use 6-state Kalman filter for robust tracking with smooth motion prediction";

      o["kalman_process_noise"]["name"] = "Kalman process noise";
      o["kalman_process_noise"]["type"] = "float";
      o["kalman_process_noise"]["default"] = "0.01";
      o["kalman_process_noise"]["sort"] = "g";
      o["kalman_process_noise"]["help"] =
        "Process noise for Kalman filter (0.001-0.1). Lower = smoother but less responsive";

      o["kalman_measurement_noise"]["name"] = "Kalman measurement noise";
      o["kalman_measurement_noise"]["type"] = "float";
      o["kalman_measurement_noise"]["default"] = "0.1";
      o["kalman_measurement_noise"]["sort"] = "h";
      o["kalman_measurement_noise"]["help"] =
        "Measurement noise for Kalman filter (0.01-1.0). Lower = trust measurements more";
    }

    // -- Group: Transcription (audio models) --
    {
      JSON::Value & g = capa["optional"]["grp_transcription"];
      g["name"] = "Transcription (audio models)";
      g["help"] =
        "Speech-to-text chunking. Audio is cut at speech pauses to avoid splitting words; these "
        "bound the chunk length. Ignored by vision models.";
      g["type"] = "group";
      g["sort"] = "e";
      JSON::Value & o = g["options"];

      o["window_seconds"]["name"] = "Target chunk (s)";
      o["window_seconds"]["type"] = "uint";
      o["window_seconds"]["default"] = 5;
      o["window_seconds"]["sort"] = "a";
      o["window_seconds"]["help"] =
        "Target audio chunk length in seconds (1-120). The chunk is cut at the nearest speech pause "
        "once this is reached. Larger gives more context and better accuracy but higher latency; "
        "lower it for live/low-latency use.";

      o["max_window_seconds"]["name"] = "Max chunk (s)";
      o["max_window_seconds"]["type"] = "uint";
      o["max_window_seconds"]["default"] = 10;
      o["max_window_seconds"]["sort"] = "b";
      o["max_window_seconds"]["help"] =
        "Hard upper bound on chunk length: a cut is forced here even if no pause is found. "
        "Defaults to 1.5x the target.";

      o["min_window_seconds"]["name"] = "Min chunk (s)";
      o["min_window_seconds"]["type"] = "float";
      o["min_window_seconds"]["default"] = 1.5;
      o["min_window_seconds"]["sort"] = "c";
      o["min_window_seconds"]["help"] =
        "Shortest chunk length: a chunk is never cut at a pause before this much audio has "
        "accumulated (the pause-duration threshold itself is fixed at 400 ms). Defaults to 0.4x the target.";

      o["max_buffer_seconds"]["name"] = "Max buffered audio (s)";
      o["max_buffer_seconds"]["type"] = "uint";
      o["max_buffer_seconds"]["default"] = 30;
      o["max_buffer_seconds"]["sort"] = "d";
      o["max_buffer_seconds"]["help"] =
        "Backpressure ceiling: if inference falls behind realtime and the backlog exceeds this, the "
        "oldest audio is dropped to bound memory and latency. Defaults to 3x the max chunk.";

    }

    // -- Group: Event detection --
    {
      JSON::Value & g = capa["optional"]["grp_events"];
      g["name"] = "Event detection";
      g["help"] =
        "Turn noisy per-frame/per-chunk scores into started/ended events with smoothing and "
        "hysteresis. Used by voice-activity models (speech events) and, with a class name set "
        "below, by vision classification models (e.g. nsfw started/ended).";
      g["type"] = "group";
      g["sort"] = "f";
      JSON::Value & o = g["options"];

      o["event_class"]["name"] = "Vision event class";
      o["event_class"]["type"] = "string";
      o["event_class"]["default"] = "";
      o["event_class"]["sort"] = "a";
      o["event_class"]["help"] =
        "Classification models only: class name to track as an event (e.g. 'nsfw'). Every metadata "
        "packet gains an event_state object; transitions are marked with changed=started/ended. "
        "Exact for two-class models; for multi-class models the score falls back to the complement "
        "of the winning class when the tracked class is outside the reported top-K.";

      o["event_enter"]["name"] = "Event start threshold";
      o["event_enter"]["type"] = "float";
      o["event_enter"]["default"] = 0.5;
      o["event_enter"]["sort"] = "b";
      o["event_enter"]["help"] = "Smoothed score at which the event starts (speech / tracked class).";

      o["event_exit"]["name"] = "Event end threshold";
      o["event_exit"]["type"] = "float";
      o["event_exit"]["default"] = 0.35;
      o["event_exit"]["sort"] = "c";
      o["event_exit"]["help"] =
        "Smoothed score below which the event ends. Must be below the start threshold "
        "(hysteresis prevents event flapping).";

      o["event_min_ms"]["name"] = "Min event duration (ms)";
      o["event_min_ms"]["type"] = "uint";
      o["event_min_ms"]["default"] = 250;
      o["event_min_ms"]["sort"] = "d";
      o["event_min_ms"]["help"] = "An active event younger than this cannot end (debounce).";

      o["event_ema"]["name"] = "Score smoothing (EMA)";
      o["event_ema"]["type"] = "float";
      o["event_ema"]["default"] = 0.3;
      o["event_ema"]["sort"] = "e";
      o["event_ema"]["help"] =
        "Weight of each NEW score in the exponential moving average, 0-1. Lower = smoother and "
        "slower to react; 1 disables smoothing (thresholds act on raw scores).";
    }

    // -- Group: Output tracks --
    {
      JSON::Value & g = capa["optional"]["grp_output"];
      g["name"] = "Output tracks";
      g["help"] = "Stable identity for resumable result and optional annotation tracks.";
      g["type"] = "group";
      g["sort"] = "f";
      JSON::Value & o = g["options"];

      o["output_id"]["name"] = "Output identity";
      o["output_id"]["type"] = "string";
      o["output_id"]["default"] = "default";
      o["output_id"]["sort"] = "a";
      o["output_id"]["help"] =
        "Stable identity embedded in track init data. Change it to intentionally create a distinct output.";
    }

    std::cout << capa.toString() << std::endl;
    return -1;
  }

  Util::redirectLogsIfNeeded();

  if (config.getString("configuration") != "-") {
    opt = JSON::fromString(config.getString("configuration"));
  } else {
    std::string json, line;
    while (std::getline(std::cin, line)) json += line;
    opt = JSON::fromString(json);
  }

  // Parse every option once (validate + clamp) into the config globals. CheckConfig and the
  // rest of main() consume those globals and never re-read `opt`.
  parseConfig();

  // Check configuration
  Mist::ProcONNX proc;
  if (!proc.CheckConfig()) {
    FAIL_MSG("Configuration check failed");
    return 1;
  }

  // Resolve model path from dropdown or direct path
  std::string modelPath;
  std::string modelChoice;

  if (opt.isMember("model") && opt["model"].isString() && !opt["model"].asString().empty()) {
    modelChoice = opt["model"].asString();
    if (modelChoice == "custom") {
      if (!opt.isMember("model_path") || opt["model_path"].asString().empty()) {
        FAIL_MSG("Custom model selected but no model_path provided");
        return 1;
      }
      modelPath = opt["model_path"].asString();
    } else {
      // Multi-file bundles (audio ASR, OCR) resolve as a set later; only single-file
      // vision models resolve to a path here. A bundle entry has an empty filename.
      const ONNX::ModelRegistryEntry *reg = ONNX::ModelRegistry::findModel(modelChoice);
      bool isMultiFile = reg && (!reg->filename || !reg->filename[0]);
      if (reg && reg->modality == ONNX::ModelModality::AUDIO && activeModality != ONNX::ModelModality::TENSOR) {
        activeModality = ONNX::ModelModality::AUDIO;
        modelPath = modelChoice; // placeholder; the bundle is resolved at model creation
      } else if (isMultiFile) {
        modelPath = modelChoice; // OCR (vision bundle) — resolved at model creation
      } else {
        modelPath = ONNX::ModelRegistry::resolveModelPath(modelChoice);
        if (modelPath.empty()) {
          // Not on disk yet — auto-provision by running the bundled script, then retry.
          std::string hint;
          if (ONNX::ModelRegistry::provision(modelChoice, hint)) {
            modelPath = ONNX::ModelRegistry::resolveModelPath(modelChoice);
          }
          if (modelPath.empty()) {
            FAIL_MSG("Model '%s' could not be provisioned. %s", modelChoice.c_str(),
                     hint.empty() ? "Run: scripts/ONNX/prepare_models.sh <id>" : hint.c_str());
            return 1;
          }
        }
      }
    }
  } else if (opt.isMember("model_path") && !opt["model_path"].asString().empty()) {
    modelPath = opt["model_path"].asString();
  } else {
    FAIL_MSG("No model specified");
    return 1;
  }

  std::string streamName = opt["sink"].asString();
  if (!streamName.size()) { streamName = opt["source"].asStringRef(); }
  Util::streamVariables(modelPath, streamName, opt["source"].asStringRef());

  INFO_MSG("Initializing ONNX model: %s", modelPath.c_str());

  // numThreads / epChoice / all other options come from parseConfig() (globals above).

  // Elevated scheduling can starve other stream workers. It is explicit opt-in.
  if (realtimePriority) {
    INFO_MSG("Applying opt-in elevated scheduling priority");
#ifdef __APPLE__
    if (setpriority(PRIO_PROCESS, 0, -10) == 0) {
      INFO_MSG("Set process priority to high (-10) for consistent timing");
    } else {
      WARN_MSG("Failed to set high process priority: %s", strerror(errno));
    }
#elif __linux__
    struct sched_param param;
    param.sched_priority = 50;
    if (sched_setscheduler(0, SCHED_FIFO, &param) == 0) {
      INFO_MSG("Set real-time scheduling (SCHED_FIFO, priority 50) for consistent timing");
    } else {
      WARN_MSG("Failed to set real-time scheduling: %s", strerror(errno));
      if (setpriority(PRIO_PROCESS, 0, -10) == 0) { INFO_MSG("Set process priority to high (-10) as fallback"); }
    }
#endif
  }

  // Parse model_type override
  ONNX::ModelType modelTypeOverride = ONNX::ModelType::UNKNOWN;
  if (opt.isMember("model_type") && opt["model_type"].isString()) {
    std::string mt = opt["model_type"].asString();
    if (mt == "detection") { modelTypeOverride = ONNX::ModelType::YOLOV8_DETECTION; }
    else if (mt == "pose") { modelTypeOverride = ONNX::ModelType::YOLOV8_POSE; }
    else if (mt == "segmentation") { modelTypeOverride = ONNX::ModelType::YOLOV8_SEGMENTATION; }
    else if (mt == "classification") { modelTypeOverride = ONNX::ModelType::YOLOV8_CLASSIFICATION; }
    else if (mt == "obb") { modelTypeOverride = ONNX::ModelType::YOLOV8_OBB; }
    else if (mt == "rt-detr" || mt == "rtdetr") { modelTypeOverride = ONNX::ModelType::RT_DETR_DETECTION; }
    else if (mt == "depth") { modelTypeOverride = ONNX::ModelType::DEPTH_ESTIMATION; }
    else if (mt == "face-detection" || mt == "scrfd") { modelTypeOverride = ONNX::ModelType::FACE_DETECTION_SCRFD; }
    else if (mt == "face-recognition" || mt == "arcface") { modelTypeOverride = ONNX::ModelType::FACE_RECOGNITION_ARCFACE; }
    else if (mt == "embedding") { modelTypeOverride = ONNX::ModelType::IMAGE_EMBEDDING; }
    else if (mt == "rtmo") { modelTypeOverride = ONNX::ModelType::POSE_RTMO; }
    else if (mt == "sam2-encoder") { modelTypeOverride = ONNX::ModelType::SAM2_ENCODER; }
    else if (mt == "sam2-decoder") { modelTypeOverride = ONNX::ModelType::SAM2_DECODER; }
    else if (mt == "yolo-nms") { modelTypeOverride = ONNX::ModelType::YOLO_NMS_DETECTION; }
    else if (mt != "auto" && !mt.empty()) { WARN_MSG("Unknown model_type '%s', using auto-detection", mt.c_str()); }
    if (modelTypeOverride != ONNX::ModelType::UNKNOWN) {
      INFO_MSG("Using model_type override: %s", mt.c_str());
    }
  }

  // Auto-set inputSize and modelTypeOverride from registry when using dropdown (vision only;
  // input_size/model type are meaningless for audio bundles).
  bool registryProvidedSize = false;
  if (activeModality == ONNX::ModelModality::VISION && !modelChoice.empty() && modelChoice != "custom") {
    for (const auto & entry : ONNX::ModelRegistry::getAvailableModels()) {
      if (modelChoice == entry.id) {
        if (!opt.isMember("input_size")) {
          inputSize = entry.defaultInputSize;
          registryProvidedSize = true;
          INFO_MSG("Using model default input_size: %d", inputSize);
        }
        if (modelTypeOverride == ONNX::ModelType::UNKNOWN) {
          modelTypeOverride = entry.type;
        }
        break;
      }
    }
  }


  // Single-file audio models (VAD / classification / tagging / embedding) go through
  // the generic AudioModel adapter; multi-file bundles stay on the ASR path below.
  const ONNX::ModelRegistryEntry *audioEntry =
    (activeModality == ONNX::ModelModality::AUDIO) ? ONNX::ModelRegistry::findModel(modelChoice) : 0;
  const bool singleFileAudio = audioEntry && audioEntry->filename && audioEntry->filename[0];
  // OCR is a VISION multi-file bundle (det + rec + charset), resolved via resolveOCRSet.
  const ONNX::ModelRegistryEntry *visionEntry =
    (activeModality == ONNX::ModelModality::VISION) ? ONNX::ModelRegistry::findModel(modelChoice) : 0;
  const bool isOCR = visionEntry && visionEntry->type == ONNX::ModelType::OCR;

  try {
    if (activeModality == ONNX::ModelModality::TENSOR) {
      tensorModel.reset(new ONNX::SessionRunner());
      std::string err;
      if (!tensorModel->load(modelPath, numThreads, epChoice, err, lowLatency)) {
        FAIL_MSG("Failed to create generic tensor session from %s: %s", modelPath.c_str(), err.c_str());
        return 1;
      }
      INFO_MSG("Generic ONNXTENSOR session ready: %zu input(s), %zu output(s), EP=%s",
               tensorModel->numInputs(), tensorModel->numOutputs(), tensorModel->activeEP().c_str());
    } else if (isOCR) {
      ONNX::OCRBundle bundle = ONNX::ModelRegistry::resolveOCRSet(modelChoice);
      if (!bundle.ok) {
        std::string hint;
        if (ONNX::ModelRegistry::provision(modelChoice, hint)) {
          bundle = ONNX::ModelRegistry::resolveOCRSet(modelChoice);
        }
        if (!bundle.ok) {
          FAIL_MSG("OCR model '%s' could not be provisioned. %s", modelChoice.c_str(),
                   hint.empty() ? "Run: scripts/ONNX/prepare_models.sh <id>" : hint.c_str());
          return 1;
        }
      }
      onnxModel = ONNX::ModelFactory::createOCRModel(bundle, numThreads, epChoice, lowLatency);
      if (!onnxModel) {
        FAIL_MSG("Failed to create OCR model '%s'", modelChoice.c_str());
        return 1;
      }
      INFO_MSG("OCR model ready: %s", modelChoice.c_str());
    } else if (activeModality == ONNX::ModelModality::AUDIO && singleFileAudio) {
      std::string audioPath = ONNX::ModelRegistry::resolveModelPath(modelChoice);
      if (audioPath.empty()) {
        // Not on disk yet — auto-provision by running the bundled script, then retry.
        std::string hint;
        if (ONNX::ModelRegistry::provision(modelChoice, hint)) {
          audioPath = ONNX::ModelRegistry::resolveModelPath(modelChoice);
        }
        if (audioPath.empty()) {
          FAIL_MSG("Audio model '%s' could not be provisioned. %s", modelChoice.c_str(),
                   hint.empty() ? "Run: scripts/ONNX/prepare_models.sh <id>" : hint.c_str());
          return 1;
        }
      }
      audioModel = ONNX::ModelFactory::createAudioModel(audioPath, audioEntry->type, numThreads, epChoice, lowLatency);
      if (!audioModel) {
        FAIL_MSG("Failed to create audio model '%s'", modelChoice.c_str());
        return 1;
      }
      // The windower buffers/timestamps PCM for both policies; fixed-chunk models pop
      // exact chunks via takeFixed and ignore the pause-windowing parameters.
      // Spectrogram models with a fixed frame count (AST: 1024 frames = 10 ms each)
      // can't use more audio than that per window — clamp the chunking so we don't
      // buffer audio whose features would just be truncated away.
      double tgtSec = windowTargetSec, maxSec = windowMaxSec, minSec = windowMinSec;
      if (audioModel->config().fbankBins > 0 && audioModel->config().fixedFrames > 0) {
        double capSec = audioModel->config().fixedFrames * 0.010 + 0.015; // + frame tail
        if (maxSec > capSec) { maxSec = capSec; }
        if (tgtSec > maxSec) { tgtSec = maxSec; }
        if (minSec > tgtSec) { minSec = tgtSec * 0.4; }
        INFO_MSG("Audio window clamped to model capacity: target %.1fs, max %.1fs "
                 "(%d spectrogram frames)", tgtSec, maxSec, audioModel->config().fixedFrames);
      }
      audioWindower.configure((uint64_t)audioModel->sampleRate(), tgtSec, maxSec, minSec, maxBufferSec);
      vadSmoother.configure((float)eventEnter, (float)eventExit, eventEma, eventMinMs);
      INFO_MSG("Audio model ready: %s (rate %d Hz, %s, EP=%s)", modelChoice.c_str(),
               audioModel->sampleRate(),
               audioModel->chunkSamples() > 0 ? "fixed-chunk streaming" : "pause-windowed",
               audioModel->executionProvider().c_str());
      {
        std::lock_guard<std::mutex> guard(statsMutex);
        pData["audio"]["model"] = modelChoice;
        pData["audio"]["provider"] = audioModel->executionProvider();
      }
    } else if (activeModality == ONNX::ModelModality::AUDIO) {
      // Transcription: resolve the multi-file bundle and build the ASR model.
      ONNX::ModelBundle bundle = ONNX::ModelRegistry::resolveModelSet(modelChoice);
      if (!bundle.ok) {
        // Auto-provision the bundle by running the bundled script, then retry.
        std::string hint;
        if (ONNX::ModelRegistry::provision(modelChoice, hint)) {
          bundle = ONNX::ModelRegistry::resolveModelSet(modelChoice);
        }
        if (!bundle.ok) {
          FAIL_MSG("Transcription model '%s' could not be provisioned. %s", modelChoice.c_str(),
                   hint.empty() ? "Run: scripts/ONNX/prepare_models.sh <id>" : hint.c_str());
          return 1;
        }
      }
      // EP default when the user didn't force one: INT8 is a CPU-shaped workload
      // (INT8-on-CUDA is a known pessimisation) so it defaults to CPU; FP16/FP32 fall through
      // to auto-detect so an accelerated EP is used when present. Threads/low_latency come
      // from the shared config (same as vision) — no audio-specific override.
      std::string asrEP = epChoice;
      if (asrEP.empty() && modelChoice.find("int8") != std::string::npos) { asrEP = "cpu"; }
      asrModel = ONNX::ModelFactory::createTranscriptionModel(bundle, numThreads, asrEP, lowLatency);
      if (!asrModel) {
        FAIL_MSG("Failed to create transcription model '%s'", modelChoice.c_str());
        return 1;
      }
      // Streaming ASR chunking (parsed by parseConfig): accumulate to windowTargetSec, cut at
      // the nearest speech pause, bounded by [windowMinSec, windowMaxSec], drop-oldest beyond
      // maxBufferSec.
      audioWindower.configure((uint64_t)asrModel->sampleRate(), windowTargetSec, windowMaxSec, windowMinSec,
                              maxBufferSec);
      INFO_MSG("Transcription model ready: %s (rate %d Hz, chunk target %.1fs, max %.1fs, min %.1fs, "
               "buffer<=%.1fs, EP=%s)",
               modelChoice.c_str(), asrModel->sampleRate(), windowTargetSec, windowMaxSec, windowMinSec,
               maxBufferSec, asrModel->executionProvider().c_str());
      {
        std::lock_guard<std::mutex> guard(statsMutex);
        pData["asr"]["model"] = modelChoice;
        pData["asr"]["provider"] = asrModel->executionProvider();
      }
    } else {
      // No explicit user or registry size: pass 0 so the factory resolves the model's
      // native size from its preprocessor sidecar (a ViT/CLIP at the generic 640
      // default loads but fails inference — fixed position embeddings).
      int createSize = (opt.isMember("input_size") || registryProvidedSize) ? inputSize : 0;
      onnxModel = ONNX::ModelFactory::createModel(modelPath, createSize, numThreads, modelTypeOverride, epChoice, lowLatency);
      if (!onnxModel) {
        FAIL_MSG("Failed to create ONNX model from %s", modelPath.c_str());
        return 1;
      }
      visionSmoother.configure((float)eventEnter, (float)eventExit, eventEma, eventMinMs);
      // Classifier surface: ranked top-K in metadata + output mode (softmax/sigmoid/raw)
      ONNX::YOLOv8ClassificationModel *clsModel =
        dynamic_cast<ONNX::YOLOv8ClassificationModel *>(onnxModel.get());
      if (clsModel) {
        clsModel->setTopK(classifierTopK);
        if (classifierMode == "sigmoid") {
          clsModel->setOutputMode(ONNX::YOLOv8ClassificationModel::SIGMOID);
        } else if (classifierMode == "raw") {
          clsModel->setOutputMode(ONNX::YOLOv8ClassificationModel::RAW);
        }
      }
      // Zero-shot CLIP tags honor the same top_k knob as classifiers.
      ONNX::EmbeddingModel *embModel = dynamic_cast<ONNX::EmbeddingModel *>(onnxModel.get());
      if (embModel) { embModel->setMatchTopK(classifierTopK); }
      if (!eventClass.empty()) {
        INFO_MSG("Event detection: tracking class '%s' (enter %.2f, exit %.2f, min %" PRIu64 "ms)",
                 eventClass.c_str(), eventEnter, eventExit, eventMinMs);
      }

      ONNX::ModelInfo modelInfo = onnxModel->getModelInfo();
      INFO_MSG("ONNX model loaded successfully: %s", modelPath.c_str());
      INFO_MSG("Model type: %s", modelInfo.name.c_str());
      INFO_MSG("Input size: %dx%d, Classes: %d", onnxModel->getInputWidth(), onnxModel->getInputHeight(), modelInfo.numClasses);
      INFO_MSG("Execution provider: %s", onnxModel->getExecutionProvider().c_str());
    }
  } catch (const std::exception & e) {
    FAIL_MSG("Failed to load ONNX model %s: %s", modelPath.c_str(), e.what());
    return 1;
  }

  // Vision-only post-configuration: push the parsed options into the model. Values came from
  // parseConfig(); this only applies them (the model object exists only here).
  if (activeModality == ONNX::ModelModality::VISION) {
    // Secondary model for chaining (e.g. ArcFace after SCRFD detection).
    if (!secondaryModelPath.empty()) {
      std::string secPath = secondaryModelPath;
      Util::streamVariables(secPath, streamName, opt["source"].asStringRef());
      // A registry id resolves to its curated file and carries its type, so
      // override-routed types like IMAGE_EMBEDDING chain correctly (a bare-path
      // [1,D] embedding head would shape-misroute to classification). Bare paths can
      // force a type via secondary_model_type.
      ONNX::ModelType secTypeOverride = ONNX::ModelType::UNKNOWN;
      const ONNX::ModelRegistryEntry *secEntry = ONNX::ModelRegistry::findModel(secPath);
      if (secEntry && secEntry->modality == ONNX::ModelModality::VISION) {
        std::string resolved = ONNX::ModelRegistry::resolveModelPath(secPath);
        if (resolved.empty()) {
          std::string hint;
          if (ONNX::ModelRegistry::provision(secPath, hint)) {
            resolved = ONNX::ModelRegistry::resolveModelPath(secPath);
          }
        }
        if (!resolved.empty()) {
          secTypeOverride = secEntry->type;
          secPath = resolved;
        }
      }
      if (!secondaryModelType.empty() && secondaryModelType != "auto") {
        if (secondaryModelType == "classification") { secTypeOverride = ONNX::ModelType::YOLOV8_CLASSIFICATION; }
        else if (secondaryModelType == "embedding") { secTypeOverride = ONNX::ModelType::IMAGE_EMBEDDING; }
        else if (secondaryModelType == "arcface") { secTypeOverride = ONNX::ModelType::FACE_RECOGNITION_ARCFACE; }
        else if (secondaryModelType == "age-gender") { secTypeOverride = ONNX::ModelType::FACE_ATTRIBUTE; }
        else if (secondaryModelType == "detection") { secTypeOverride = ONNX::ModelType::YOLOV8_DETECTION; }
        else { WARN_MSG("Unknown secondary_model_type '%s', using auto", secondaryModelType.c_str()); }
      }
      try {
        secondaryModel = ONNX::ModelFactory::createModel(secPath, 0, numThreads, secTypeOverride,
                                                         epChoice, lowLatency);
        if (secondaryModel) {
          ONNX::ModelInfo secInfo = secondaryModel->getModelInfo();
          INFO_MSG("Secondary model loaded: %s (%s)", secPath.c_str(), secInfo.name.c_str());
          // Same classifier surface as the primary (ranked top-K + output mode)
          ONNX::YOLOv8ClassificationModel *secCls =
            dynamic_cast<ONNX::YOLOv8ClassificationModel *>(secondaryModel.get());
          // A FACE_ATTRIBUTE secondary is a classifier under the hood but the factory
          // fixed its RAW/top-2 contract (the age/gender decode depends on it) — don't
          // overwrite it with the generic classification knobs.
          if (secCls && secTypeOverride != ONNX::ModelType::FACE_ATTRIBUTE) {
            secCls->setTopK(classifierTopK);
            if (classifierMode == "sigmoid") {
              secCls->setOutputMode(ONNX::YOLOv8ClassificationModel::SIGMOID);
            } else if (classifierMode == "raw") {
              secCls->setOutputMode(ONNX::YOLOv8ClassificationModel::RAW);
            }
          }
        } else {
          WARN_MSG("Failed to create secondary model from %s, continuing without chaining", secPath.c_str());
        }
      } catch (const std::exception & e) {
        WARN_MSG("Failed to load secondary model %s: %s, continuing without chaining", secPath.c_str(), e.what());
      }
    }

    onnxModel->setImageEnhancement(enhanceImage);
    onnxModel->setSoftNmsSigma(softNmsSigma);

    // Preprocessing overrides. Precedence: resize_mode wins over the legacy `letterbox`
    // toggle (both control letterboxing), so letterbox is applied first and resize_mode last.
    if (letterboxOpt != -1) {
      onnxModel->setLetterbox(letterboxOpt == 1);
      INFO_MSG("Letterbox preprocessing: %s", letterboxOpt == 1 ? "enabled" : "disabled");
    }
    if (!normalizationMode.empty()) {
      ONNX::PreprocessConfig cfg = onnxModel->getPreprocessConfig();
      if (normalizationMode == "imagenet") { cfg.normMode = ONNX::PreprocessConfig::IMAGENET; }
      else if (normalizationMode == "scrfd") { cfg.normMode = ONNX::PreprocessConfig::SCRFD_NORM; }
      else if (normalizationMode == "scale01") { cfg.normMode = ONNX::PreprocessConfig::SCALE_01; }
      else { WARN_MSG("Unknown normalization '%s', keeping model default", normalizationMode.c_str()); }
      onnxModel->setPreprocessConfig(cfg);
      INFO_MSG("Normalization override: %s", normalizationMode.c_str());
    }
    if (!resizeMode.empty()) {
      ONNX::PreprocessConfig cfg = onnxModel->getPreprocessConfig();
      if (resizeMode == "letterbox") { cfg.resizeMode = ONNX::PreprocessConfig::LETTERBOX; onnxModel->setLetterbox(true); }
      else if (resizeMode == "direct") { cfg.resizeMode = ONNX::PreprocessConfig::DIRECT_RESIZE; onnxModel->setLetterbox(false); }
      else { WARN_MSG("Unknown resize_mode '%s', keeping model default", resizeMode.c_str()); }
      onnxModel->setPreprocessConfig(cfg);
      INFO_MSG("Resize mode override: %s", resizeMode.c_str());
    }
    if (processEveryNth > 1) { INFO_MSG("Processing every %d frames", processEveryNth); }
  } // end vision-only post-configuration

  // Mark ONNX as initialized
  {
    std::lock_guard<std::mutex> lock(onnxInitMutex);
    onnxInitialized = true;
  }
  INFO_MSG("ONNX processing initialized with confidence threshold: %f, NMS threshold: %f, Soft-NMS "
           "sigma: %f, input size: %d",
           confThreshold, nmsThreshold, softNmsSigma, inputSize);

  co.is_active = true;
  conf.is_active = true;
  isActive = true;

  // stream which connects to input
  std::thread source(Mist::sourceThread);

  // needs to pass through encoder to outputEBML
  std::thread sink(Mist::sinkThread);

  // process the video frames
  std::thread process(Mist::processThread);

  // Run main processing
  proc.Run();

  co.is_active = false;
  conf.is_active = false;
  isActive = false;
  onnxInitialized = false;

  source.join();
  HIGH_MSG("source thread joined");

  process.join();
  HIGH_MSG("process thread joined");

  sink.join();
  HIGH_MSG("sink thread joined");

  return 0;
}

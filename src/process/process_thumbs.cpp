#include "process_thumbs.h"

#include "../input/input.h"
#include "../output/output.h"
#include "process.hpp"
#include "thumbnail_artifacts.h"

#include <mist/proc_stats.h>
#include <mist/procs.h>
#include <mist/shared_memory.h>
#include <mist/stream.h>
#include <mist/triggers.h>
#include <mist/util.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdarg>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavcodec/version.h"
#include "libavutil/avutil.h"
#include "libavutil/imgutils.h"
#include "libswscale/swscale.h"
}

Util::Config co;
Util::Config conf;

// Thumbnail cache entry: timestamp + shared RGB pixels
struct ThumbFrame {
    uint64_t timeMs;
    std::shared_ptr<std::vector<uint8_t>> rgb;
};

// Shared state between source and sink threads
std::mutex thumbMutex;
std::condition_variable thumbCV;
std::deque<ThumbFrame> thumbCache;
uint64_t bufferFirstMs = 0;
uint64_t bufferLastMs = 0;
bool isVod = false;
bool vodDone = false; // true when VOD source has finished scanning
bool newData = false; // set by source when new keyframes are cached

// Sink-activity bookkeeping for exit attribution: when the sink dies, the
// exit log names what it had (not) done rather than a bare "failed".
std::atomic<uint64_t> composeCount(0);
std::atomic<uint64_t> lastComposeBootMs(0);

// Config values
uint32_t configuredThumbWidth = 160;
uint32_t configuredThumbHeight = 90;
uint32_t thumbWidth = 160;
uint32_t thumbHeight = 90;
bool thumbWidthExplicit = false;
bool thumbHeightExplicit = false;
bool thumbGeometryReady = false;
uint32_t thumbSourceWidth = 0;
uint32_t thumbSourceHeight = 0;
uint32_t gridCols = 10;
uint32_t gridRows = 10;
uint32_t jpegQuality = 75;
uint32_t regenInterval = 5000; // ms between regenerations for live
size_t maxCacheSize = 300; // cap for smart thinning (set from gridCols * gridRows * 3)

static const uint32_t MAX_THUMB_DIMENSION = 4096;
static const uint32_t MAX_GRID_AXIS = 64;
static const uint32_t MAX_SPRITE_DIMENSION = 8192;
static const uint64_t MAX_SPRITE_PIXELS = 16 * 1024 * 1024;

bool validThumbnailLayout(uint32_t cellWidth, uint32_t cellHeight) {
  if (!cellWidth || !cellHeight || cellWidth > MAX_THUMB_DIMENSION || cellHeight > MAX_THUMB_DIMENSION || !gridCols ||
      !gridRows || gridCols > MAX_GRID_AXIS || gridRows > MAX_GRID_AXIS) {
    return false;
  }
  uint64_t gridWidth = (uint64_t)cellWidth * gridCols;
  uint64_t gridHeight = (uint64_t)cellHeight * gridRows;
  return gridWidth <= MAX_SPRITE_DIMENSION && gridHeight <= MAX_SPRITE_DIMENSION && gridWidth * gridHeight <= MAX_SPRITE_PIXELS;
}

uint32_t evenDimension(uint64_t value) {
  if (value < 2) { return 2; }
  if (value > 0xFFFFFFFEull) { return 0xFFFFFFFEu; }
  if (value & 1) { --value; }
  return (uint32_t)value;
}

uint32_t evenDimensionForRatio(uint32_t fixedSize, uint32_t ratioNum, uint32_t ratioDen) {
  if (!ratioDen) { return 2; }
  return evenDimension((uint64_t)fixedSize * ratioNum / ratioDen);
}

bool updateThumbGeometry(uint32_t sourceWidth, uint32_t sourceHeight, uint32_t & outWidth, uint32_t & outHeight) {
  if (!sourceWidth || !sourceHeight) { return false; }

  std::lock_guard<std::mutex> lk(thumbMutex);

  uint32_t boxWidth = evenDimension(configuredThumbWidth);
  uint32_t boxHeight = evenDimension(configuredThumbHeight);
  uint32_t newWidth;
  uint32_t newHeight;

  if (thumbWidthExplicit && !thumbHeightExplicit) {
    newWidth = boxWidth;
    newHeight = evenDimensionForRatio(boxWidth, sourceHeight, sourceWidth);
  } else if (!thumbWidthExplicit && thumbHeightExplicit) {
    newWidth = evenDimensionForRatio(boxHeight, sourceWidth, sourceHeight);
    newHeight = boxHeight;
  } else {
    uint32_t widthAtConfiguredHeight = evenDimensionForRatio(boxHeight, sourceWidth, sourceHeight);
    if (widthAtConfiguredHeight <= boxWidth) {
      newWidth = widthAtConfiguredHeight;
      newHeight = boxHeight;
    } else {
      newWidth = boxWidth;
      newHeight = evenDimensionForRatio(boxWidth, sourceHeight, sourceWidth);
    }
  }

  if (!validThumbnailLayout(newWidth, newHeight)) { return false; }

  bool changed = !thumbGeometryReady || newWidth != thumbWidth || newHeight != thumbHeight ||
    sourceWidth != thumbSourceWidth || sourceHeight != thumbSourceHeight;
  if (changed) {
    if (thumbGeometryReady && !thumbCache.empty()) {
      thumbCache.clear();
      newData = false;
      HIGH_MSG("Thumbnail geometry changed; cleared cached thumbnails");
    }
    thumbWidth = newWidth;
    thumbHeight = newHeight;
    thumbSourceWidth = sourceWidth;
    thumbSourceHeight = sourceHeight;
    thumbGeometryReady = true;
    INFO_MSG("Thumbnail geometry: source %ux%u -> thumbs %ux%u (configured %ux%u%s%s)", sourceWidth, sourceHeight,
             thumbWidth, thumbHeight, configuredThumbWidth, configuredThumbHeight,
             thumbWidthExplicit ? ", fixed width" : "", thumbHeightExplicit ? ", fixed height" : "");
  }

  outWidth = thumbWidth;
  outHeight = thumbHeight;
  return true;
}

/// Thin the cache to stay under maxCacheSize while preserving even temporal coverage.
/// Keeps recent entries dense (near live edge), thins older entries evenly.
/// Must be called under thumbMutex.
void smartThin() {
  size_t totalCells = gridCols * gridRows;
  size_t recentKeep = totalCells / 2;
  if (thumbCache.size() <= recentKeep) { return; }

  size_t histSize = thumbCache.size() - recentKeep;
  size_t targetHist = maxCacheSize - recentKeep;
  if (histSize <= targetHist) { return; }

  std::deque<ThumbFrame> thinned;
  for (size_t i = 0; i < targetHist; ++i) {
    size_t idx = i * (histSize - 1) / (targetHist - 1);
    thinned.push_back(std::move(thumbCache[idx]));
  }
  for (size_t i = histSize; i < thumbCache.size(); ++i) { thinned.push_back(std::move(thumbCache[i])); }
  thumbCache.swap(thinned);
}

// Stats
JSON::Value pStat;
JSON::Value & pData = pStat["proc_status_update"]["status"];
std::mutex statsMutex;
IPC::sharedPage procStatsPage;
ProcExitState procExit;
std::atomic<uint64_t> thumbTotalWork{0};
std::atomic<uint64_t> thumbTotalSourceSleep{0};
std::atomic<uint64_t> thumbFrameCount{0};
std::atomic<uint64_t> thumbLastWorkEnd{0};

namespace Mist {

  class ProcessSink : public Input {
    private:
      size_t spriteIdx;
      size_t vttIdx;
      size_t previewIdx;
      uint32_t publishedThumbWidth;
      uint32_t publishedThumbHeight;

    public:
      ProcessSink(Util::Config *cfg) : Input(cfg) {
        spriteIdx = INVALID_TRACK_ID;
        vttIdx = INVALID_TRACK_ID;
        previewIdx = INVALID_TRACK_ID;
        publishedThumbWidth = 0;
        publishedThumbHeight = 0;
        capa["name"] = "Thumbs";
        streamName = opt["sink"].asString();
        if (!streamName.size()) { streamName = opt["source"].asString(); }
        Util::streamVariables(streamName, opt["source"].asString());
        Util::sanitizeName(streamName);
        Util::setStreamName(opt["source"].asString() + "→" + streamName);
        if (opt.isMember("target_mask") && !opt["target_mask"].isNull() && opt["target_mask"].asString() != "") {
          DTSC::trackValidDefault = opt["target_mask"].asInt();
        } else {
          DTSC::trackValidDefault = TRACK_VALID_EXT_HUMAN | TRACK_VALID_EXT_PUSH;
        }
      }

      ~ProcessSink() {}
      bool checkArguments() { return true; }
      bool needHeader() { return false; }
      bool readHeader() { return true; }
      bool openStreamSource() { return true; }
      void parseStreamHeader() {}
      bool needsLock() { return false; }
      bool isSingular() { return false; }
      virtual bool publishesTracks() { return false; }
      void connStats(Comms::Connections & statComm) {}

      void initTracks(uint32_t cellWidth, uint32_t cellHeight) {
        uint32_t gridW = cellWidth * gridCols;
        uint32_t gridH = cellHeight * gridRows;

        if (spriteIdx != INVALID_TRACK_ID) {
          if (cellWidth != publishedThumbWidth || cellHeight != publishedThumbHeight) {
            meta.setWidth(spriteIdx, gridW);
            meta.setHeight(spriteIdx, gridH);
            meta.markUpdated(spriteIdx);
            meta.setWidth(previewIdx, cellWidth);
            meta.setHeight(previewIdx, cellHeight);
            meta.markUpdated(previewIdx);
            publishedThumbWidth = cellWidth;
            publishedThumbHeight = cellHeight;
            INFO_MSG("Thumbnail tracks resized: sprite=%zu preview=%zu (grid %ux%u = %ux%u)", spriteIdx, previewIdx,
                     gridCols, gridRows, gridW, gridH);
          }
          return;
        }

        // Sprite sheet JPEG track
        spriteIdx = meta.addTrack();
        meta.setType(spriteIdx, "video");
        meta.setCodec(spriteIdx, "JPEG");
        meta.setLang(spriteIdx, "thu");
        meta.setWidth(spriteIdx, gridW);
        meta.setHeight(spriteIdx, gridH);
        meta.setID(spriteIdx, spriteIdx);
        meta.markUpdated(spriteIdx);
        userSelect[spriteIdx].reload(streamName, spriteIdx, COMM_STATUS_ACTSOURCEDNT);

        // VTT subtitle track
        vttIdx = meta.addTrack();
        meta.setType(vttIdx, "meta");
        meta.setCodec(vttIdx, "thumbvtt");
        meta.setID(vttIdx, vttIdx);
        meta.markUpdated(vttIdx);
        userSelect[vttIdx].reload(streamName, vttIdx, COMM_STATUS_ACTSOURCEDNT);

        // Preview JPEG track (single latest keyframe, lang="pre")
        previewIdx = meta.addTrack();
        meta.setType(previewIdx, "video");
        meta.setCodec(previewIdx, "JPEG");
        meta.setLang(previewIdx, "pre");
        meta.setWidth(previewIdx, cellWidth);
        meta.setHeight(previewIdx, cellHeight);
        meta.setID(previewIdx, previewIdx);
        meta.markUpdated(previewIdx);
        userSelect[previewIdx].reload(streamName, previewIdx, COMM_STATUS_ACTSOURCEDNT);

        publishedThumbWidth = cellWidth;
        publishedThumbHeight = cellHeight;
        INFO_MSG("Thumbnail tracks created: sprite=%zu vtt=%zu preview=%zu (grid %ux%u = %ux%u)", spriteIdx, vttIdx,
                 previewIdx, gridCols, gridRows, gridW, gridH);
      }

      void writeToDiskAndFireTrigger(const std::string & posterData, const std::string & spriteData, const std::string & vttData) {
        std::string base = "/tmp/mist_thumbs/" + streamName;
        ThumbnailArtifacts::Paths paths;
        std::string error;
        if (!ThumbnailArtifacts::publish(base, posterData, spriteData, vttData, paths, error)) {
          WARN_MSG("Failed to publish thumbnail generation: %s", error.c_str());
          return;
        }

        if (Triggers::shouldTrigger("THUMBNAIL_UPDATED", streamName)) {
          std::string payload = streamName + "\n" + paths.poster + "\n" + paths.sprite + "\n" + paths.manifest;
          Triggers::doTrigger("THUMBNAIL_UPDATED", payload, streamName);
        }
      }

      void printEncodeError(const char *message, int code) {
        char detail[128];
        av_strerror(code, detail, sizeof(detail));
        ERROR_MSG("%s: `%s` (%i)", message, detail, code);
      }

      bool encodeJpeg(const std::vector<uint8_t> & rgb, uint32_t width, uint32_t height, std::string & encoded) {
        const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
        AVCodecContext *context = codec ? avcodec_alloc_context3(codec) : NULL;
        AVFrame *frame = NULL;
        AVPacket *packet = NULL;
        SwsContext *converter = NULL;
        bool success = false;

        do {
          if (!context) {
            ERROR_MSG("Could not allocate MJPEG context");
            break;
          }
          context->width = width;
          context->height = height;
          context->pix_fmt = AV_PIX_FMT_YUVJ420P;
          context->time_base = (AVRational){1, 1};
          context->codec_type = AVMEDIA_TYPE_VIDEO;
          int quality = 31 - (int)(jpegQuality * 30.0 / 100.0);
          context->qmin = std::max(1, std::min(31, quality));
          context->qmax = context->qmin;
          int result = avcodec_open2(context, codec, 0);
          if (result < 0) {
            printEncodeError("Could not open MJPEG encoder", result);
            break;
          }

          frame = av_frame_alloc();
          if (!frame) {
            ERROR_MSG("Could not allocate MJPEG frame");
            break;
          }
          frame->width = width;
          frame->height = height;
          frame->format = AV_PIX_FMT_YUVJ420P;
          result = av_frame_get_buffer(frame, 0);
          if (result < 0) {
            printEncodeError("Could not allocate MJPEG frame data", result);
            break;
          }

          converter = sws_getContext(width, height, AV_PIX_FMT_RGB24, width, height, AV_PIX_FMT_YUVJ420P, SWS_BILINEAR,
                                     NULL, NULL, NULL);
          if (!converter) {
            ERROR_MSG("Could not create MJPEG pixel converter");
            break;
          }
          const uint8_t *source[1] = {rgb.data()};
          int sourceStride[1] = {(int)(width * 3)};
          if (sws_scale(converter, source, sourceStride, 0, height, frame->data, frame->linesize) <= 0) {
            ERROR_MSG("Could not convert MJPEG frame pixels");
            break;
          }

          packet = av_packet_alloc();
          if (!packet) {
            ERROR_MSG("Could not allocate MJPEG packet");
            break;
          }
          frame->pts = 0;
          result = avcodec_send_frame(context, frame);
          if (result < 0) {
            printEncodeError("MJPEG encode send failed", result);
            break;
          }
          result = avcodec_receive_packet(context, packet);
          if (result < 0) {
            printEncodeError("MJPEG encode receive failed", result);
            break;
          }
          encoded.assign((const char *)packet->data, packet->size);
          success = true;
        } while (false);

        if (packet) { av_packet_free(&packet); }
        if (converter) { sws_freeContext(converter); }
        if (frame) { av_frame_free(&frame); }
        if (context) { avcodec_free_context(&context); }
        return success;
      }

      /// Compose the 10x10 grid from cached thumbnails, encode as JPEG, generate VTT
      void composeAndBuffer() {
        uint32_t totalCells = gridCols * gridRows;

        // Snapshot thumbCache under lock (cheap: shared_ptr refcount bumps only)
        std::deque<ThumbFrame> localCache;
        uint64_t firstMs, lastMs;
        uint32_t cellWidth, cellHeight;
        {
          std::lock_guard<std::mutex> lk(thumbMutex);
          if (!thumbGeometryReady) { return; }
          localCache = thumbCache;
          firstMs = bufferFirstMs;
          lastMs = bufferLastMs;
          cellWidth = thumbWidth;
          cellHeight = thumbHeight;
        }

        if (localCache.empty() || lastMs <= firstMs) {
          HIGH_MSG("No thumbnails to compose (cache=%zu, range=%" PRIu64 "-%" PRIu64 ")", localCache.size(), firstMs, lastMs);
          return;
        }

        initTracks(cellWidth, cellHeight);

        uint32_t gridW = cellWidth * gridCols;
        uint32_t gridH = cellHeight * gridRows;

        // Sample evenly when cache exceeds grid capacity
        std::vector<size_t> selected;
        size_t cacheSize = localCache.size();
        if (cacheSize <= totalCells) {
          for (size_t i = 0; i < cacheSize; ++i) { selected.push_back(i); }
        } else {
          for (uint32_t c = 0; c < totalCells; ++c) { selected.push_back(c * (cacheSize - 1) / (totalCells - 1)); }
        }

        // Allocate RGB buffer for the full grid
        std::vector<uint8_t> gridRgb(gridW * gridH * 3, 0);
        size_t rgbSize = cellWidth * cellHeight * 3;

        uint32_t cellIdx = 0;
        for (size_t si = 0; si < selected.size(); ++si, ++cellIdx) {
          auto & entry = localCache[selected[si]];
          if (!entry.rgb || entry.rgb->size() != rgbSize) { continue; }

          uint32_t col = cellIdx % gridCols;
          uint32_t row = cellIdx / gridCols;
          uint32_t xOff = col * cellWidth;
          uint32_t yOff = row * cellHeight;

          for (uint32_t y = 0; y < cellHeight; y++) {
            memcpy(&gridRgb[(yOff + y) * gridW * 3 + xOff * 3], &(*entry.rgb)[y * cellWidth * 3], cellWidth * 3);
          }
        }

        std::string spriteJpegData;
        if (!encodeJpeg(gridRgb, gridW, gridH, spriteJpegData)) { return; }
        uint64_t bufTs = localCache.back().timeMs;
        thisIdx = spriteIdx;
        thisTime = bufTs;
        bufferLivePacket(bufTs, 0, spriteIdx, spriteJpegData.data(), spriteJpegData.size(), 0, true);
        INFO_MSG("Buffered sprite sheet: %zu bytes at %" PRIu64 "ms", spriteJpegData.size(), bufTs);

        // Build VTT manifest using the same sampled entries as the grid
        std::stringstream vtt;
        vtt << "WEBVTT\n\n";
        for (size_t si = 0; si < selected.size(); ++si) {
          uint64_t startMs = localCache[selected[si]].timeMs;
          uint64_t endMs;
          if (si + 1 < selected.size()) {
            endMs = localCache[selected[si + 1]].timeMs;
          } else {
            endMs = lastMs;
          }
          if (endMs <= startMs) { endMs = startMs + 1000; }

          uint32_t col = (uint32_t)si % gridCols;
          uint32_t row = (uint32_t)si / gridCols;
          uint32_t x = col * cellWidth;
          uint32_t y = row * cellHeight;

          char timeBuf[80];
          snprintf(timeBuf, sizeof(timeBuf),
                   "%02" PRIu64 ":%02" PRIu64 ":%02" PRIu64 ".%03" PRIu64 " --> "
                   "%02" PRIu64 ":%02" PRIu64 ":%02" PRIu64 ".%03" PRIu64,
                   startMs / 3600000, (startMs % 3600000) / 60000, ((startMs % 3600000) % 60000) / 1000, startMs % 1000,
                   endMs / 3600000, (endMs % 3600000) / 60000, ((endMs % 3600000) % 60000) / 1000, endMs % 1000);
          vtt << timeBuf << "\n";
          vtt << "/" << streamName << ".jpg?track=" << spriteIdx << "#xywh=" << x << "," << y << "," << cellWidth << ","
              << cellHeight << "\n\n";
        }
        std::string vttStr = vtt.str();
        thisIdx = vttIdx;
        thisTime = bufTs;
        bufferLivePacket(bufTs, 0, vttIdx, vttStr.c_str(), vttStr.size(), 0, true);
        INFO_MSG("Buffered VTT manifest (%zu cues, %zu bytes) from %zu keyframes at %" PRIu64 "ms", selected.size(),
                 vttStr.size(), localCache.size(), bufTs);

        // Encode latest thumbnail as standalone preview JPEG
        std::string previewJpegData;
        auto & latestEntry = localCache.back();
        if (latestEntry.rgb && latestEntry.rgb->size() == rgbSize) {
          if (encodeJpeg(*latestEntry.rgb, cellWidth, cellHeight, previewJpegData)) {
            thisIdx = previewIdx;
            thisTime = bufTs;
            bufferLivePacket(bufTs, 0, previewIdx, previewJpegData.data(), previewJpegData.size(), 0, true);
            INFO_MSG("Buffered preview JPEG: %zu bytes at %" PRIu64 "ms", previewJpegData.size(), bufTs);
          }
        }

        if (!previewJpegData.empty() || !spriteJpegData.empty()) {
          writeToDiskAndFireTrigger(previewJpegData, spriteJpegData, vttStr);
        }
        ++composeCount;
        lastComposeBootMs = Util::bootMS();
      }

      void streamMainLoop() {
        Comms::Connections statComm;
        while (config->is_active && co.is_active) {
          bool sourceDone = false;
          {
            std::unique_lock<std::mutex> lk(thumbMutex);
            thumbCV.wait(lk, [&]() { return newData || vodDone || !co.is_active || !config->is_active; });
            if (!co.is_active || !config->is_active) {
              // Normal teardown: the source thread or the job ended. Without an
              // explicit reason this would exit unclean (rc=1) and get logged —
              // and restarted — as a failure.
              Util::logExitReason(ER_CLEAN_INACTIVE, "source ended");
              return;
            }

            // Publish the first generation immediately, then coalesce keyframe
            // updates until the configured interval. A finished VOD bypasses
            // the delay so its final manifest is never omitted.
            uint64_t lastCompose = lastComposeBootMs.load(std::memory_order_relaxed);
            uint64_t now = Util::bootMS();
            if (!vodDone && lastCompose && now < lastCompose + regenInterval) {
              thumbCV.wait_for(lk, std::chrono::milliseconds(lastCompose + regenInterval - now),
                               [&]() { return vodDone || !co.is_active || !config->is_active; });
              if (!co.is_active || !config->is_active) {
                Util::logExitReason(ER_CLEAN_INACTIVE, "source ended");
                return;
              }
            }
            sourceDone = vodDone;
            newData = false;
          }

          // Check for shutdown requests
          if (spriteIdx != INVALID_TRACK_ID && !userSelect.count(spriteIdx)) {
            userSelect[spriteIdx].reload(streamName, spriteIdx, COMM_STATUS_ACTSOURCEDNT);
          }
          if (previewIdx != INVALID_TRACK_ID && !userSelect.count(previewIdx)) {
            userSelect[previewIdx].reload(streamName, previewIdx, COMM_STATUS_ACTSOURCEDNT);
          }
          if (spriteIdx != INVALID_TRACK_ID && userSelect.count(spriteIdx) &&
              (userSelect[spriteIdx].getStatus() & COMM_STATUS_REQDISCONNECT)) {
            procExit.log(ER_CLEAN_LIVE_BUFFER_REQ, 0, "buffer requested shutdown");
            return;
          }
          if (isSingular() && !bufferActive()) {
            procExit.log(ER_SHM_LOST, 0, "Buffer shut down");
            return;
          }

          // Check if we have thumbnails to compose
          bool shouldCompose = false;
          {
            std::lock_guard<std::mutex> lk(thumbMutex);
            shouldCompose = !thumbCache.empty() && bufferLastMs > bufferFirstMs;
          }

          if (shouldCompose) {
            composeAndBuffer();

            if (sourceDone) {
              // Successful completion — classify it as such, or the exit is
              // reported (and restarted) as "Thumbnail sink thread failed".
              Util::logExitReason(ER_CLEAN_EOF, "VOD sprite sheet complete");
              INFO_MSG("VOD sprite sheet complete");
              return;
            }
          }
        }
        // While-condition exit: job/source ended between iterations.
        Util::logExitReason(ER_CLEAN_INACTIVE, "source ended");
      }
  };

  class ProcessSource : public Output {
    protected:
      inline virtual bool keepGoing() { return config->is_active; }

    private:
      AVCodecContext *decCtx;
      const AVCodec *decCodec;
      SwsContext *scaleCtx;
      AVFrame *rawFrame;
      std::string codecName;
      bool decoderReady;

      void printError(std::string preamble, int code) {
        char err[128];
        av_strerror(code, err, sizeof(err));
        ERROR_MSG("%s: `%s` (%i)", preamble.c_str(), err, code);
      }

      bool initDecoder() {
        if (decoderReady) { return true; }
        codecName = M.getCodec(thisIdx);

        if (codecName == "H264") {
          decCodec = avcodec_find_decoder(AV_CODEC_ID_H264);
        } else if (codecName == "AV1") {
          decCodec = avcodec_find_decoder(AV_CODEC_ID_AV1);
        } else if (codecName == "JPEG") {
          decCodec = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
        } else {
          ERROR_MSG("Unsupported input codec for thumbnails: %s", codecName.c_str());
          return false;
        }

        if (!decCodec) {
          ERROR_MSG("Could not find decoder for %s", codecName.c_str());
          return false;
        }

        decCtx = avcodec_alloc_context3(decCodec);
        if (!decCtx) {
          ERROR_MSG("Could not allocate decode context");
          return false;
        }

        std::string init = M.getInit(thisIdx);
        if (init.size()) {
          decCtx->extradata = (unsigned char *)av_mallocz(init.size() + AV_INPUT_BUFFER_PADDING_SIZE);
          if (!decCtx->extradata) {
            ERROR_MSG("Could not allocate decoder initialization data");
            avcodec_free_context(&decCtx);
            return false;
          }
          decCtx->extradata_size = init.size();
          memcpy(decCtx->extradata, init.data(), init.size());
        }

        decCtx->width = M.getWidth(thisIdx);
        decCtx->height = M.getHeight(thisIdx);
        decCtx->pkt_timebase = (AVRational){1, 1000};

        int ret = avcodec_open2(decCtx, decCodec, 0);
        if (ret < 0) {
          printError("Could not open decoder", ret);
          avcodec_free_context(&decCtx);
          decCtx = 0;
          return false;
        }

        rawFrame = av_frame_alloc();
        if (!rawFrame) {
          ERROR_MSG("Could not allocate thumbnail decode frame");
          avcodec_free_context(&decCtx);
          return false;
        }
        decoderReady = true;
        INFO_MSG("Decoder initialized for %s (%ux%u)", codecName.c_str(), (unsigned)decCtx->width, (unsigned)decCtx->height);
        return true;
      }

      bool decodeAndScale(char *data, size_t len, uint64_t decodeTime, uint64_t presentationTime, std::vector<uint8_t> & outRgb) {
        // Keyframes are independent access points, but codecs with frame
        // reordering may still delay their single decoded frame. Reset around
        // each keyframe and drain it explicitly; decoding every inter-frame
        // packet would make thumbnail generation needlessly expensive.
        avcodec_flush_buffers(decCtx);
        AVPacket *pktIn = av_packet_alloc();
        if (!pktIn) {
          ERROR_MSG("Could not allocate thumbnail decode packet");
          return false;
        }
        int ret = av_new_packet(pktIn, len);
        if (ret < 0) {
          av_packet_free(&pktIn);
          printError("Could not allocate thumbnail packet data", ret);
          return false;
        }
        memcpy(pktIn->data, data, len);
        pktIn->dts = (int64_t)decodeTime;
        pktIn->pts = (int64_t)presentationTime;

        ret = avcodec_send_packet(decCtx, pktIn);
        av_packet_free(&pktIn);
        if (ret < 0) {
          printError("Send packet failed", ret);
          return false;
        }

        ret = avcodec_receive_frame(decCtx, rawFrame);
        if (ret == AVERROR(EAGAIN)) {
          ret = avcodec_send_packet(decCtx, NULL);
          if (ret < 0) {
            printError("Drain keyframe failed", ret);
            return false;
          }
          ret = avcodec_receive_frame(decCtx, rawFrame);
        }
        if (ret < 0) {
          printError("Receive keyframe failed", ret);
          return false;
        }

        uint32_t targetWidth = 0;
        uint32_t targetHeight = 0;
        if (!updateThumbGeometry(rawFrame->width, rawFrame->height, targetWidth, targetHeight)) {
          ERROR_MSG("Could not determine thumbnail geometry from source frame");
          return false;
        }

        // Scale to thumbnail size
        scaleCtx = sws_getCachedContext(scaleCtx, rawFrame->width, rawFrame->height, (AVPixelFormat)rawFrame->format,
                                        targetWidth, targetHeight, AV_PIX_FMT_RGB24, SWS_BILINEAR, NULL, NULL, NULL);
        if (!scaleCtx) {
          ERROR_MSG("Could not create scale context");
          return false;
        }

        outRgb.resize(targetWidth * targetHeight * 3);
        uint8_t *dstSlice[1] = {outRgb.data()};
        int dstStride[1] = {(int)(targetWidth * 3)};
        sws_scale(scaleCtx, rawFrame->data, rawFrame->linesize, 0, rawFrame->height, dstSlice, dstStride);
        return true;
      }

    public:
      bool isRecording() { return false; }

      ProcessSource(Socket::Connection & c, Util::Config & _cfg, JSON::Value & _capa) : Output(c, _cfg, _capa) {
        meta.ignorePid(getpid());
        closeMyConn();
        targetParams["keeptimes"] = true;
        realTime = 0;
        decCtx = 0;
        decCodec = 0;
        scaleCtx = 0;
        rawFrame = 0;
        decoderReady = false;
        initialize();
        wantRequest = false;
        parseData = true;
      }

      ~ProcessSource() {
        if (scaleCtx) { sws_freeContext(scaleCtx); }
        if (rawFrame) { av_frame_free(&rawFrame); }
        if (decCtx) { avcodec_free_context(&decCtx); }
      }

      static void init(Util::Config *cfg, JSON::Value & capa) {
        Output::init(cfg, capa);
        capa["name"] = "Thumbs";
        capa["codecs"][0u][0u].append("H264");
        capa["codecs"][0u][0u].append("AV1");
        capa["codecs"][0u][0u].append("JPEG");
        cfg->addOption("streamname",
                       JSON::fromString("{\"arg\":\"string\",\"short\":\"s\",\"long\":"
                                        "\"stream\",\"help\":\"The name of the stream "
                                        "that this connector will transmit.\"}"));
        cfg->addBasicConnectorOptions(capa);
      }

      void sendNext() {
        if (!config->is_active) { return; }

        // Only process keyframes
        if (!thisPacket.getFlag("keyframe")) { return; }

        // Track source sleep: time between end of last work and start of this work
        uint64_t workStart = Util::getMicros();
        uint64_t lastEnd = thumbLastWorkEnd.load(std::memory_order_relaxed);
        if (lastEnd) {
          thumbTotalSourceSleep.store(thumbTotalSourceSleep.load(std::memory_order_relaxed) + (workStart - lastEnd),
                                      std::memory_order_relaxed);
        }

        // Init decoder on first keyframe
        if (!initDecoder()) {
          thumbLastWorkEnd.store(Util::getMicros(), std::memory_order_relaxed);
          return;
        }

        // Decode and scale outside lock
        auto rgbData = std::make_shared<std::vector<uint8_t>>();
        int64_t presentationTime = (int64_t)thisTime + thisPacket.getInt("offset");
        if (presentationTime < 0) { presentationTime = 0; }
        if (!decodeAndScale(thisData, thisDataLen, thisTime, (uint64_t)presentationTime, *rgbData)) {
          thumbLastWorkEnd.store(Util::getMicros(), std::memory_order_relaxed);
          return;
        }

        thumbTotalWork.store(thumbTotalWork.load(std::memory_order_relaxed) + Util::getMicros(workStart), std::memory_order_relaxed);
        thumbLastWorkEnd.store(Util::getMicros(), std::memory_order_relaxed);
        thumbFrameCount.store(thumbFrameCount.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);

        {
          std::lock_guard<std::mutex> lk(thumbMutex);
          bufferFirstMs = M.getFirstms(thisIdx);
          bufferLastMs = M.getLastms(thisIdx);
          isVod = M.getVod();

          thumbCache.push_back({(uint64_t)presentationTime, std::move(rgbData)});
          newData = true;

          // Hard prune: entries outside DVR window
          while (!thumbCache.empty() && thumbCache.front().timeMs < bufferFirstMs) { thumbCache.pop_front(); }

          // Smart thin: cap memory usage
          if (thumbCache.size() > maxCacheSize) { smartThin(); }
        }
        thumbCV.notify_all();
      }

      /// Called when we've caught up with the live edge or end of VOD
      bool onFinish() {
        bool sourceIsVod;
        size_t cached;
        {
          std::lock_guard<std::mutex> lk(thumbMutex);
          sourceIsVod = isVod;
          cached = thumbCache.size();
          if (sourceIsVod) { vodDone = true; }
        }
        if (sourceIsVod) {
          INFO_MSG("VOD scan complete, %zu keyframes decoded", cached);
          thumbCV.notify_all();
          return true;
        }

        thumbCV.notify_all();

        HIGH_MSG("Live scan cycle done, %zu keyframes cached. Continuing...", cached);
        return false;
      }
  };

  bool ProcThumbs::CheckConfig() {
    if (!opt.isMember("source") || !opt["source"] || !opt["source"].isString()) {
      FAIL_MSG("Invalid source in config!");
      return false;
    }
    if (!validThumbnailLayout(configuredThumbWidth, configuredThumbHeight)) {
      FAIL_MSG("Thumbnail grid exceeds safe geometry limits");
      return false;
    }
    if (!jpegQuality || jpegQuality > 100) {
      FAIL_MSG("JPEG quality must be between 1 and 100");
      return false;
    }
    if (!regenInterval || regenInterval > 3600000) {
      FAIL_MSG("Thumbnail regeneration interval must be between 1 and 3600000ms");
      return false;
    }
    return true;
  }

  void ProcThumbs::Run() {
    uint64_t lastProcUpdate = Util::bootSecs();
    {
      std::lock_guard<std::mutex> guard(statsMutex);
      pStat["proc_status_update"]["id"] = getpid();
      pStat["proc_status_update"]["proc"] = "Thumbs";
    }
    uint64_t startTime = Util::bootSecs();
    // Previous-window snapshots for pressure derivation
    uint64_t prevWork = 0, prevSrcWait = 0;
    uint64_t prevBufferLastMs = 0; // last published source-clock high-water mark
    uint64_t prevUpdateBootMs = Util::bootMS();
    uint32_t capacitySamples = 0;
    while (conf.is_active && co.is_active) {
      Util::sleep(200);
      if (lastProcUpdate + 1 <= Util::bootSecs()) {
        std::lock_guard<std::mutex> guard(statsMutex);
        size_t cachedThumbs;
        uint64_t firstMs;
        uint64_t lastMs;
        {
          std::lock_guard<std::mutex> thumbGuard(thumbMutex);
          cachedThumbs = thumbCache.size();
          firstMs = bufferFirstMs;
          lastMs = bufferLastMs;
        }
        pData["active_seconds"] = (Util::bootSecs() - startTime);
        pData["ainfo"]["thumbs_cached"] = (uint64_t)cachedThumbs;
        pData["ainfo"]["buffer_first_ms"] = firstMs;
        pData["ainfo"]["buffer_last_ms"] = lastMs;
        Util::sendUDPApi(pStat);
        // Write timing stats + normalized pressure to shm for InputBuffer rate control
        if (procStatsPage.mapped && ProcState::isValid(procStatsPage)) {
          ProcState *s = (ProcState *)procStatsPage.mapped;
          uint64_t curWork = thumbTotalWork.load(std::memory_order_relaxed);
          uint64_t curSrcWait = thumbTotalSourceSleep.load(std::memory_order_relaxed);
          uint64_t curFrames = thumbFrameCount.load(std::memory_order_relaxed);
          uint64_t nowBootMs = Util::bootMS();

          // Pressure: thumbs is rarely a bottleneck. Decode-dominated -> cpu;
          // otherwise source-wait (no real backpressure).
          uint8_t reason = PRC_REASON_UNKNOWN;
          uint16_t pressureQ = 0;
          uint64_t dWork = curWork - prevWork;
          uint64_t dSrc = curSrcWait - prevSrcWait;
          uint64_t dTotal = dWork + dSrc;
          if (dTotal > 0) {
            double workRatio = (double)dWork / (double)dTotal;
            double srcRatio = (double)dSrc / (double)dTotal;
            if (workRatio > 0.9) {
              reason = PRC_REASON_CPU;
              pressureQ = (uint16_t)(workRatio * 0.5 * 65535.0);
            } else if (srcRatio > 0.5) {
              reason = PRC_REASON_SOURCE_WAIT;
              pressureQ = 0;
            }
          }

          // Observed speed: how much source-clock advanced per wall-clock ms,
          // i.e. a real realtime multiplier in the same unit as the controller's
          // effectiveSpeed. (Reporting fps here would be a unit mismatch:
          // controller compares obsSpeed to feederSpeed in realtime-multiples.)
          // For thumbs the source-clock proxy is bufferLastMs (highest keyframe
          // timestamp cached so far).
          uint32_t obsSpeedQ = 0, capacitySpeedQ = 0;
          uint64_t wallDeltaMs = (nowBootMs > prevUpdateBootMs) ? (nowBootMs - prevUpdateBootMs) : 0;
          if (wallDeltaMs > 0 && prevBufferLastMs && lastMs > prevBufferLastMs) {
            double rtf = (double)(lastMs - prevBufferLastMs) / (double)wallDeltaMs;
            obsSpeedQ = ProcState::speedToQ16(rtf);
            if (dWork) {
              capacitySpeedQ = ProcState::speedToQ16((double)(lastMs - prevBufferLastMs) * 1000.0 / (double)dWork);
            }
          }

          s->beginPublish();
          s->totalWork = curWork;
          s->totalSourceWait = curSrcWait;
          s->totalSinkWait = 0;
          s->totalExternalWait = 0;
          s->frameCount = curFrames;
          s->lastUpdateMs = nowBootMs;
          s->observedSpeedQ16_16 = obsSpeedQ;
          s->inputSpeedQ16_16 = obsSpeedQ;
          s->outputSpeedQ16_16 = obsSpeedQ;
          if (capacitySpeedQ) {
            ++capacitySamples;
            s->capacitySpeedQ16_16 = capacitySpeedQ;
            s->recommendedFeedQ16_16 = ProcState::speedToQ16(std::max(1.0, ((double)capacitySpeedQ / 65536.0) * 0.85));
            s->flags |= PRC_FLAG_CAPACITY_VALID;
          }
          s->flags &= ~(PRC_FLAG_SOURCE_LIMITED | PRC_FLAG_PROCESSOR_LIMITED);
          if (dTotal && dSrc * 2 > dTotal) { s->flags |= PRC_FLAG_SOURCE_LIMITED; }
          if (pressureQ > (uint16_t)(0.7 * 65535.0)) { s->flags |= PRC_FLAG_PROCESSOR_LIMITED; }
          s->phase = capacitySamples >= 3 ? PRC_PHASE_READY : PRC_PHASE_MEASURING;
          s->confidenceQ0_16 = (uint16_t)std::min((uint32_t)65535, capacitySamples * 65535 / 3);
          s->pressureQ0_16 = pressureQ;
          s->canAcceptMore = 1;
          s->reasonCode = reason;
          s->queueDepth = (uint32_t)cachedThumbs;
          s->inflight = 0;
          s->retryCount = 0;
          s->primaryResource = PRC_RESOURCE_CPU;
          s->endPublish();

          prevWork = curWork;
          prevSrcWait = curSrcWait;
          prevBufferLastMs = lastMs;
          prevUpdateBootMs = nowBootMs;
        }
        lastProcUpdate = Util::bootSecs();
      }
    }
  }

} // namespace Mist

void sinkThread() {
  Util::nameThread("sinkThread");
  Mist::ProcessSink in(&co);
  co.getOption("output", true).append("-");
  MEDIUM_MSG("Running thumbnail sink thread...");
  int rc = in.run();
  if (rc == 0) {
    procExit.log(ER_CLEAN_EOF, 0, "Thumbnail sink thread finished");
  } else {
    // Attribute the failure: include the sink's last activity so an exit
    // before the first compose (e.g. buffer attach failure) is
    // distinguishable from one mid-stream.
    uint64_t composed = composeCount.load(std::memory_order_relaxed);
    uint64_t lastCompose = lastComposeBootMs.load(std::memory_order_relaxed);
    uint64_t cacheSize;
    {
      std::lock_guard<std::mutex> lk(thumbMutex);
      cacheSize = thumbCache.size();
    }
    procExit.log(Util::mRExitReason ? Util::mRExitReason : ER_UNKNOWN, rc,
                 "%s (composes=%" PRIu64 ", lastComposeAgoMs=%" PRIu64 ", cachedThumbs=%" PRIu64 ")",
                 Util::exitReason[0] ? Util::exitReason : "Thumbnail sink thread failed", composed,
                 lastCompose ? (Util::bootMS() - lastCompose) : 0, cacheSize);
  }
  INFO_MSG("Stop thumbnail sink thread");
  conf.is_active = false;
}

void sourceThread() {
  Util::nameThread("sourceThread");
  JSON::Value capa;
  Mist::ProcessSource::init(&conf, capa);
  conf.getOption("streamname", true).append(Mist::opt["source"].c_str());
  JSON::Value optJ;
  optJ["arg"] = "string";
  optJ["default"] = "";
  optJ["arg_num"] = 1;
  conf.addOption("target", optJ);
  conf.getOption("target", true).append("-");
  if (Mist::opt.isMember("track_select")) {
    conf.getOption("target", true).append("-?" + Mist::opt["track_select"].asString());
  }
  Socket::Connection S;
  Mist::ProcessSource out(S, conf, capa);
  MEDIUM_MSG("Running thumbnail source thread...");
  int rc = out.run();
  if (rc == 0) {
    procExit.log(ER_CLEAN_EOF, 0, "Thumbnail source thread finished");
  } else {
    procExit.log(Util::mRExitReason ? Util::mRExitReason : ER_UNKNOWN, rc, "%s",
                 Util::exitReason[0] ? Util::exitReason : "Thumbnail source thread failed");
  }
  INFO_MSG("Stop thumbnail source thread");
  if (rc == 0) {
    // A finite source presented through InputBuffer may identify as live. Let
    // the sink publish its coalesced final generation before stopping it.
    {
      std::lock_guard<std::mutex> lk(thumbMutex);
      vodDone = true;
    }
  } else {
    co.is_active = false;
  }
  thumbCV.notify_all();
}

void logcallback(void *ptr, int level, const char *fmt, va_list vl) {
  if (level > AV_LOG_WARNING) { return; }
  static int print_prefix = 1;
  char line[1024];
  av_log_format_line(ptr, level, fmt, vl, line, sizeof(line), &print_prefix);
  INFO_MSG("LibAV: %s", line);
}

int main(int argc, char *argv[]) {
  DTSC::trackValidMask = TRACK_VALID_INT_PROCESS;
  Util::Config config(argv[0]);
  Util::Config::binaryType = Util::PROCESS;

  // Initialize SHM early so exit reasons are available even for config errors
  {
    char shmName[NAME_BUFFER_SIZE];
    snprintf(shmName, NAME_BUFFER_SIZE, SHM_PROC_STATE, getpid());
    procStatsPage.init(shmName, sizeof(ProcState), true, false);
    ProcState::initPage(procStatsPage);
  }

  JSON::Value capa;
  av_log_set_callback(logcallback);

  {
    JSON::Value optJ;
    optJ["arg"] = "string";
    optJ["default"] = "-";
    optJ["arg_num"] = 1;
    optJ["help"] = "JSON configuration, or - (default) to read from stdin";
    config.addOption("configuration", optJ);
    optJ.null();
    optJ["long"] = "json";
    optJ["short"] = "j";
    optJ["help"] = "Output connector info in JSON format, then exit.";
    optJ["value"].append(0);
    config.addOption("json", optJ);
  }

  capa["codecs"][0u][0u].append("H264");
  capa["codecs"][0u][0u].append("AV1");
  capa["codecs"][0u][0u].append("JPEG");

  if (!(config.parseArgs(argc, argv))) {
    procExit.log(ER_FORMAT_SPECIFIC, 2, "Failed to parse command-line arguments");
    return procExit.flush(procStatsPage);
  }
  if (config.getBool("json")) {
    capa["name"] = "Thumbs";
    capa["hrn"] = "Thumbnail sprite sheet generator";
    capa["desc"] = "Generates thumbnail sprite sheets (10x10 grid) and WebVTT metadata for "
                   "scrub-bar preview thumbnails";
    addGenericProcessOptions(capa);
    {
      JSON::Value & genopts = capa["optional"]["general_process_options"]["options"];

      genopts["track_select"]["name"] = "Source selector";
      genopts["track_select"]["help"] = "Which video track to use as source. Defaults to first video track.";
      genopts["track_select"]["type"] = "string";
      genopts["track_select"]["validate"][0u] = "track_selector";
      genopts["track_select"]["default"] = "video=lowres";
      genopts["track_select"]["sort"] = "a";

      genopts["sink"]["name"] = "Target stream";
      genopts["sink"]["help"] = "Stream to add thumbnail tracks to. Defaults to source stream. May contain variables.";
      genopts["sink"]["type"] = "string";
      genopts["sink"]["validate"][0u] = "streamname_with_wildcard_and_variables";
      genopts["sink"]["sort"] = "b";

      genopts["source_mask"]["name"] = "Source track mask";
      genopts["source_mask"]["help"] = "What internal processes should have access to the source track(s)";
      genopts["source_mask"]["type"] = "select";
      genopts["source_mask"]["select"][0u][0u] = 255;
      genopts["source_mask"]["select"][0u][1u] = "Everything";
      genopts["source_mask"]["select"][1u][0u] = 4;
      genopts["source_mask"]["select"][1u][1u] = "Processing tasks (not viewers, not pushes)";
      genopts["source_mask"]["default"] = "Keep original value";
      genopts["source_mask"]["sort"] = "c";

      genopts["target_mask"]["name"] = "Output track mask";
      genopts["target_mask"]["help"] = "What internal processes should have access to the output track(s)";
      genopts["target_mask"]["type"] = "select";
      genopts["target_mask"]["select"][0u][0u] = 255;
      genopts["target_mask"]["select"][0u][1u] = "Everything";
      genopts["target_mask"]["select"][1u][0u] = 1;
      genopts["target_mask"]["select"][1u][1u] = "Viewer tasks (not processing, not pushes)";
      genopts["target_mask"]["default"] = "Keep original value";
      genopts["target_mask"]["sort"] = "d";
    }

    capa["optional"]["thumb_width"]["name"] = "Thumbnail width";
    capa["optional"]["thumb_width"]["help"] = "Maximum width of each individual thumbnail in the grid";
    capa["optional"]["thumb_width"]["type"] = "uint";
    capa["optional"]["thumb_width"]["default"] = 160;
    capa["optional"]["thumb_width"]["max"] = MAX_THUMB_DIMENSION;
    capa["optional"]["thumb_width"]["sort"] = "ba";

    capa["optional"]["thumb_height"]["name"] = "Thumbnail height";
    capa["optional"]["thumb_height"]["help"] = "Maximum height of each individual thumbnail in the grid";
    capa["optional"]["thumb_height"]["type"] = "uint";
    capa["optional"]["thumb_height"]["default"] = 90;
    capa["optional"]["thumb_height"]["max"] = MAX_THUMB_DIMENSION;
    capa["optional"]["thumb_height"]["sort"] = "bb";

    capa["optional"]["grid_cols"]["name"] = "Grid columns";
    capa["optional"]["grid_cols"]["help"] = "Number of columns in the sprite grid";
    capa["optional"]["grid_cols"]["type"] = "uint";
    capa["optional"]["grid_cols"]["default"] = 10;
    capa["optional"]["grid_cols"]["max"] = MAX_GRID_AXIS;
    capa["optional"]["grid_cols"]["sort"] = "bc";

    capa["optional"]["grid_rows"]["name"] = "Grid rows";
    capa["optional"]["grid_rows"]["help"] = "Number of rows in the sprite grid";
    capa["optional"]["grid_rows"]["type"] = "uint";
    capa["optional"]["grid_rows"]["default"] = 10;
    capa["optional"]["grid_rows"]["max"] = MAX_GRID_AXIS;
    capa["optional"]["grid_rows"]["sort"] = "bd";

    capa["optional"]["jpeg_quality"]["name"] = "JPEG quality";
    capa["optional"]["jpeg_quality"]["help"] = "Quality of the sprite sheet JPEG (1-100)";
    capa["optional"]["jpeg_quality"]["type"] = "uint";
    capa["optional"]["jpeg_quality"]["default"] = 75;
    capa["optional"]["jpeg_quality"]["min"] = 1;
    capa["optional"]["jpeg_quality"]["max"] = 100;
    capa["optional"]["jpeg_quality"]["sort"] = "be";

    capa["optional"]["interval"]["name"] = "Regeneration interval";
    capa["optional"]["interval"]["help"] = "How often to regenerate the sprite sheet for live streams (in ms)";
    capa["optional"]["interval"]["type"] = "uint";
    capa["optional"]["interval"]["unit"] = "ms";
    capa["optional"]["interval"]["default"] = 5000;
    capa["optional"]["interval"]["min"] = 1;
    capa["optional"]["interval"]["max"] = 3600000;
    capa["optional"]["interval"]["sort"] = "bf";

    std::cout << capa.toString() << std::endl;
    return -1;
  }

  Util::redirectLogsIfNeeded();

  // Read configuration
  if (config.getString("configuration") != "-") {
    Mist::opt = JSON::fromString(config.getString("configuration"));
  } else {
    std::string json, line;
    INFO_MSG("Reading configuration from standard input");
    while (std::getline(std::cin, line)) { json.append(line); }
    Mist::opt = JSON::fromString(json.c_str());
  }

  // Apply config
  if (Mist::opt.isMember("thumb_width") && Mist::opt["thumb_width"].asInt()) {
    configuredThumbWidth = Mist::opt["thumb_width"].asInt();
    thumbWidth = evenDimension(configuredThumbWidth);
    thumbWidthExplicit = true;
  }
  if (Mist::opt.isMember("thumb_height") && Mist::opt["thumb_height"].asInt()) {
    configuredThumbHeight = Mist::opt["thumb_height"].asInt();
    thumbHeight = evenDimension(configuredThumbHeight);
    thumbHeightExplicit = true;
  }
  if (Mist::opt.isMember("grid_cols") && Mist::opt["grid_cols"].asInt()) { gridCols = Mist::opt["grid_cols"].asInt(); }
  if (Mist::opt.isMember("grid_rows") && Mist::opt["grid_rows"].asInt()) { gridRows = Mist::opt["grid_rows"].asInt(); }
  if (Mist::opt.isMember("jpeg_quality") && Mist::opt["jpeg_quality"].asInt()) {
    jpegQuality = Mist::opt["jpeg_quality"].asInt();
  }
  if (Mist::opt.isMember("interval") && Mist::opt["interval"].asInt()) {
    regenInterval = Mist::opt["interval"].asInt();
  }

  maxCacheSize = (size_t)gridCols * gridRows * 3;

  ProcState::publishStartup(procStatsPage, 8.0, PRC_RESOURCE_CPU);

  // Validate
  Mist::ProcThumbs proc;
  if (!proc.CheckConfig()) {
    procExit.log(ER_FORMAT_SPECIFIC, 2, "Invalid process configuration");
    return procExit.flush(procStatsPage);
  }

  INFO_MSG("Thumbnail generator: %ux%u grid, %ux%u thumb box, quality=%u, interval=%ums", gridCols, gridRows,
           configuredThumbWidth, configuredThumbHeight, jpegQuality, regenInterval);

  co.is_active = true;
  conf.is_active = true;

  std::thread source(sourceThread);
  std::thread sink(sinkThread);

  proc.Run();

  co.is_active = false;
  conf.is_active = false;
  thumbCV.notify_all();

  source.join();
  HIGH_MSG("Source thread joined");

  sink.join();
  HIGH_MSG("Sink thread joined");

  return procExit.flush(procStatsPage);
}

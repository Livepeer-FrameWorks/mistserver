#include "process_thumbs.h"

#include "../input/input.h"
#include "../output/output.h"
#include "process.hpp"

#include <mist/proc_stats.h>
#include <mist/procs.h>
#include <mist/shared_memory.h>
#include <mist/triggers.h>
#include <mist/util.h>

#include <atomic>
#include <condition_variable>
#include <cstdarg>
#include <iostream>
#include <deque>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>

extern "C"{
#include "libavcodec/avcodec.h"
#include "libavcodec/version.h"
#include "libavutil/avutil.h"
#include "libavutil/imgutils.h"
#include "libswscale/swscale.h"
}

Util::Config co;
Util::Config conf;

// Thumbnail cache entry: timestamp + shared RGB pixels
struct ThumbFrame{
  uint64_t timeMs;
  std::shared_ptr<std::vector<uint8_t>> rgb;
};

// Shared state between source and sink threads
std::mutex thumbMutex;
std::condition_variable thumbCV;
std::deque<ThumbFrame> thumbCache;
uint64_t bufferFirstMs = 0;
uint64_t bufferLastMs = 0;
int64_t bootMsOffset = 0;
bool isVod = false;
bool vodDone = false; // true when VOD source has finished scanning
bool newData = false; // set by source when new keyframes are cached

// Config values
uint32_t thumbWidth = 160;
uint32_t thumbHeight = 90;
uint32_t gridCols = 10;
uint32_t gridRows = 10;
uint32_t jpegQuality = 75;
uint32_t regenInterval = 5000; // ms between regenerations for live
size_t maxCacheSize = 300;     // cap for smart thinning (set from gridCols * gridRows * 3)

/// Thin the cache to stay under maxCacheSize while preserving even temporal coverage.
/// Keeps recent entries dense (near live edge), thins older entries evenly.
/// Must be called under thumbMutex.
void smartThin(){
  size_t totalCells = gridCols * gridRows;
  size_t recentKeep = totalCells / 2;
  if (thumbCache.size() <= recentKeep){return;}

  size_t histSize = thumbCache.size() - recentKeep;
  size_t targetHist = maxCacheSize - recentKeep;
  if (histSize <= targetHist){return;}

  std::deque<ThumbFrame> thinned;
  for (size_t i = 0; i < targetHist; ++i){
    size_t idx = i * (histSize - 1) / (targetHist - 1);
    thinned.push_back(std::move(thumbCache[idx]));
  }
  for (size_t i = histSize; i < thumbCache.size(); ++i){
    thinned.push_back(std::move(thumbCache[i]));
  }
  thumbCache.swap(thinned);
}

// Stats
JSON::Value pStat;
JSON::Value &pData = pStat["proc_status_update"]["status"];
std::mutex statsMutex;
IPC::sharedPage procStatsPage;
std::atomic<uint64_t> thumbTotalWork{0};
std::atomic<uint64_t> thumbTotalSourceSleep{0};
std::atomic<uint64_t> thumbFrameCount{0};
std::atomic<uint64_t> thumbLastWorkEnd{0};

namespace Mist{

  class ProcessSink : public Input{
  private:
    size_t spriteIdx;
    size_t vttIdx;
    size_t previewIdx;

  public:
    ProcessSink(Util::Config *cfg) : Input(cfg){
      spriteIdx = INVALID_TRACK_ID;
      vttIdx = INVALID_TRACK_ID;
      previewIdx = INVALID_TRACK_ID;
      capa["name"] = "Thumbs";
      streamName = opt["sink"].asString();
      if (!streamName.size()){streamName = opt["source"].asString();}
      Util::streamVariables(streamName, opt["source"].asString());
      Util::setStreamName(opt["source"].asString() + "→" + streamName);
      if (opt.isMember("target_mask") && !opt["target_mask"].isNull() &&
          opt["target_mask"].asString() != ""){
        DTSC::trackValidDefault = opt["target_mask"].asInt();
      }else{
        DTSC::trackValidDefault = TRACK_VALID_EXT_HUMAN | TRACK_VALID_EXT_PUSH;
      }
    }

    ~ProcessSink(){}
    bool checkArguments(){return true;}
    bool needHeader(){return false;}
    bool readHeader(){return true;}
    bool openStreamSource(){return true;}
    void parseStreamHeader(){}
    bool needsLock(){return false;}
    bool isSingular(){return false;}
    virtual bool publishesTracks(){return false;}
    void connStats(Comms::Connections &statComm){}

    void initTracks(){
      if (spriteIdx != INVALID_TRACK_ID){return;}
      uint32_t gridW = thumbWidth * gridCols;
      uint32_t gridH = thumbHeight * gridRows;

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
      meta.setWidth(previewIdx, thumbWidth);
      meta.setHeight(previewIdx, thumbHeight);
      meta.setID(previewIdx, previewIdx);
      meta.markUpdated(previewIdx);
      userSelect[previewIdx].reload(streamName, previewIdx, COMM_STATUS_ACTSOURCEDNT);

      INFO_MSG("Thumbnail tracks created: sprite=%zu vtt=%zu preview=%zu (grid %ux%u = %ux%u)",
               spriteIdx, vttIdx, previewIdx, gridCols, gridRows, gridW, gridH);
    }

    void writeToDiskAndFireTrigger(const std::string &posterData,
                                    const std::string &spriteData,
                                    const std::string &vttData){
      std::string base = "/tmp/mist_thumbs/" + streamName;
      std::string posterPath = base + "/poster.jpg";
      std::string spritePath = base + "/sprite.jpg";
      std::string vttPath = base + "/sprite.vtt";

      Util::createPathFor(posterPath);

      struct FileEntry{
        const std::string &path;
        const std::string &data;
      };
      FileEntry files[] = {
        {posterPath, posterData},
        {spritePath, spriteData},
        {vttPath, vttData},
      };
      for (auto &f : files){
        if (f.data.empty()){continue;}
        FILE *fp = fopen(f.path.c_str(), "wb");
        if (!fp){
          WARN_MSG("Failed to write thumbnail file %s", f.path.c_str());
          continue;
        }
        fwrite(f.data.data(), 1, f.data.size(), fp);
        fclose(fp);
      }

      if (Triggers::shouldTrigger("THUMBNAIL_UPDATED", streamName)){
        std::string payload = streamName + "\n" + posterPath + "\n" + spritePath + "\n" + vttPath;
        Triggers::doTrigger("THUMBNAIL_UPDATED", payload, streamName);
      }
    }

    /// Compose the 10x10 grid from cached thumbnails, encode as JPEG, generate VTT
    void composeAndBuffer(){
      initTracks();

      uint32_t totalCells = gridCols * gridRows;
      uint32_t gridW = thumbWidth * gridCols;
      uint32_t gridH = thumbHeight * gridRows;

      // Snapshot thumbCache under lock (cheap: shared_ptr refcount bumps only)
      std::deque<ThumbFrame> localCache;
      uint64_t firstMs, lastMs;
      {
        std::lock_guard<std::mutex> lk(thumbMutex);
        localCache = thumbCache;
        firstMs = bufferFirstMs;
        lastMs = bufferLastMs;
      }

      if (localCache.empty() || lastMs <= firstMs){
        HIGH_MSG("No thumbnails to compose (cache=%zu, range=%" PRIu64 "-%" PRIu64 ")",
                 localCache.size(), firstMs, lastMs);
        return;
      }

      // Sample evenly when cache exceeds grid capacity
      std::vector<size_t> selected;
      size_t cacheSize = localCache.size();
      if (cacheSize <= totalCells){
        for (size_t i = 0; i < cacheSize; ++i){ selected.push_back(i); }
      }else{
        for (uint32_t c = 0; c < totalCells; ++c){
          selected.push_back(c * (cacheSize - 1) / (totalCells - 1));
        }
      }

      // Allocate RGB buffer for the full grid
      std::vector<uint8_t> gridRgb(gridW * gridH * 3, 0);
      size_t rgbSize = thumbWidth * thumbHeight * 3;

      uint32_t cellIdx = 0;
      for (size_t si = 0; si < selected.size(); ++si, ++cellIdx){
        auto &entry = localCache[selected[si]];
        if (!entry.rgb || entry.rgb->size() != rgbSize){continue;}

        uint32_t col = cellIdx % gridCols;
        uint32_t row = cellIdx / gridCols;
        uint32_t xOff = col * thumbWidth;
        uint32_t yOff = row * thumbHeight;

        for (uint32_t y = 0; y < thumbHeight; y++){
          memcpy(&gridRgb[(yOff + y) * gridW * 3 + xOff * 3],
                 &(*entry.rgb)[y * thumbWidth * 3], thumbWidth * 3);
        }
      }

      // Encode the grid as JPEG using MJPEG encoder
      const AVCodec *jpegCodec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
      if (!jpegCodec){
        ERROR_MSG("Could not find MJPEG encoder");
        return;
      }
      AVCodecContext *jpegCtx = avcodec_alloc_context3(jpegCodec);
      if (!jpegCtx){
        ERROR_MSG("Could not allocate MJPEG context");
        return;
      }
      jpegCtx->width = gridW;
      jpegCtx->height = gridH;
      jpegCtx->pix_fmt = AV_PIX_FMT_YUVJ420P;
      jpegCtx->time_base = (AVRational){1, 1};
      jpegCtx->codec_type = AVMEDIA_TYPE_VIDEO;
      // Quality: libav uses qmin/qmax (1-31 scale, lower = better)
      // Map our 1-100 quality to 1-31 (inverted)
      int qval = 31 - (int)(jpegQuality * 30.0 / 100.0);
      if (qval < 1) qval = 1;
      if (qval > 31) qval = 31;
      jpegCtx->qmin = qval;
      jpegCtx->qmax = qval;

      if (avcodec_open2(jpegCtx, jpegCodec, 0) < 0){
        ERROR_MSG("Could not open MJPEG encoder");
        avcodec_free_context(&jpegCtx);
        return;
      }

      // Convert RGB to YUV420P for the encoder
      AVFrame *frame = av_frame_alloc();
      frame->width = gridW;
      frame->height = gridH;
      frame->format = AV_PIX_FMT_YUVJ420P;
      av_frame_get_buffer(frame, 0);

      SwsContext *swsCtx =
          sws_getContext(gridW, gridH, AV_PIX_FMT_RGB24, gridW, gridH,
                         AV_PIX_FMT_YUVJ420P, SWS_BILINEAR, NULL, NULL, NULL);
      uint8_t *srcSlice[1] = {gridRgb.data()};
      int srcStride[1] = {(int)(gridW * 3)};
      sws_scale(swsCtx, srcSlice, srcStride, 0, gridH, frame->data, frame->linesize);
      sws_freeContext(swsCtx);

      frame->pts = 0;

      // Encode
      AVPacket *pkt = av_packet_alloc();
      std::string spriteJpegData;
      int ret = avcodec_send_frame(jpegCtx, frame);
      if (ret >= 0){
        ret = avcodec_receive_packet(jpegCtx, pkt);
        if (ret >= 0){
          // Buffer the JPEG sprite sheet at the latest cached keyframe's time
          uint64_t bufTs = localCache.back().timeMs;
          thisIdx = spriteIdx;
          thisTime = bufTs;
          bufferLivePacket(bufTs, 0, spriteIdx, (const char *)pkt->data, pkt->size, 0, true);
          spriteJpegData.assign((const char *)pkt->data, pkt->size);
          INFO_MSG("Buffered sprite sheet: %d bytes at %" PRIu64 "ms", pkt->size, bufTs);
        }else{
          ERROR_MSG("MJPEG encode receive failed: %d", ret);
        }
      }else{
        ERROR_MSG("MJPEG encode send failed: %d", ret);
      }

      av_packet_free(&pkt);
      av_frame_free(&frame);
      avcodec_free_context(&jpegCtx);

      // Build VTT manifest using the same sampled entries as the grid
      std::stringstream vtt;
      vtt << "WEBVTT\n\n";
      for (size_t si = 0; si < selected.size(); ++si){
        uint64_t startMs = localCache[selected[si]].timeMs;
        uint64_t endMs;
        if (si + 1 < selected.size()){
          endMs = localCache[selected[si + 1]].timeMs;
        }else{
          endMs = lastMs;
        }
        if (endMs <= startMs){endMs = startMs + 1000;}

        uint32_t col = (uint32_t)si % gridCols;
        uint32_t row = (uint32_t)si / gridCols;
        uint32_t x = col * thumbWidth;
        uint32_t y = row * thumbHeight;

        char timeBuf[80];
        snprintf(timeBuf, sizeof(timeBuf),
                 "%02" PRIu64 ":%02" PRIu64 ":%02" PRIu64 ".%03" PRIu64
                 " --> "
                 "%02" PRIu64 ":%02" PRIu64 ":%02" PRIu64 ".%03" PRIu64,
                 startMs / 3600000, (startMs % 3600000) / 60000,
                 ((startMs % 3600000) % 60000) / 1000, startMs % 1000,
                 endMs / 3600000, (endMs % 3600000) / 60000,
                 ((endMs % 3600000) % 60000) / 1000, endMs % 1000);
        vtt << timeBuf << "\n";
        vtt << "/" << streamName << ".jpg?track=" << spriteIdx
            << "#xywh=" << x << "," << y << "," << thumbWidth << "," << thumbHeight << "\n\n";
      }
      std::string vttStr = vtt.str();
      uint64_t bufTs = localCache.back().timeMs;
      thisIdx = vttIdx;
      thisTime = bufTs;
      bufferLivePacket(bufTs, 0, vttIdx, vttStr.c_str(), vttStr.size(), 0, true);
      INFO_MSG("Buffered VTT manifest (%zu cues, %zu bytes) from %zu keyframes at %" PRIu64 "ms",
               selected.size(), vttStr.size(), localCache.size(), bufTs);

      // Encode latest thumbnail as standalone preview JPEG
      std::string previewJpegData;
      auto &latestEntry = localCache.back();
      if (latestEntry.rgb && latestEntry.rgb->size() == rgbSize){
        const AVCodec *prevCodec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
        AVCodecContext *prevCtx = prevCodec ? avcodec_alloc_context3(prevCodec) : NULL;
        if (prevCtx){
          prevCtx->width = thumbWidth;
          prevCtx->height = thumbHeight;
          prevCtx->pix_fmt = AV_PIX_FMT_YUVJ420P;
          prevCtx->time_base = (AVRational){1, 1};
          prevCtx->codec_type = AVMEDIA_TYPE_VIDEO;
          prevCtx->qmin = qval;
          prevCtx->qmax = qval;
          if (avcodec_open2(prevCtx, prevCodec, 0) >= 0){
            AVFrame *prevFrame = av_frame_alloc();
            prevFrame->width = thumbWidth;
            prevFrame->height = thumbHeight;
            prevFrame->format = AV_PIX_FMT_YUVJ420P;
            av_frame_get_buffer(prevFrame, 0);
            SwsContext *prevSws = sws_getContext(thumbWidth, thumbHeight, AV_PIX_FMT_RGB24,
                                                 thumbWidth, thumbHeight, AV_PIX_FMT_YUVJ420P,
                                                 SWS_BILINEAR, NULL, NULL, NULL);
            uint8_t *prevSrc[1] = {latestEntry.rgb->data()};
            int prevStride[1] = {(int)(thumbWidth * 3)};
            sws_scale(prevSws, prevSrc, prevStride, 0, thumbHeight, prevFrame->data, prevFrame->linesize);
            sws_freeContext(prevSws);
            prevFrame->pts = 0;
            AVPacket *prevPkt = av_packet_alloc();
            if (avcodec_send_frame(prevCtx, prevFrame) >= 0 &&
                avcodec_receive_packet(prevCtx, prevPkt) >= 0){
              thisIdx = previewIdx;
              thisTime = bufTs;
              bufferLivePacket(bufTs, 0, previewIdx, (const char *)prevPkt->data, prevPkt->size, 0, true);
              previewJpegData.assign((const char *)prevPkt->data, prevPkt->size);
              INFO_MSG("Buffered preview JPEG: %d bytes at %" PRIu64 "ms", prevPkt->size, bufTs);
            }
            av_packet_free(&prevPkt);
            av_frame_free(&prevFrame);
          }
          avcodec_free_context(&prevCtx);
        }
      }

      if (!previewJpegData.empty() || !spriteJpegData.empty()){
        writeToDiskAndFireTrigger(previewJpegData, spriteJpegData, vttStr);
      }
    }

    void streamMainLoop(){
      Comms::Connections statComm;
      while (config->is_active && co.is_active){
        // Wait for signal from source or timeout for periodic regen
        {
          std::unique_lock<std::mutex> lk(thumbMutex);
          thumbCV.wait_for(lk, std::chrono::milliseconds(regenInterval),
                           [&](){return newData || vodDone || !co.is_active || !config->is_active;});
          if (!co.is_active || !config->is_active){return;}
          newData = false;
        }

        // Check for shutdown requests
        if (spriteIdx != INVALID_TRACK_ID && !userSelect.count(spriteIdx)){
          userSelect[spriteIdx].reload(streamName, spriteIdx, COMM_STATUS_ACTSOURCEDNT);
        }
        if (previewIdx != INVALID_TRACK_ID && !userSelect.count(previewIdx)){
          userSelect[previewIdx].reload(streamName, previewIdx, COMM_STATUS_ACTSOURCEDNT);
        }
        if (spriteIdx != INVALID_TRACK_ID && userSelect.count(spriteIdx) &&
            (userSelect[spriteIdx].getStatus() & COMM_STATUS_REQDISCONNECT)){
          Util::logExitReason(ER_CLEAN_LIVE_BUFFER_REQ, "buffer requested shutdown");
          return;
        }
        if (isSingular() && !bufferActive()){
          Util::logExitReason(ER_SHM_LOST, "Buffer shut down");
          return;
        }

        // Check if we have thumbnails to compose
        bool shouldCompose = false;
        {
          std::lock_guard<std::mutex> lk(thumbMutex);
          shouldCompose = !thumbCache.empty() && bufferLastMs > bufferFirstMs;
        }

        if (shouldCompose){
          composeAndBuffer();

          if (vodDone){
            INFO_MSG("VOD sprite sheet complete");
            return;
          }
        }
      }
    }
  };

  ProcessSink *sinkClass = 0;

  class ProcessSource : public Output{
  protected:
    inline virtual bool keepGoing(){return config->is_active;}

  private:
    AVCodecContext *decCtx;
    const AVCodec *decCodec;
    SwsContext *scaleCtx;
    AVFrame *rawFrame;
    std::string codecName;
    bool decoderReady;

    void printError(std::string preamble, int code){
      char err[128];
      av_strerror(code, err, sizeof(err));
      ERROR_MSG("%s: `%s` (%i)", preamble.c_str(), err, code);
    }

    bool initDecoder(){
      if (decoderReady){return true;}
      codecName = M.getCodec(thisIdx);

      if (codecName == "H264"){
        decCodec = avcodec_find_decoder(AV_CODEC_ID_H264);
      }else if (codecName == "AV1"){
        decCodec = avcodec_find_decoder(AV_CODEC_ID_AV1);
      }else if (codecName == "JPEG"){
        decCodec = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
      }else{
        ERROR_MSG("Unsupported input codec for thumbnails: %s", codecName.c_str());
        return false;
      }

      if (!decCodec){
        ERROR_MSG("Could not find decoder for %s", codecName.c_str());
        return false;
      }

      decCtx = avcodec_alloc_context3(decCodec);
      if (!decCtx){
        ERROR_MSG("Could not allocate decode context");
        return false;
      }

      std::string init = M.getInit(thisIdx);
      if (init.size()){
        decCtx->extradata = (unsigned char *)malloc(init.size());
        decCtx->extradata_size = init.size();
        memcpy(decCtx->extradata, init.data(), init.size());
      }

      decCtx->width = M.getWidth(thisIdx);
      decCtx->height = M.getHeight(thisIdx);

      int ret = avcodec_open2(decCtx, decCodec, 0);
      if (ret < 0){
        printError("Could not open decoder", ret);
        avcodec_free_context(&decCtx);
        decCtx = 0;
        return false;
      }

      rawFrame = av_frame_alloc();
      decoderReady = true;
      INFO_MSG("Decoder initialized for %s (%ux%u)", codecName.c_str(),
               (unsigned)decCtx->width, (unsigned)decCtx->height);
      return true;
    }

    bool decodeAndScale(char *data, size_t len, std::vector<uint8_t> &outRgb){
      AVPacket *pktIn = av_packet_alloc();
      av_new_packet(pktIn, len);
      memcpy(pktIn->data, data, len);

      int ret = avcodec_send_packet(decCtx, pktIn);
      av_packet_free(&pktIn);
      if (ret < 0){
        printError("Send packet failed", ret);
        return false;
      }

      ret = avcodec_receive_frame(decCtx, rawFrame);
      if (ret < 0){
        if (ret != AVERROR(EAGAIN)){printError("Receive frame failed", ret);}
        return false;
      }

      // Scale to thumbnail size
      if (!scaleCtx){
        scaleCtx = sws_getContext(rawFrame->width, rawFrame->height,
                                  (AVPixelFormat)rawFrame->format, thumbWidth, thumbHeight,
                                  AV_PIX_FMT_RGB24, SWS_BILINEAR, NULL, NULL, NULL);
        if (!scaleCtx){
          ERROR_MSG("Could not create scale context");
          return false;
        }
      }

      outRgb.resize(thumbWidth * thumbHeight * 3);
      uint8_t *dstSlice[1] = {outRgb.data()};
      int dstStride[1] = {(int)(thumbWidth * 3)};
      sws_scale(scaleCtx, rawFrame->data, rawFrame->linesize, 0, rawFrame->height, dstSlice,
                dstStride);
      return true;
    }

  public:
    bool isRecording(){return false;}

    ProcessSource(Socket::Connection &c, Util::Config &_cfg, JSON::Value &_capa)
        : Output(c, _cfg, _capa){
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

    ~ProcessSource(){
      if (scaleCtx){sws_freeContext(scaleCtx);}
      if (rawFrame){av_frame_free(&rawFrame);}
      if (decCtx){avcodec_free_context(&decCtx);}
    }

    static void init(Util::Config *cfg, JSON::Value &capa){
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

    void sendNext(){
      if (!config->is_active){return;}

      // Only process keyframes
      if (!thisPacket.getFlag("keyframe")){return;}

      // Track source sleep: time between end of last work and start of this work
      uint64_t workStart = Util::getMicros();
      uint64_t lastEnd = thumbLastWorkEnd.load(std::memory_order_relaxed);
      if (lastEnd){
        thumbTotalSourceSleep.store(
          thumbTotalSourceSleep.load(std::memory_order_relaxed) + (workStart - lastEnd),
          std::memory_order_relaxed);
      }

      // Init decoder on first keyframe
      if (!initDecoder()){
        thumbLastWorkEnd.store(Util::getMicros(), std::memory_order_relaxed);
        return;
      }

      // Decode and scale outside lock
      auto rgbData = std::make_shared<std::vector<uint8_t>>();
      if (!decodeAndScale(thisData, thisDataLen, *rgbData)){
        thumbLastWorkEnd.store(Util::getMicros(), std::memory_order_relaxed);
        return;
      }

      thumbTotalWork.store(
        thumbTotalWork.load(std::memory_order_relaxed) + Util::getMicros(workStart),
        std::memory_order_relaxed);
      thumbLastWorkEnd.store(Util::getMicros(), std::memory_order_relaxed);
      thumbFrameCount.store(
        thumbFrameCount.load(std::memory_order_relaxed) + 1,
        std::memory_order_relaxed);

      {
        std::lock_guard<std::mutex> lk(thumbMutex);
        bufferFirstMs = M.getFirstms(thisIdx);
        bufferLastMs = M.getLastms(thisIdx);
        bootMsOffset = M.getBootMsOffset();
        isVod = M.getVod();

        thumbCache.push_back({thisTime, std::move(rgbData)});
        newData = true;

        // Hard prune: entries outside DVR window
        while (!thumbCache.empty() && thumbCache.front().timeMs < bufferFirstMs){
          thumbCache.pop_front();
        }

        // Smart thin: cap memory usage
        if (thumbCache.size() > maxCacheSize){
          smartThin();
        }
      }
      thumbCV.notify_all();
    }

    /// Called when we've caught up with the live edge or end of VOD
    bool onFinish(){
      if (isVod){
        INFO_MSG("VOD scan complete, %zu keyframes decoded", thumbCache.size());
        {
          std::lock_guard<std::mutex> lk(thumbMutex);
          vodDone = true;
        }
        thumbCV.notify_all();
        return true;
      }

      thumbCV.notify_all();

      HIGH_MSG("Live scan cycle done, %zu keyframes cached. Continuing...", thumbCache.size());
      return false;
    }
  };

  bool ProcThumbs::CheckConfig(){
    if (!opt.isMember("source") || !opt["source"] || !opt["source"].isString()){
      FAIL_MSG("Invalid source in config!");
      return false;
    }
    return true;
  }

  void ProcThumbs::Run(){
    uint64_t lastProcUpdate = Util::bootSecs();
    {
      std::lock_guard<std::mutex> guard(statsMutex);
      pStat["proc_status_update"]["id"] = getpid();
      pStat["proc_status_update"]["proc"] = "Thumbs";
    }
    uint64_t startTime = Util::bootSecs();
    while (conf.is_active && co.is_active){
      Util::sleep(200);
      if (lastProcUpdate + 5 <= Util::bootSecs()){
        std::lock_guard<std::mutex> guard(statsMutex);
        pData["active_seconds"] = (Util::bootSecs() - startTime);
        pData["ainfo"]["thumbs_cached"] = (uint64_t)thumbCache.size();
        pData["ainfo"]["buffer_first_ms"] = bufferFirstMs;
        pData["ainfo"]["buffer_last_ms"] = bufferLastMs;
        Util::sendUDPApi(pStat);
        // Write timing stats to shm for InputBuffer rate control
        if (procStatsPage.mapped){
          ProcTimingStats *s = (ProcTimingStats *)procStatsPage.mapped;
          s->totalWork = thumbTotalWork.load(std::memory_order_relaxed);
          s->totalSourceSleep = thumbTotalSourceSleep.load(std::memory_order_relaxed);
          s->totalSinkSleep = 0;
          s->frameCount = thumbFrameCount.load(std::memory_order_relaxed);
          s->lastUpdateMs = Util::bootMS();
        }
        lastProcUpdate = Util::bootSecs();
      }
    }
  }

}// namespace Mist

void sinkThread(){
  Util::nameThread("sinkThread");
  Mist::ProcessSink in(&co);
  Mist::sinkClass = &in;
  co.getOption("output", true).append("-");
  MEDIUM_MSG("Running thumbnail sink thread...");
  in.run();
  INFO_MSG("Stop thumbnail sink thread...");
  conf.is_active = false;
}

void sourceThread(){
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
  if (Mist::opt.isMember("track_select")){
    conf.getOption("target", true).append("-?" + Mist::opt["track_select"].asString());
  }
  Socket::Connection S;
  Mist::ProcessSource out(S, conf, capa);
  MEDIUM_MSG("Running thumbnail source thread...");
  out.run();
  INFO_MSG("Stop thumbnail source thread...");
  co.is_active = false;
  thumbCV.notify_all();
}

void logcallback(void *ptr, int level, const char *fmt, va_list vl){
  if (level > AV_LOG_WARNING){return;}
  static int print_prefix = 1;
  char line[1024];
  av_log_format_line(ptr, level, fmt, vl, line, sizeof(line), &print_prefix);
  INFO_MSG("LibAV: %s", line);
}

int main(int argc, char *argv[]){
  DTSC::trackValidMask = TRACK_VALID_INT_PROCESS;
  Util::Config config(argv[0]);
  Util::Config::binaryType = Util::PROCESS;
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

  if (!(config.parseArgs(argc, argv))){return 1;}
  if (config.getBool("json")){
    capa["name"] = "Thumbs";
    capa["hrn"] = "Thumbnail sprite sheet generator";
    capa["desc"] = "Generates thumbnail sprite sheets (10x10 grid) and WebVTT metadata for "
                   "scrub-bar preview thumbnails";
    addGenericProcessOptions(capa);
    {
      JSON::Value &genopts = capa["optional"]["general_process_options"]["options"];

      genopts["track_select"]["name"] = "Source selector";
      genopts["track_select"]["help"] =
          "Which video track to use as source. Defaults to first video track.";
      genopts["track_select"]["type"] = "string";
      genopts["track_select"]["validate"][0u] = "track_selector";
      genopts["track_select"]["default"] = "video=lowres";
      genopts["track_select"]["sort"] = "a";

      genopts["sink"]["name"] = "Target stream";
      genopts["sink"]["help"] =
          "Stream to add thumbnail tracks to. Defaults to source stream. May contain variables.";
      genopts["sink"]["type"] = "string";
      genopts["sink"]["validate"][0u] = "streamname_with_wildcard_and_variables";
      genopts["sink"]["sort"] = "b";

      genopts["source_mask"]["name"] = "Source track mask";
      genopts["source_mask"]["help"] =
          "What internal processes should have access to the source track(s)";
      genopts["source_mask"]["type"] = "select";
      genopts["source_mask"]["select"][0u][0u] = 255;
      genopts["source_mask"]["select"][0u][1u] = "Everything";
      genopts["source_mask"]["select"][1u][0u] = 4;
      genopts["source_mask"]["select"][1u][1u] = "Processing tasks (not viewers, not pushes)";
      genopts["source_mask"]["default"] = "Keep original value";
      genopts["source_mask"]["sort"] = "c";

      genopts["target_mask"]["name"] = "Output track mask";
      genopts["target_mask"]["help"] =
          "What internal processes should have access to the output track(s)";
      genopts["target_mask"]["type"] = "select";
      genopts["target_mask"]["select"][0u][0u] = 255;
      genopts["target_mask"]["select"][0u][1u] = "Everything";
      genopts["target_mask"]["select"][1u][0u] = 1;
      genopts["target_mask"]["select"][1u][1u] = "Viewer tasks (not processing, not pushes)";
      genopts["target_mask"]["default"] = "Keep original value";
      genopts["target_mask"]["sort"] = "d";
    }

    capa["optional"]["thumb_width"]["name"] = "Thumbnail width";
    capa["optional"]["thumb_width"]["help"] = "Width of each individual thumbnail in the grid";
    capa["optional"]["thumb_width"]["type"] = "uint";
    capa["optional"]["thumb_width"]["default"] = 160;
    capa["optional"]["thumb_width"]["sort"] = "ba";

    capa["optional"]["thumb_height"]["name"] = "Thumbnail height";
    capa["optional"]["thumb_height"]["help"] = "Height of each individual thumbnail in the grid";
    capa["optional"]["thumb_height"]["type"] = "uint";
    capa["optional"]["thumb_height"]["default"] = 90;
    capa["optional"]["thumb_height"]["sort"] = "bb";

    capa["optional"]["grid_cols"]["name"] = "Grid columns";
    capa["optional"]["grid_cols"]["help"] = "Number of columns in the sprite grid";
    capa["optional"]["grid_cols"]["type"] = "uint";
    capa["optional"]["grid_cols"]["default"] = 10;
    capa["optional"]["grid_cols"]["sort"] = "bc";

    capa["optional"]["grid_rows"]["name"] = "Grid rows";
    capa["optional"]["grid_rows"]["help"] = "Number of rows in the sprite grid";
    capa["optional"]["grid_rows"]["type"] = "uint";
    capa["optional"]["grid_rows"]["default"] = 10;
    capa["optional"]["grid_rows"]["sort"] = "bd";

    capa["optional"]["jpeg_quality"]["name"] = "JPEG quality";
    capa["optional"]["jpeg_quality"]["help"] = "Quality of the sprite sheet JPEG (1-100)";
    capa["optional"]["jpeg_quality"]["type"] = "uint";
    capa["optional"]["jpeg_quality"]["default"] = 75;
    capa["optional"]["jpeg_quality"]["min"] = 1;
    capa["optional"]["jpeg_quality"]["max"] = 100;
    capa["optional"]["jpeg_quality"]["sort"] = "be";

    capa["optional"]["interval"]["name"] = "Regeneration interval";
    capa["optional"]["interval"]["help"] =
        "How often to regenerate the sprite sheet for live streams (in ms)";
    capa["optional"]["interval"]["type"] = "uint";
    capa["optional"]["interval"]["unit"] = "ms";
    capa["optional"]["interval"]["default"] = 5000;
    capa["optional"]["interval"]["sort"] = "bf";

    std::cout << capa.toString() << std::endl;
    return -1;
  }

  Util::redirectLogsIfNeeded();

  // Read configuration
  if (config.getString("configuration") != "-"){
    Mist::opt = JSON::fromString(config.getString("configuration"));
  }else{
    std::string json, line;
    INFO_MSG("Reading configuration from standard input");
    while (std::getline(std::cin, line)){json.append(line);}
    Mist::opt = JSON::fromString(json.c_str());
  }

  // Apply config
  if (Mist::opt.isMember("thumb_width") && Mist::opt["thumb_width"].asInt()){
    thumbWidth = Mist::opt["thumb_width"].asInt();
  }
  if (Mist::opt.isMember("thumb_height") && Mist::opt["thumb_height"].asInt()){
    thumbHeight = Mist::opt["thumb_height"].asInt();
  }
  if (Mist::opt.isMember("grid_cols") && Mist::opt["grid_cols"].asInt()){
    gridCols = Mist::opt["grid_cols"].asInt();
  }
  if (Mist::opt.isMember("grid_rows") && Mist::opt["grid_rows"].asInt()){
    gridRows = Mist::opt["grid_rows"].asInt();
  }
  if (Mist::opt.isMember("jpeg_quality") && Mist::opt["jpeg_quality"].asInt()){
    jpegQuality = Mist::opt["jpeg_quality"].asInt();
  }
  if (Mist::opt.isMember("interval") && Mist::opt["interval"].asInt()){
    regenInterval = Mist::opt["interval"].asInt();
  }

  maxCacheSize = (size_t)gridCols * gridRows * 3;

  // Validate
  Mist::ProcThumbs proc;
  if (!proc.CheckConfig()){
    FAIL_MSG("Configuration error!");
    return 1;
  }

  INFO_MSG("Thumbnail generator: %ux%u grid, %ux%u per thumb, quality=%u, interval=%ums",
           gridCols, gridRows, thumbWidth, thumbHeight, jpegQuality, regenInterval);

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

  return 0;
}

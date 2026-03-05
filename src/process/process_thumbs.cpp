#include "process_thumbs.h"

#include "../input/input.h"
#include "../output/output.h"
#include "process.hpp"

#include <mist/procs.h>
#include <mist/util.h>

#include <condition_variable>
#include <cstdarg>
#include <iostream>
#include <map>
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

// Thumbnail cache: timestamp -> scaled RGB pixels
struct ThumbFrame{
  uint64_t timeMs;
  std::vector<uint8_t> rgb; // thumb_width * thumb_height * 3
};

// Shared state between source and sink threads
std::mutex thumbMutex;
std::condition_variable thumbCV;
std::map<uint64_t, ThumbFrame> thumbCache; // keyframe timestamp -> decoded thumb
uint64_t bufferFirstMs = 0;
uint64_t bufferLastMs = 0;
int64_t bootMsOffset = 0;
bool isVod = false;
bool vodDone = false; // true when VOD source has finished scanning

// Config values
uint32_t thumbWidth = 160;
uint32_t thumbHeight = 90;
uint32_t gridCols = 10;
uint32_t gridRows = 10;
uint32_t jpegQuality = 75;
uint32_t regenInterval = 5000; // ms between regenerations for live

// Stats
JSON::Value pStat;
JSON::Value &pData = pStat["proc_status_update"]["status"];
std::mutex statsMutex;

namespace Mist{

  class ProcessSink : public Input{
  private:
    size_t spriteIdx;
    size_t vttIdx;

  public:
    ProcessSink(Util::Config *cfg) : Input(cfg){
      spriteIdx = INVALID_TRACK_ID;
      vttIdx = INVALID_TRACK_ID;
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

      // Sprite sheet JPEG track (lang="thumbnails" used by HLS/DASH for detection)
      spriteIdx = meta.addTrack();
      meta.setType(spriteIdx, "video");
      meta.setCodec(spriteIdx, "JPEG");
      meta.setLang(spriteIdx, "thumbnails");
      meta.setWidth(spriteIdx, gridW);
      meta.setHeight(spriteIdx, gridH);
      meta.setID(spriteIdx, spriteIdx);
      meta.markUpdated(spriteIdx);
      userSelect[spriteIdx].reload(streamName, spriteIdx, COMM_STATUS_ACTSOURCEDNT);

      // VTT subtitle track
      vttIdx = meta.addTrack();
      meta.setType(vttIdx, "meta");
      meta.setCodec(vttIdx, "subtitle");
      meta.setID(vttIdx, vttIdx);
      meta.markUpdated(vttIdx);
      userSelect[vttIdx].reload(streamName, vttIdx, COMM_STATUS_ACTSOURCEDNT);

      INFO_MSG("Thumbnail tracks created: sprite=%zu vtt=%zu (grid %ux%u = %ux%u)",
               spriteIdx, vttIdx, gridCols, gridRows, gridW, gridH);
    }

    /// Compose the 10x10 grid from cached thumbnails, encode as JPEG, generate VTT
    void composeAndBuffer(){
      initTracks();

      uint32_t totalCells = gridCols * gridRows;
      uint32_t gridW = thumbWidth * gridCols;
      uint32_t gridH = thumbHeight * gridRows;

      // Copy thumbCache under lock
      std::map<uint64_t, ThumbFrame> localCache;
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

      uint64_t duration = lastMs - firstMs;
      double cellDuration = (double)duration / totalCells;

      // Allocate RGB buffer for the full grid
      std::vector<uint8_t> gridRgb(gridW * gridH * 3, 0);

      // For each cell, find the nearest cached thumbnail
      for (uint32_t i = 0; i < totalCells; i++){
        uint64_t cellMidMs = firstMs + (uint64_t)(cellDuration * (i + 0.5));

        // Find nearest thumbnail by timestamp
        auto it = localCache.lower_bound(cellMidMs);
        if (it == localCache.end()){
          --it; // use last
        }else if (it != localCache.begin()){
          auto prev = std::prev(it);
          if (cellMidMs - prev->first < it->first - cellMidMs){it = prev;}
        }

        if (it->second.rgb.size() != thumbWidth * thumbHeight * 3){continue;}

        // Calculate grid position
        uint32_t col = i % gridCols;
        uint32_t row = i / gridCols;
        uint32_t xOff = col * thumbWidth;
        uint32_t yOff = row * thumbHeight;

        // Blit thumbnail into grid
        for (uint32_t y = 0; y < thumbHeight; y++){
          memcpy(&gridRgb[(yOff + y) * gridW * 3 + xOff * 3],
                 &it->second.rgb[y * thumbWidth * 3], thumbWidth * 3);
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
      int ret = avcodec_send_frame(jpegCtx, frame);
      if (ret >= 0){
        ret = avcodec_receive_packet(jpegCtx, pkt);
        if (ret >= 0){
          // Buffer the JPEG sprite sheet
          thisIdx = spriteIdx;
          thisTime = lastMs;
          bufferLivePacket(thisTime, 0, spriteIdx, (const char *)pkt->data, pkt->size, 0, true);
          INFO_MSG("Buffered sprite sheet: %d bytes at %" PRIu64 "ms", pkt->size, thisTime);
        }else{
          ERROR_MSG("MJPEG encode receive failed: %d", ret);
        }
      }else{
        ERROR_MSG("MJPEG encode send failed: %d", ret);
      }

      av_packet_free(&pkt);
      av_frame_free(&frame);
      avcodec_free_context(&jpegCtx);

      // Generate and buffer VTT
      std::stringstream vtt;
      vtt << "WEBVTT\n\n";
      for (uint32_t i = 0; i < totalCells; i++){
        uint64_t startMs = firstMs + (uint64_t)(cellDuration * i);
        uint64_t endMs = firstMs + (uint64_t)(cellDuration * (i + 1));
        uint32_t col = i % gridCols;
        uint32_t row = i / gridCols;
        uint32_t x = col * thumbWidth;
        uint32_t y = row * thumbHeight;

        // Format timestamps HH:MM:SS.mmm
        auto fmtTime = [](uint64_t ms) -> std::string{
          char buf[32];
          uint32_t h = ms / 3600000;
          uint32_t m = (ms / 60000) % 60;
          uint32_t s = (ms / 1000) % 60;
          uint32_t f = ms % 1000;
          snprintf(buf, sizeof(buf), "%02u:%02u:%02u.%03u", h, m, s, f);
          return std::string(buf);
        };

        vtt << fmtTime(startMs) << " --> " << fmtTime(endMs) << "\n";
        vtt << "/" << streamName << ".jpg?track=" << spriteIdx << "#xywh=" << x << "," << y
            << "," << thumbWidth << "," << thumbHeight << "\n\n";
      }

      std::string vttStr = vtt.str();
      thisIdx = vttIdx;
      thisTime = lastMs;
      bufferLivePacket(thisTime, 0, vttIdx, vttStr.data(), vttStr.size(), 0, true);
      INFO_MSG("Buffered VTT: %zu bytes", vttStr.size());
    }

    void streamMainLoop(){
      Comms::Connections statComm;
      while (config->is_active && co.is_active){
        // Wait for signal from source or timeout for periodic regen
        {
          std::unique_lock<std::mutex> lk(thumbMutex);
          thumbCV.wait_for(lk, std::chrono::milliseconds(regenInterval),
                           [&](){return vodDone || !co.is_active || !config->is_active;});
          if (!co.is_active || !config->is_active){return;}
        }

        // Check for shutdown requests
        if (spriteIdx != INVALID_TRACK_ID && !userSelect.count(spriteIdx)){
          userSelect[spriteIdx].reload(streamName, spriteIdx, COMM_STATUS_ACTSOURCEDNT);
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

    bool decodeAndScale(char *data, size_t len, ThumbFrame &out){
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

      out.rgb.resize(thumbWidth * thumbHeight * 3);
      uint8_t *dstSlice[1] = {out.rgb.data()};
      int dstStride[1] = {(int)(thumbWidth * 3)};
      sws_scale(scaleCtx, rawFrame->data, rawFrame->linesize, 0, rawFrame->height, dstSlice,
                dstStride);
      out.timeMs = thisTime;
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

      // Init decoder on first keyframe
      if (!initDecoder()){return;}

      // Track buffer range
      {
        std::lock_guard<std::mutex> lk(thumbMutex);
        bufferFirstMs = M.getFirstms(thisIdx);
        bufferLastMs = M.getLastms(thisIdx);
        bootMsOffset = M.getBootMsOffset();
        isVod = M.getVod();
      }

      // Decode and scale this keyframe
      ThumbFrame frame;
      if (decodeAndScale(thisData, thisDataLen, frame)){
        std::lock_guard<std::mutex> lk(thumbMutex);
        thumbCache[thisTime] = std::move(frame);
      }
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

      // For live: prune old entries, notify sink, then continue reading
      {
        std::lock_guard<std::mutex> lk(thumbMutex);
        uint64_t newFirstMs = bufferFirstMs;
        auto it = thumbCache.begin();
        while (it != thumbCache.end()){
          if (it->first < newFirstMs){
            it = thumbCache.erase(it);
          }else{
            break;
          }
        }
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
      genopts["track_select"]["default"] = "video=smallfirst";
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

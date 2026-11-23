#include "input_av.h"

#include "../../lib/av_codec_map.h"

#include <mist/ac3.h>
#include <mist/defines.h>
#include <mist/orientation.h>
#include <mist/stream.h>

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
extern "C" {
#include <libavutil/display.h>
}

namespace Mist{
  InputAV::InputAV(Util::Config *cfg) : Input(cfg){
    pFormatCtx = 0;
    capa["name"] = "AV";
    capa["desc"] =
        "This input uses libavformat to read any type of file. Unfortunately this input cannot be "
        "redistributed, but it is a great tool for testing the other file-based inputs against.";
    capa["source_match"] = "/*";
    capa["source_prefill"] = "/";
    capa["source_syntax"] = "/[path/to/][file_name]";
    capa["source_help"] = "Location where MistServer can find the input file.";
    capa["source_file"] = "$source";
    capa["priority"] = 1;
    capa["codecs"].null();
#if (LIBAVFORMAT_VERSION_MAJOR < 59)
    av_register_all();
#endif
    const AVCodec *cInfo = 0;
#if (LIBAVCODEC_VERSION_MAJOR < 59)
    while ((cInfo = av_codec_next(cInfo)) != 0){
#else
    void *i = 0;
    while ((cInfo = av_codec_iterate(&i))) {
#endif
      if (cInfo->type == AVMEDIA_TYPE_VIDEO){capa["codecs"]["video"].append(cInfo->name);}
      if (cInfo->type == AVMEDIA_TYPE_AUDIO){capa["codecs"]["audio"].append(cInfo->name);}
      if (cInfo->type == AVMEDIA_TYPE_SUBTITLE){capa["codecs"]["subtitle"].append(cInfo->name);}
    }
  }

  InputAV::~InputAV(){
    if (pFormatCtx){avformat_close_input(&pFormatCtx);}
  }

  bool InputAV::checkArguments(){
    if (config->getString("input") == "-"){
      Util::logExitReason(ER_FORMAT_SPECIFIC, "Input from stdin not yet supported");
      return false;
    }
    if (!config->getString("streamname").size()){
      if (config->getString("output") == "-"){
        Util::logExitReason(ER_FORMAT_SPECIFIC, "Output to stdout not yet supported");
        return false;
      }
    }else{
      if (config->getString("output") != "-"){
        Util::logExitReason(ER_FORMAT_SPECIFIC, "File output in player mode not supported");
        return false;
      }
    }
    return true;
  }

  bool InputAV::preRun(){
    // make sure all av inputs are registered properly, just in case
    // the constructor already does this, but under windows it doesn't remember that it has.
    // Very sad, that. We may need to get windows some medication for it.
#if (LIBAVFORMAT_VERSION_MAJOR < 59)
    av_register_all();
#endif

    // close any already open files
    if (pFormatCtx){
      avformat_close_input(&pFormatCtx);
      pFormatCtx = 0;
    }

    // Open video file
    int ret = avformat_open_input(&pFormatCtx, config->getString("input").c_str(), NULL, NULL);
    if (ret != 0){
      char errstr[300];
      av_strerror(ret, errstr, 300);
      Util::logExitReason(ER_READ_START_FAILURE, "Could not open file: %s", errstr);
      return false; // Couldn't open file
    }

    // Retrieve stream information
    ret = avformat_find_stream_info(pFormatCtx, NULL);
    if (ret < 0){
      char errstr[300];
      av_strerror(ret, errstr, 300);
      Util::logExitReason(ER_FORMAT_SPECIFIC, "Could not find stream info: %s", errstr);
      return false;
    }
    return true;
  }

  bool InputAV::readHeader(){
    if (!meta || (needsLock() && isSingular())){
      meta.reInit(isSingular() ? streamName : "");
    }
    for (unsigned int i = 0; i < pFormatCtx->nb_streams;++i){
      AVStream *strm = pFormatCtx->streams[i];
      size_t idx = meta.addTrack();
      meta.setID(idx, i);
      const char *mappedCodec = AVCodecMap::mistCodec(strm->codecpar->codec_id);
      if (*mappedCodec) {
        meta.setCodec(idx, mappedCodec);
      } else {
        const AVCodecDescriptor *desc = avcodec_descriptor_get(strm->codecpar->codec_id);
        meta.setCodec(idx, desc && desc->name ? desc->name : "?");
      }
      if (strm->codecpar->extradata_size){
        meta.setInit(idx, std::string((char *)strm->codecpar->extradata, strm->codecpar->extradata_size));
      }
      if (strm->codecpar->codec_type == AVMEDIA_TYPE_VIDEO){
        meta.setType(idx, "video");
        if (strm->avg_frame_rate.den && strm->avg_frame_rate.num){
          meta.setFpks(idx, (strm->avg_frame_rate.num * 1000) / strm->avg_frame_rate.den);
        }else{
          meta.setFpks(idx, 0);
        }
        const uint8_t *matrix = 0;
#if LIBAVFORMAT_VERSION_MAJOR >= 61
        const AVPacketSideData *side =
          av_packet_side_data_get(strm->codecpar->coded_side_data, strm->codecpar->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX);
        if (side && side->size >= 9 * sizeof(int32_t)) { matrix = side->data; }
#elif LIBAVFORMAT_VERSION_MAJOR >= 59
        size_t matrixSize = 0;
        matrix = av_stream_get_side_data(strm, AV_PKT_DATA_DISPLAYMATRIX, &matrixSize);
        if (matrixSize < 9 * sizeof(int32_t)) { matrix = 0; }
#else
        int matrixSize = 0;
        matrix = av_stream_get_side_data(strm, AV_PKT_DATA_DISPLAYMATRIX, &matrixSize);
        if (matrixSize < 9 * sizeof(int32_t)) { matrix = 0; }
#endif
        if (matrix) {
          const int32_t *display = (const int32_t *)matrix;
          Mist::Orientation orientation;
          if (Mist::Orientation::fromMatrix(display[0], display[1], display[3], display[4], orientation)) {
            meta.setRotation(idx, orientation.rotation);
            meta.setMirror(idx, orientation.mirror);
          } else {
            WARN_MSG("Ignoring non-D4 video display matrix");
          }
        }
        meta.setWidth(idx, strm->codecpar->width);
        meta.setHeight(idx, strm->codecpar->height);
      }
      if (strm->codecpar->codec_type == AVMEDIA_TYPE_AUDIO){
        meta.setType(idx, "audio");
        meta.setRate(idx, strm->codecpar->sample_rate);
        meta.setSize(idx, strm->codecpar->frame_size);
#if (LIBAVUTIL_VERSION_MAJOR < 57 || (LIBAVUTIL_VERSION_MAJOR == 57 && LIBAVUTIL_VERSION_MINOR < 24))
        meta.setChannels(idx, strm->codecpar->channels);
#else
        meta.setChannels(idx, strm->codecpar->ch_layout.nb_channels);
#endif
      }
    }

    AVPacket packet;
    while (av_read_frame(pFormatCtx, &packet) >= 0){
      AVStream *strm = pFormatCtx->streams[packet.stream_index];
      long long packTime = (packet.dts * 1000 * strm->time_base.num / strm->time_base.den);
      long long packOffset = 0;
      bool isKey = false;
      if (packTime < 0){packTime = 0;}
      size_t idx = meta.trackIDToIndex(packet.stream_index);
      if (packet.flags & AV_PKT_FLAG_KEY && M.getType(idx) != "audio"){
        isKey = true;
      }
      if (packet.pts != AV_NOPTS_VALUE && packet.pts != packet.dts){
        const long long presentationTime = packet.pts * 1000 * strm->time_base.num / strm->time_base.den;
        packOffset = (presentationTime < 0 ? 0 : presentationTime) - packTime;
      }
      if (M.getCodec(idx) == "EAC-3" && !AC3::validDEC3(M.getInit(idx))) {
        const AC3::Header header = AC3::parse(packet.data, packet.size);
        if (header.valid && header.enhanced && header.streamType != 1) {
          meta.setInit(idx, AC3::makeDEC3(header));
          meta.setRate(idx, header.rate);
          meta.setChannels(idx, header.channels);
        }
      }
      meta.update(packTime, packOffset, idx, packet.size, packet.pos, isKey);
      av_packet_unref(&packet);
    }
    return true;
  }

  void InputAV::getNext(size_t wantIdx){
    AVPacket packet;
    while (av_read_frame(pFormatCtx, &packet) >= 0){
      // filter tracks we don't care about
      size_t idx = meta.trackIDToIndex(packet.stream_index);
      if (idx == INVALID_TRACK_ID){continue;}
      if (wantIdx != INVALID_TRACK_ID && idx != wantIdx){continue;}
      AVStream *strm = pFormatCtx->streams[packet.stream_index];
      long long packTime = (packet.dts * 1000 * strm->time_base.num / strm->time_base.den);
      long long packOffset = 0;
      bool isKey = false;
      if (packTime < 0){packTime = 0;}
      if (packet.flags & AV_PKT_FLAG_KEY && M.getType(idx) != "audio"){
        isKey = true;
      }
      if (packet.pts != AV_NOPTS_VALUE && packet.pts != packet.dts){
        const long long presentationTime = packet.pts * 1000 * strm->time_base.num / strm->time_base.den;
        packOffset = (presentationTime < 0 ? 0 : presentationTime) - packTime;
      }
      thisTime = packTime;
      thisIdx = idx;
      thisPacket.genericFill(packTime, packOffset, packet.stream_index,
                             (const char *)packet.data, packet.size, 0, isKey);
      av_packet_unref(&packet);
      return; // success!
    }
    thisPacket.null();
    preRun();
    // failure :-(
    Util::logExitReason(ER_UNKNOWN, "getNext failed");
  }

  void InputAV::seek(uint64_t seekTime, size_t idx){
    int stream_index = av_find_default_stream_index(pFormatCtx);
    // Convert ts to frame
    unsigned long long reseekTime =
        av_rescale(seekTime, pFormatCtx->streams[stream_index]->time_base.den,
                   pFormatCtx->streams[stream_index]->time_base.num);
    reseekTime /= 1000;
    unsigned long long seekStreamDuration = pFormatCtx->streams[stream_index]->duration;
    int flags = AVSEEK_FLAG_BACKWARD;
    if (reseekTime > 0 && reseekTime < seekStreamDuration){
      flags |= AVSEEK_FLAG_ANY; // H.264 I frames don't always register as "key frames" in FFmpeg
    }
    int ret = av_seek_frame(pFormatCtx, stream_index, reseekTime, flags);
    if (ret < 0){ret = av_seek_frame(pFormatCtx, stream_index, reseekTime, AVSEEK_FLAG_ANY);}
  }

}// namespace Mist

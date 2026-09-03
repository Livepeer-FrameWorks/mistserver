
#include "output_jpegreceiver.h"

#include <mist/defines.h>
#include <mist/h264.h>
#include <mist/http_parser.h>
#include <mist/mp4_generic.h>
#include <mist/nal.h>
#include <mist/stream.h>
#include <mist/triggers.h>
#include <mist/url.h>

#include <algorithm>

namespace Mist {
  OutJPEGReceiver::OutJPEGReceiver(Socket::Connection & conn, Util::Config & _cfg, JSON::Value & _capa)
    : Output(conn, _cfg, _capa), trkIdx(INVALID_TRACK_ID), codec_H264(0), codec_JPEG(0), context_H264(0),
      context_JPEG(0), frame_JPEG(0), offset(0), isKey(false) {
    wantRequest = true;
    parseData = false;
    // Initialise libav JPEG decoder
    frame_JPEG = av_frame_alloc();
    if (!frame_JPEG) {
      FAIL_MSG("Could not allocate video frame");
      config->is_active = false;
      return;
    }
    codec_JPEG = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
    if (!codec_JPEG) {
      FAIL_MSG("MJPEG codec not found");
      config->is_active = false;
      return;
    }
    context_JPEG = avcodec_alloc_context3(codec_JPEG);
    if (!context_JPEG) {
      FAIL_MSG("Could not allocate JPEG context");
      config->is_active = false;
      return;
    }
    if (avcodec_open2(context_JPEG, codec_JPEG, NULL) < 0) {
      FAIL_MSG("Could not open JPEG codec");
      config->is_active = false;
      return;
    }
    // Initialise libav H264 encoder
    codec_H264 = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!codec_H264) {
      FAIL_MSG("H264 codec not found");
      config->is_active = false;
      return;
    }
    // Turn this Output into an Input
    if (!allowPush("")) {
      FAIL_MSG("Pushing not allowed");
      config->is_active = false;
      return;
    }
    // Add a single track and init some metadata
    trkIdx = meta.addTrack();
    if (trkIdx == INVALID_TRACK_ID) {
      FAIL_MSG("Could not add H264 track");
      config->is_active = false;
      return;
    }
    meta.setType(trkIdx, "video");
    meta.setCodec(trkIdx, "H264");
    meta.setID(trkIdx, 1);
    offset = M.getBootMsOffset();
    myConn.setBlocking(false);
    if (!userSelect.count(trkIdx)) {
      userSelect[trkIdx].reload(streamName, trkIdx, COMM_STATUS_ACTIVE | COMM_STATUS_SOURCE);
    }
    INFO_MSG("H264 track index is %zu", trkIdx);
  }

  OutJPEGReceiver::~OutJPEGReceiver() {
    // Free libav H264 encoder and JPEG decoder
    av_frame_free(&frame_JPEG);
    avcodec_free_context(&context_JPEG);
    avcodec_free_context(&context_H264);
    if (trkIdx != INVALID_TRACK_ID && M) { meta.abandonTrack(trkIdx); }
  }

  void OutJPEGReceiver::init(Util::Config *cfg, JSON::Value & capa) {
    Output::init(cfg, capa);
    capa["name"] = "JPEGReceiver";
    capa["friendly"] = "JPEG frames over raw TCP";
    capa["desc"] = "Accepts JPEG frames on a TCP connection and converts them into a h264 track";
    capa["deps"] = "";
    capa["required"]["streamname"]["name"] = "Stream";
    capa["required"]["streamname"]["help"] = "What streamname to serve. For multiple streams, add "
                                             "this protocol multiple times using different ports.";
    capa["required"]["streamname"]["type"] = "str";
    capa["required"]["streamname"]["option"] = "--stream";
    capa["required"]["streamname"]["short"] = "s";

    capa["optional"]["gopsize"]["name"] = "GOP Size";
    capa["optional"]["gopsize"]["help"] = "Amount of frames before a new keyframe is created.";
    capa["optional"]["gopsize"]["option"] = "--gopsize";
    capa["optional"]["gopsize"]["short"] = "G";
    capa["optional"]["gopsize"]["type"] = "uint";
    capa["optional"]["gopsize"]["default"] = 40;
    capa["optional"]["gopsize"]["min"] = 1;

    capa["optional"]["bitrate"]["name"] = "Bitrate";
    capa["optional"]["bitrate"]["help"] = "Target bitrate for the H264 encoder.";
    capa["optional"]["bitrate"]["option"] = "--bitrate";
    capa["optional"]["bitrate"]["short"] = "B";
    capa["optional"]["bitrate"]["type"] = "uint";
    capa["optional"]["bitrate"]["unit"] = "bits per second";
    capa["optional"]["bitrate"]["default"] = 1000000;

    capa["codecs"][0u][0u].append("H264");
    cfg->addConnectorOptions(3457, capa); ///< Allows listening on TCP port
  }

  void OutJPEGReceiver::requestHandler(bool readable) {
    if (readable) {
      if (myConn.spool()) {
        while (myConn.Received().size()) {
          const std::string & received = myConn.Received().get();
          if (received.size() > 32 * 1024 * 1024 || bufferJPEG.size() > 32 * 1024 * 1024 - received.size() ||
              !bufferJPEG.append(received)) {
            FAIL_MSG("JPEG receive buffer exceeded 32 MiB");
            config->is_active = false;
            return;
          }
          myConn.Received().get().clear();
        }
      }
    } else {
      Util::sleep(10);
      return;
    }
    while (bufferJPEG.size()) {
      JPEG::ScanResult frame = JPEG::scanFrame((const uint8_t *)(const char *)bufferJPEG, bufferJPEG.size(), 16 * 1024 * 1024);
      if (frame.status == JPEG::NEED_MORE) { return; }
      if (frame.status == JPEG::DISCARD_INVALID || frame.status == JPEG::DISCARD_OVERSIZED) {
        WARN_MSG("Discarding %zu bytes while resynchronizing JPEG input%s", frame.bytes,
                 frame.status == JPEG::DISCARD_OVERSIZED ? " (frame too large)" : "");
        if (!frame.bytes) {
          config->is_active = false;
          return;
        }
        bufferJPEG.shift(frame.bytes);
        continue;
      }

      VERYHIGH_MSG("Found JPEG frame of %zuB", frame.bytes);
      bool decoded = decodeJPEG((const uint8_t *)(const char *)bufferJPEG, frame.bytes);
      bufferJPEG.shift(frame.bytes);
      if (!decoded) { continue; }
      if (!encodeH264()) { return; }
      while (retrievePacket()) {
        if (bufferH264.size()) {
          thisTime = std::max<uint64_t>(Util::bootMS() - offset, thisTime + 1);
          sendH264();
        }
      }
    }
  }

  std::string OutJPEGReceiver::getStatsName() {
    if (!parseData) {
      return "INPUT:" + capa["name"].asStringRef();
    } else {
      return Output::getStatsName();
    }
  }

  /// \brief Prints out libav error codes
  void OutJPEGReceiver::printError(std::string preamble, int code) {
    char err[128];
    av_strerror(code, err, sizeof(err));
    ERROR_MSG("%s: `%s` (%i)", preamble.c_str(), err, code);
  }

  /// \brief Decodes one complete JPEG frame.
  /// \return true if the frame was decoded successfully, else returns false
  bool OutJPEGReceiver::decodeJPEG(const uint8_t *data, size_t size) {
    AVPacket *packet_JPEG = av_packet_alloc();
    if (!packet_JPEG) {
      FAIL_MSG("Could not allocate JPEG packet");
      return false;
    }
    int ret = av_new_packet(packet_JPEG, size);
    if (ret < 0) {
      av_packet_free(&packet_JPEG);
      printError("Could not allocate JPEG packet data", ret);
      return false;
    }
    memcpy(packet_JPEG->data, data, size);
    // Decode JPEG
    ret = avcodec_send_packet(context_JPEG, packet_JPEG);
    av_packet_free(&packet_JPEG);
    if (ret < 0) {
      printError("Error sending a packet for decoding", ret);
      return false;
    }
    ret = avcodec_receive_frame(context_JPEG, frame_JPEG);
    if (ret < 0) {
      printError("Error during decoding", ret);
      return false;
    }
    return true;
  }

  /// \brief Transcodes JPEG frames contained in frame_JPEG to H264
  ///   Allocates a new H264 context if it does not exist yet
  /// \return true if the frame was encoded successfully, else returns false
  bool OutJPEGReceiver::encodeH264() {
    // Prepare MJPEG encoder
    if (!context_H264) {
      INFO_MSG("Allocating H264 context");
      context_H264 = avcodec_alloc_context3(codec_H264);
      if (!context_H264) {
        FAIL_MSG("Could not allocate H264 context");
        return false;
      }
      const int fps = 30;
      context_H264->bit_rate = config->getInteger("bitrate");
      context_H264->time_base = (AVRational){fps, 1};
      context_H264->codec_type = AVMEDIA_TYPE_VIDEO;
      context_H264->pix_fmt = AV_PIX_FMT_YUV420P;
      context_H264->height = context_JPEG->height;
      context_H264->width = context_JPEG->width;
      context_H264->qmin = 40;
      context_H264->qmax = 40;
      context_H264->framerate = (AVRational){fps, 1};
#if defined(LIBAVCODEC_VERSION_MAJOR) && LIBAVCODEC_VERSION_MAJOR >= 61
      context_H264->profile = AV_PROFILE_H264_BASELINE;
#elif defined(FF_PROFILE_H264_BASELINE)
      context_H264->profile = FF_PROFILE_H264_BASELINE;
#endif
      context_H264->gop_size = config->getInteger("gopsize");
      context_H264->max_b_frames = 0;
      context_H264->has_b_frames = false;
      context_H264->refs = 2;
      context_H264->slices = 0;
      context_H264->codec_id = codec_H264->id;
      context_H264->compression_level = 4;
      context_H264->flags &= ~AV_CODEC_FLAG_CLOSED_GOP;

      // Set "veryfast" preset
      AVDictionary *codec_options(0);
      av_dict_set(&codec_options, "preset", "veryfast", 0);
      av_dict_set(&codec_options, "tune", "zerolatency", 0);

      int ret = avcodec_open2(context_H264, codec_H264, &codec_options);
      av_dict_free(&codec_options);
      if (ret < 0) {
        printError("Could not open H264 codec context", ret);
        return false;
      }
      INFO_MSG("Using %s H264 encoder", codec_H264->name);
    }
    frame_JPEG->pict_type = AV_PICTURE_TYPE_P;
    int ret = avcodec_send_frame(context_H264, frame_JPEG);
    if (ret < 0) {
      printError("Unable to send frame to the encoder", ret);
      return false;
    }
    return true;
  }

  /// \brief Retrieves a H264 packet contained in context_H264
  /// \return true if the packet was retrieved successfully, else returns false
  bool OutJPEGReceiver::retrievePacket() {
    AVPacket *packet_h264 = av_packet_alloc();
    if (!packet_h264) {
      FAIL_MSG("Could not allocate H264 packet");
      return false;
    }
    int ret = avcodec_receive_packet(context_H264, packet_h264);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
      HIGH_MSG("Encoder requires more input frames...");
      av_packet_free(&packet_h264);
      return false;
    } else if (ret < 0) {
      av_packet_free(&packet_h264);
      printError("Unable to encode H264", ret);
      return false;
    }
    isKey = packet_h264->flags & AV_PKT_FLAG_KEY;
    // Add JPEG image to the buffer
    bufferH264.assign(packet_h264->data, packet_h264->size);
    av_packet_free(&packet_h264);
    return true;
  }

  /// \brief Buffers H264 packets contained in bufferH264
  /// Overwrites Init data for each packet it buffers
  void OutJPEGReceiver::sendH264() {
    if (isKey) {
      INFO_MSG("Buffering %zuB H264 keyframe @%" PRIu64 "ms", bufferH264.size(), thisTime);
    } else {
      VERYHIGH_MSG("Buffering %zuB H264 packet @%" PRIu64 "ms", bufferH264.size(), thisTime);
    }
    thisIdx = trkIdx;
    thisPacket.null();

    const char *bufIt = bufferH264;
    uint64_t bufSize = bufferH264.size();
    const char *nextPtr;
    const char *pesEnd = bufferH264 + bufSize;
    uint32_t nalSize = 0;

    nextPtr = nalu::scanAnnexB(bufIt, bufSize);
    if (!nextPtr) {
      WARN_MSG("Unable to find AnnexB data in the H264 buffer. Clearing buffer");
      bufferH264.truncate(0);
      return;
    }
    while (nextPtr < pesEnd) {
      if (!nextPtr) { nextPtr = pesEnd; }
      // Calculate size of NAL unit, removing null bytes from the end
      nalSize = nalu::nalEndPosition(bufIt, nextPtr - bufIt) - bufIt;
      if (nalSize) {
        // If we don't have a packet yet, init an empty packet
        if (!thisPacket) { thisPacket.genericFill(thisTime, 0, 1, 0, 0, 0, isKey); }
        // Set PPS/SPS info
        uint8_t typeNal = bufIt[0] & 0x1F;
        if (typeNal == 0x07) {
          spsInfo.assign(std::string(bufIt, nextPtr - bufIt));
        } else if (typeNal == 0x08) {
          ppsInfo.assign(std::string(bufIt, nextPtr - bufIt));
        }
        thisPacket.appendNal(bufIt, nalSize);
      }
      if (((nextPtr - bufIt) + 3) >= bufSize) { break; } // end of the line
      bufSize -= ((nextPtr - bufIt) + 3); // decrease the total size
      bufIt = nextPtr + 3;
      nextPtr = nalu::scanAnnexB(bufIt, bufSize);
    }
    setInit();
    bufferH264.truncate(0);
    if (!thisPacket) {
      WARN_MSG("Unable to parse H264 packet");
      return;
    }
    bufferLivePacket(thisPacket);
  }

  /// \brief Sets init data based on the last loaded SPS and PPS data
  void OutJPEGReceiver::setInit() {
    if (spsInfo.size() < 4 || !ppsInfo.size()) { return; }

    MP4::AVCC avccBox;
    avccBox.setVersion(1);
    avccBox.setProfile(spsInfo[1]);
    avccBox.setCompatibleProfiles(spsInfo[2]);
    avccBox.setLevel(spsInfo[3]);
    avccBox.setSPSCount(1);
    avccBox.setSPS(spsInfo, spsInfo.size());
    avccBox.setPPSCount(1);
    avccBox.setPPS(ppsInfo, ppsInfo.size());

    if (avccBox.payloadSize()) { meta.setInit(trkIdx, avccBox.payload(), avccBox.payloadSize()); }
  }
} // namespace Mist

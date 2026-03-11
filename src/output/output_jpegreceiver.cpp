
#include "output_jpegreceiver.h"
#include <mist/defines.h>
#include <mist/http_parser.h>
#include <mist/h264.h>
#include <mist/url.h>
#include <mist/mp4_generic.h>
#include <mist/nal.h>
#include <mist/triggers.h>
#include <mist/stream.h>

namespace Mist{
  OutJPEGReceiver::OutJPEGReceiver(Socket::Connection &conn, Util::Config & _cfg, JSON::Value & _capa) : Output(conn, _cfg, _capa){
      lastPos = 0;
      wantRequest = true;
      parseData = false;
      bufferValid = true;
      isKey = true;
      context_H264 = NULL;
      // Initialise libav JPEG decoder
      frame_JPEG = av_frame_alloc();
      if (!frame_JPEG) {
        ERROR_MSG("Could not allocate video frame");
        exit(1);
      }
      codec_JPEG = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
      if (!codec_JPEG) {
        ERROR_MSG("MJPEG Codec not found");
        exit(1);
      }
      context_JPEG = avcodec_alloc_context3(codec_JPEG);
      if (!context_JPEG) {
        ERROR_MSG("Could not allocate JPEG context");
        exit(1);
      }
      if (avcodec_open2(context_JPEG, codec_JPEG, NULL) < 0) {
        ERROR_MSG("Could not open JPEG codec");
        exit(1);
      }
      // Initialise libav H264 encoder
      codec_H264 = avcodec_find_encoder(AV_CODEC_ID_H264);
      if (!codec_H264) {
        ERROR_MSG("H264 Codec not found");
        exit(1);
      }
      // Turn this Output into an Input
      if (!allowPush("")){
        FAIL_MSG("Pushing not allowed");
        config->is_active = false;
        return;
      }
      initialize();
      // Add a single track and init some metadata
      trkIdx = meta.addTrack();
      meta.setType(trkIdx, "video");
      meta.setCodec(trkIdx, "H264");
      meta.setID(trkIdx, 1);
      offset = M.getBootMsOffset();
      myConn.setBlocking(false);
      if (trkIdx != INVALID_TRACK_ID && !userSelect.count(trkIdx)){
        userSelect[trkIdx].reload(streamName, trkIdx, COMM_STATUS_ACTIVE | COMM_STATUS_SOURCE);
      }
      INFO_MSG("H264 track index is %zu", trkIdx);
  }

  OutJPEGReceiver::~OutJPEGReceiver(){
    // Free libav H264 encoder and JPEG decoder
    av_frame_free(&frame_JPEG);
    avcodec_free_context(&context_JPEG);
    avcodec_free_context(&context_H264);
    if (trkIdx != INVALID_TRACK_ID && M){
      meta.abandonTrack(trkIdx);
    }
  }

  void OutJPEGReceiver::init(Util::Config *cfg, JSON::Value & capa){
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

    capa["optional"]["minfps"]["name"] = "Minimum FPS";
    capa["optional"]["minfps"]["help"] = "Will duplicate frames to keep at least this frame rate, if less JPEGs are received than this rate. Set to zero to always have exactly one H264 frame per JPEG received.";
    capa["optional"]["minfps"]["option"] = "--minfps";
    capa["optional"]["minfps"]["short"] = "F";
    capa["optional"]["minfps"]["type"] = "uint";
    capa["optional"]["minfps"]["default"] = 30;

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

  void OutJPEGReceiver::requestHandler(bool readable){
    static bool hasFrame = false;
    static uint64_t lastReceive = 0;
    if (readable && myConn.spool()){
      while (myConn.Received().size()){
        bufferJPEG.append(myConn.Received().get());
        myConn.Received().get().clear();
      }
    }else{
      if (hasFrame && config->getInteger("minfps") && lastReceive + (1000 / config->getInteger("minfps")) <= Util::bootMS()){
        lastReceive = Util::bootMS();
        if (!encodeH264()){return;}
        while(retrievePacket()){
          if (bufferH264.size()){
            thisTime = Util::bootMS() - offset;
            sendH264();
          }
        }
        return;
      }
      Util::sleep(10);
      return;
    }
    // Set packet time to the time we received JPEG data
    if (!bufferJPEG.size()){
      return;
    }
    // Calculate the size of the current JPEG frame, if we have any
    uint64_t frameSize = getFrameSize();
    if (!frameSize){
      return;
    }
    VERYHIGH_MSG("Found JPEG frame of %zuB", frameSize);
    lastPos = 0;
    if (!decodeJPEG(frameSize)){
      hasFrame = false;
      return;
    }
    hasFrame = true;
    lastReceive = Util::bootMS();
    if (!encodeH264()){return;}
    while(retrievePacket()){
      if (bufferH264.size()){
        lastReceive = Util::bootMS();
        thisTime = lastReceive - offset;
        sendH264();
      }
    }
  }

  std::string OutJPEGReceiver::getStatsName(){
    if (!parseData){
      return "INPUT:" + capa["name"].asStringRef();
    }else{
      return Output::getStatsName();
    }
  }

  /// \brief Skips to the next JPEG frame in the buffer
  /// Tries to find the start of a new JPEG frame. If it can find one, removes
  /// all data preceding that frame from the buffer and sets bufferValid to true
  /// Otherwise returns, keeping bufferValid to false
  void OutJPEGReceiver::skipFrame(){
    uint64_t maxPos = bufferJPEG.size();
    // Invalidate buffer
    bufferValid = false;
    // We need at least two bytes to parse any marker at the current position
    if (lastPos + 2 > maxPos){
      return;
    }
    // Increase lastPos until we've found the end of picture marker
    while (bufferJPEG[lastPos] != 0xFF || bufferJPEG[lastPos+1] != 0xD9){
      // Reached end of buffer, wait for more data
      if (lastPos + 2 > maxPos){
        return;
      }
      // Skip bytes until we reach a marker byte 0xFF
      if (bufferJPEG[lastPos] != 0xFF){
        lastPos++;
        continue;
      }
      // Parse marker
      uint64_t thisLen = parseMarker();
      // If we cannot parse it, increment lastPos and retry
      if (!thisLen){
        lastPos++;
        continue;
      }
      // If we need more data, abort for now
      if (thisLen >= maxPos){
        return;
      }
      // Check if this actually ends up at another marker byte
      if (bufferJPEG[lastPos+thisLen] != 0xFF){
        // Otherwise this might not have been an actual marker, so skip and retry
        lastPos++;
        continue;
      }
      // Else continue at the next marker
      lastPos = thisLen;
    }
    // Remove data including the current EOI marker and mark buffer as valid
    INFO_MSG("Skipping %zu bytes to reach the next JPEG frame", lastPos + 2);
    bufferJPEG.shift(lastPos + 2);
    lastPos = 0;
    bufferValid = true;
  }

  // Returns the start position of the next marker, after the current marker and it's payload
  // Return 0 if we were not able to successfully parse the marker for any reason
  // When the marker cannot be parsed due to invalid data, sets bufferValid to false
  uint64_t OutJPEGReceiver::parseMarker(){
    // We need at least two bytes to parse any marker at the current position
    if (lastPos + 2 > bufferJPEG.size()){
      return 0;
    }
    // First byte should always be 0xFF
    if (bufferJPEG[lastPos] != 0xFF){
      // In case we are already skipping a frame, don't spam the logs
      if (bufferValid){
        WARN_MSG("Marker starts with invalid character `%u`", bufferJPEG[lastPos]);
        bufferValid = false;
      }
      return 0;
    }
    // Next byte is the marker type
    uint8_t thisType = bufferJPEG[lastPos + 1];
    uint64_t tmpPos;
    switch (thisType){
      // Fixed size markers
      case 0xD8:
        INSANE_MSG("Found SOI marker");
        return lastPos + 2;
      case 0xDD:
        INSANE_MSG("Found DRI marker");
        return lastPos + 2 + 4;
      case 0xD9:
        INSANE_MSG("Found EOI marker");
        return lastPos + 2;

      // Variable size markers
      case 0xC0: // Start Of Frame (baseline DCT) (SOF0)
      case 0xC2: // Start Of Frame (progressive DCT) (SOF2)
      case 0xC4: // Define Huffman Tables (DHT)
      case 0xDB: // Define Quantization Tables (DQT)
      case 0xFE: // Comment (COM)
      case 0xE0: // Application-specific
      case 0xE1: // Application-specific
      case 0xE2: // Application-specific
        // Size is in the next two bytes (high then low) including the two bytes for the length, but not the two bytes for the marker
        tmpPos = ((uint16_t)bufferJPEG[lastPos+2] << 8) | bufferJPEG[lastPos+3];
        INSANE_MSG("Found variable sized marker of length %lu", tmpPos);
        return lastPos + 2 + tmpPos;
      
      // Entropy encoded data
      // Start Of Scan (SOS)
      case 0xDA:
      // Restart (RST0-RST7)
      case 0xD0:
      case 0xD1:
      case 0xD2:
      case 0xD3:
      case 0xD4:
      case 0xD5:
      case 0xD6:
      case 0xD7:
        // First calculate the size like above
        tmpPos = lastPos + 2 +( ((uint16_t)bufferJPEG[lastPos+2] << 8) | bufferJPEG[lastPos+3]);
        while (tmpPos + 1 < bufferJPEG.size()){
          // Next we need to find the next 0xFF byte not followed by 0x00 or reset markers 0xD0 through 0xD7
          if (bufferJPEG[tmpPos] == 0xFF){
            uint8_t nextByte = bufferJPEG[tmpPos+1];
            if (nextByte != 0x00 && (nextByte < 0xD0 || nextByte > 0xD7)){
              INSANE_MSG("Found Entropy encoded from %lu to %lu", lastPos, tmpPos);
              return tmpPos;
            }
          }
          tmpPos++;
          continue;
        }
        return 0;
      default:
        if (bufferValid){
          WARN_MSG("Found invalid marker %u %u @ pos %zu", bufferJPEG[lastPos], bufferJPEG[lastPos+1], lastPos);
          bufferValid = false;
        }
        return 0;
    };
    return 0;
  }

  // Returns the size of the JPEG frame from the start of the buffer
  // Else returns 0 to signal we need more buffer data
  uint64_t OutJPEGReceiver::getFrameSize(){
    uint64_t maxPos = bufferJPEG.size();
    if (lastPos + 2 > maxPos){
      return 0;
    }
    // Check if the buffer starts with a start of image 
    if (bufferJPEG[0] != 0xFF || bufferJPEG[1] != 0xD8){
      if (bufferValid){
        WARN_MSG("JPEG buffer starts with invalid characters %u %u. Expected SOI marker (0xFFD8)", bufferJPEG[lastPos], bufferJPEG[lastPos+1]);
        bufferValid = false;
        return 0;
      }
    }
    // Continue parsing markers until we reach an end of frame marker
    while (bufferJPEG[lastPos] != 0xFF || bufferJPEG[lastPos+1] != 0xD9){
      // Reached end of buffer, wait for more data
      if (lastPos + 2 > maxPos){
        return 0;
      }
      // If there is malformed data, we need to skip to the next JPEG frame
      if (!bufferValid){
        skipFrame();
        // If we couldn't, wait for more data
        if (!bufferValid){
          return 0;
        }
      }
      // Skip to the start of the next marker if possible
      uint64_t nextPos = parseMarker();
      if (!nextPos){
        return 0;
      }
      lastPos = nextPos;
    }
    // Return size including the current EOI marker
    return lastPos + 2;
  }

  /// \brief Prints out libav error codes
  void OutJPEGReceiver::printError(std::string preamble, int code){
    char err[128];
    av_strerror(code, err, sizeof(err));
    ERROR_MSG("%s: `%s` (%i)", preamble.c_str(), err, code);
  }

  /// \brief Decodes the JPEG frame at the start of the JPEG buffer
  /// \param brief: the amount of bytes to consume from the JPEG buffer
  /// \return true if the frame was decoded successfully, else returns false
  bool OutJPEGReceiver::decodeJPEG(uint64_t size){
    if (size > bufferJPEG.size()){
      return false;
    }
    // Packetize JPEG
    AVPacket *packet_JPEG = av_packet_alloc();
    av_new_packet(packet_JPEG, size);
    memcpy(packet_JPEG->data, bufferJPEG, size);
    bufferJPEG.shift(size);
    // Decode JPEG
    int ret = avcodec_send_packet(context_JPEG, packet_JPEG);
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
  bool OutJPEGReceiver::encodeH264(){
    // Prepare MJPEG encoder
    if (!context_H264) {
      INFO_MSG("Allocating H264 context");
      context_H264 = avcodec_alloc_context3(codec_H264);
      if (!context_H264) {
        ERROR_MSG("Could not allocate H264 context");
        exit(1);
      }
      int fps = config->getInteger("minfps");
      if (fps < 1){fps = 30;}
      context_H264->bit_rate = config->getInteger("bitrate");
      context_H264->time_base.num = fps;
      context_H264->time_base.den = 1;
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
      AVDictionary * codec_options(0);
      av_dict_set(&codec_options, "preset", "veryfast", 0);
      av_dict_set(&codec_options, "tune", "zerolatency", 0);

      int ret = avcodec_open2(context_H264, codec_H264, &codec_options);
      if (ret < 0) {
        printError("Could not open H264 codec context", ret);
        return false;
      }
      meta.setInit(trkIdx, (char *)context_H264->extradata, context_H264->extradata_size);
    }
    // Encode to H264. Force P frame to prevent keyframe-only outputs from appearing
    frame_JPEG->pict_type = AV_PICTURE_TYPE_P;
    // Increase frame counter
    frame_JPEG->pts++;
    int ret = avcodec_send_frame(context_H264, frame_JPEG);
    if (ret < 0) {
      printError("Unable to send frame to the encoder", ret);
      return false;
    }
    return true;
  }

  /// \brief Retrieves a H264 packet contained in context_H264
  /// \return true if the packet was retrieved successfully, else returns false
  bool OutJPEGReceiver::retrievePacket(){
    AVPacket* packet_h264 = av_packet_alloc();
    int ret = avcodec_receive_packet(context_H264, packet_h264);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF){
      HIGH_MSG("Encoder requires more input frames...");
      av_packet_free(&packet_h264);
      return false;
    }else if (ret < 0) {
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
  void OutJPEGReceiver::sendH264(){
    if (isKey){
      INFO_MSG("Buffering %zuB H264 keyframe @%zums", bufferH264.size(), thisTime);
    }else{
      VERYHIGH_MSG("Buffering %zuB H264 packet @%zums", bufferH264.size(), thisTime);
    }
    thisIdx = trkIdx;
    thisPacket.null();

    const char* bufIt = bufferH264;
    uint64_t bufSize = bufferH264.size();
    const char *nextPtr;
    const char *pesEnd = bufferH264 + bufSize;
    uint32_t nalSize = 0;

    nextPtr = nalu::scanAnnexB(bufIt, bufSize);
    if (!nextPtr){
      WARN_MSG("Unable to find AnnexB data in the H264 buffer. Clearing buffer");
      bufferH264.truncate(0);
      return;
    }
    while (nextPtr < pesEnd){
      if (!nextPtr){nextPtr = pesEnd;}
      // Calculate size of NAL unit, removing null bytes from the end
      nalSize = nalu::nalEndPosition(bufIt, nextPtr - bufIt) - bufIt;
      if (nalSize){
        // If we don't have a packet yet, init an empty packet
        if (!thisPacket){
          thisPacket.genericFill(thisTime, 0, 1, 0, 0, 0, isKey);
        }
        // Set PPS/SPS info
        uint8_t typeNal = bufIt[0] & 0x1F;
        if (typeNal == 0x07){
          spsInfo.assign(std::string(bufIt, (nextPtr - bufIt)));
        } else if (typeNal == 0x08){
          ppsInfo.assign(std::string(bufIt, (nextPtr - bufIt)));
        }
        thisPacket.appendNal(bufIt, nalSize);
      }
      if (((nextPtr - bufIt) + 3) >= bufSize){break;}// end of the line
      bufSize -= ((nextPtr - bufIt) + 3); // decrease the total size
      bufIt = nextPtr + 3;
      nextPtr = nalu::scanAnnexB(bufIt, bufSize);
    }
    setInit();
    bufferH264.truncate(0);
    if (!thisPacket){
      WARN_MSG("Unable to parse H264 packet");
      return;
    }
    bufferLivePacket(thisPacket);
  }

  /// \brief Sets init data based on the last loaded SPS and PPS data
  void OutJPEGReceiver::setInit(){
    if (!spsInfo.size() || !ppsInfo.size()){return;}

    MP4::AVCC avccBox;
    avccBox.setVersion(1);
    avccBox.setProfile(spsInfo[1]);
    avccBox.setCompatibleProfiles(spsInfo[2]);
    avccBox.setLevel(spsInfo[3]);
    avccBox.setSPSCount(1);
    avccBox.setSPS(spsInfo, spsInfo.size());
    avccBox.setPPSCount(1);
    avccBox.setPPS(ppsInfo, ppsInfo.size());

    if (avccBox.payloadSize()){meta.setInit(trkIdx, avccBox.payload(), avccBox.payloadSize());}
  }
}// namespace Mist

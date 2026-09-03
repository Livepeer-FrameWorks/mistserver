#include "input_rawgenerator.h"

namespace Mist {

  InputRawGenerator::InputRawGenerator(Util::Config *cfg) : Input(cfg) {
    capa["name"] = "RawGenerator";
    capa["desc"] = "Generates raw video and timed subtitles";
    capa["source_match"].append("rawgenerator*");
    capa["priority"] = 9;
    capa["codecs"]["video"].append("UYVY");
    capa["codecs"]["subtitle"].append("subtitle");
  }

  bool InputRawGenerator::readHeader() {
    bMs = Util::bootMS();
    meta.setBootMsOffset(bMs);
    meta.setUTCOffset(Util::unixMS());
    meta.setLive(true);
    meta.setVod(false);

    size_t staticSize = Util::pixfmtToSize("UYVY", 800, 600);
    vidIdx = meta.addTrack(0, 0, 0, 0, true, staticSize);
    if (vidIdx == INVALID_TRACK_ID || !vidPtr.allocate(staticSize) || !vidPtr.appendNull(staticSize)) {
      FAIL_MSG("Could not allocate raw video track");
      return false;
    }
    meta.setID(vidIdx, 1);
    meta.setType(vidIdx, "video");
    meta.setCodec(vidIdx, "UYVY");
    meta.setWidth(vidIdx, 800);
    meta.setHeight(vidIdx, 600);
    meta.setFpks(vidIdx, 50000);
    nextVid = 0;

    subIdx = meta.addTrack();
    if (subIdx == INVALID_TRACK_ID) {
      FAIL_MSG("Could not allocate subtitle track");
      return false;
    }
    meta.setID(subIdx, 3);
    meta.setType(subIdx, "meta");
    meta.setCodec(subIdx, "subtitle");
    meta.setLang(subIdx, "und");
    nextSub = 0;

    userSelect[vidIdx].reload(streamName, vidIdx, COMM_STATUS_ACTSOURCEDNT);
    userSelect[subIdx].reload(streamName, subIdx, COMM_STATUS_ACTSOURCEDNT);
    return true;
  }

  void InputRawGenerator::streamMainLoop() {
    uint64_t statTimer = 0;
    uint64_t startTime = Util::bootSecs();
    Comms::Connections statComm;

    while (config->is_active) {
      if (!bufferActive()) {
        Util::logExitReason(ER_SHM_LOST, "Buffer shut down");
        return;
      }
      if ((userSelect[vidIdx].getStatus() | userSelect[subIdx].getStatus()) & COMM_STATUS_REQDISCONNECT) {
        Util::logExitReason(ER_CLEAN_LIVE_BUFFER_REQ, "buffer requested shutdown");
        return;
      }

      thisTime = Util::bootMS() - bMs;

      while (nextVid <= thisTime) {
        bufferLivePacket(nextVid, 0, vidIdx, vidPtr, vidPtr.size(), 0, true);
        nextVid += 20;
      }

      while (nextSub <= thisTime) {

        time_t rawtime;
        char buffer[80];
        time(&rawtime);
        struct tm timebuf;
        struct tm *timeinfo = localtime_r(&rawtime, &timebuf);
        if (!timeinfo || !strftime(buffer, 80, "%Y-%m-%d-%H:%M:%S", timeinfo)) { buffer[0] = 0; }
        std::string subLine = buffer;
        bufferLivePacket(nextSub, 0, subIdx, subLine.data(), subLine.size(), 0, true);
        nextSub += 1000;
      }

      if (Util::bootSecs() - statTimer > 1) {
        // Connect to stats for INPUT detection
        if (!statComm) {
          statComm.reload(streamName, getConnectedBinHost(), JSON::Value(getpid()).asString(),
                          "INPUT:" + capa["name"].asStringRef(), "");
        }
        if (statComm) {
          if (statComm.getExit() || statComm.getStatus() & COMM_STATUS_REQDISCONNECT) {
            config->is_active = false;
            Util::logExitReason(ER_CLEAN_CONTROLLER_REQ, "received shutdown request from session");
            return;
          }
          uint64_t now = Util::bootSecs();
          statComm.setNow(now);
          statComm.setStream(streamName);
          statComm.setTime(now - startTime);
          statComm.setLastSecond(0);
          connStats(statComm);
        }

        statTimer = Util::bootSecs();
      }

      Util::sleep(20);
    }
  }

} // namespace Mist

#include "output_ts_base.h"
#include <mist/bitfields.h>

namespace Mist{
  TSOutput::TSOutput(Socket::Connection & conn, Util::Config & _cfg, JSON::Value & _capa)
    : TS_BASECLASS(conn, _cfg, _capa) {
    packCounter = 0;
    ts_from = 0;
    setBlocking(true);
    sendRepeatingHeaders = 0;
    lastHeaderTime = 0;
    maxSkipAhead = 0;
  }

  void TSOutput::fillPacket(const char *data, size_t dataLen, bool & firstPack, bool video, bool keyframe,
                            size_t pkgPid, uint16_t & contPkg) {
    do{
      if (!packData.getBytesFree()){
        if ((sendRepeatingHeaders && thisPacket.getTime() - lastHeaderTime > sendRepeatingHeaders) || !packCounter){

          std::set<size_t> selectedTracks;
          for (std::map<size_t, Comms::Users>::iterator it = userSelect.begin(); it != userSelect.end(); it++){
            selectedTracks.insert(it->first);
          }

          lastHeaderTime = thisPacket.getTime();
          TS::Packet tmpPack;
          tmpPack.FromPointer(TS::PAT);
          tmpPack.setContinuityCounter(++contPAT);
          sendTS(tmpPack.checkAndGetBuffer());
          sendTS(TS::createPMT(selectedTracks, M, ++contPMT,
                               [this](const DTSC::Meta & M, size_t idx) { return pidMapper(M, idx); }));
          sendTS(TS::createSDT(streamName, ++contSDT));
          packCounter += 3;
        }
        sendTS(packData.checkAndGetBuffer());
        packCounter++;
        packData.clear();
      }

      if (!dataLen) { return; }

      if (packData.getBytesFree() == 184){
        packData.clear();
        packData.setPID(pkgPid);
        packData.setContinuityCounter(++contPkg);
        if (firstPack){
          packData.setUnitStart(1);
          if (video){
            if (keyframe){
              packData.setRandomAccess(true);
              packData.setESPriority(true);
            }
            packData.setPCR(thisPacket.getTime() * 27000);
          }
          firstPack = false;
        }
      }

      size_t tmp = packData.fillFree(data, dataLen);
      data += tmp;
      dataLen -= tmp;
    } while (dataLen);
  }

  size_t TSOutput::pidMapper(const DTSC::Meta & M, size_t idx) {
    if (pidMap.size() != userSelect.size() || !pidMap.count(idx)) {
      std::set<size_t> selectedTracks;
      for (const auto & selection : userSelect) { selectedTracks.insert(selection.first); }
      pidMap = TS::buildPidMap(M, selectedTracks, targetParams);
    }
    // Return 0 if not found, or found mapping otherwise
    auto f = pidMap.find(idx);
    if (f == pidMap.end()) { return 0; }
    return f->second;
  }

  void TSOutput::updatePresentationDelay() {
    // TS requires PTS >= DTS. A common presentation delay makes signed source
    // composition offsets nonnegative without changing decode order or A/V sync.
    // Use the whole stream so HLS video variants and audio-only requests share a clock.
    for (const size_t track : M.getValidTracks()) {
      if (!M.trackLoaded(track) || M.getType(track) != "video" || M.hasEmbeddedFrames(track)) { continue; }
      const DTSC::Parts parts(M.parts(track));
      size_t & cursor = scannedParts[track];
      if (cursor < parts.getFirstValid() || cursor > parts.getEndValid()) { cursor = parts.getFirstValid(); }
      while (cursor < parts.getEndValid()) {
        const int64_t offset = parts.getOffset(cursor++);
        if (offset < 0 && (uint64_t)-offset > presentationDelay) { presentationDelay = -offset; }
      }
    }
    // A live packet may precede its part metadata.
    if (M.getType(thisIdx) == "video") {
      const int64_t offset = thisPacket.getInt("offset");
      if (offset < 0 && (uint64_t)-offset > presentationDelay) { presentationDelay = -offset; }
    }
  }

  void TSOutput::sendNext(){
    static uint64_t lastMeta = 0;
    if (Util::epoch() > lastMeta + 5){
      lastMeta = Util::epoch();
      if (selectDefaultTracks()){
        INFO_MSG("Track selection changed - resending headers and continuing");
        packCounter = 0;
        return;
      }
    }
    if (liveSeek(true)) { return; }
    if (!M.trackLoaded(thisIdx)){return;}
    // Get ready some data to speed up accesses
    std::string type = M.getType(thisIdx);
    std::string codec = M.getCodec(thisIdx);
    bool video = (type == "video");
    size_t pkgPid = pidMapper(M, thisIdx);
    bool &firstPack = first[thisIdx];
    uint16_t &contPkg = contCounters[pkgPid];
    uint64_t packTime = thisPacket.getTime();
    bool keyframe = thisPacket.getInt("keyframe");
    firstPack = true;
    char *dataPointer = 0;
    size_t dataLen = 0;
    thisPacket.getString("data", dataPointer, dataLen); // data

    if (codec == "rawts"){
      for (size_t i = 0; i+188 <= dataLen; i+=188){sendTS(dataPointer+i, 188);}
      return;
    }

    updatePresentationDelay();
    packTime *= 90;
    if (!video && type != "audio") { packTime += presentationDelay * 90; }
    std::string bs;
    // prepare bufferstring
    if (video){
      bool addInit = keyframe;
      bool addEndNal = true;
      if (codec == "H264" || codec == "HEVC"){
        uint32_t extraSize = 0;
        //Check if we need to skip sending some things
        if (codec == "H264"){
          size_t ctr = 0;
          char * ptr = dataPointer;
          while (ptr+4 < dataPointer+dataLen && ++ctr <= 5){
            switch (ptr[4] & 0x1f){
            case 0x07://init
            case 0x08://init
              addInit = false;
              break;
            case 0x09://new nal
              addEndNal = false;
              break;
            default: break;
            }
            ptr += Bit::btohl(ptr) + 4;
          }
        }

        if (addEndNal && codec == "H264"){extraSize += 6;}
        if (addInit){
          if (codec == "H264"){
            MP4::AVCC avccbox;
            avccbox.setPayload(M.getInit(thisIdx));
            bs = avccbox.asAnnexB();
            extraSize += bs.size();
          }
          if (codec == "HEVC"){
            MP4::HVCC hvccbox;
            hvccbox.setPayload(M.getInit(thisIdx));
            bs = hvccbox.asAnnexB();
            extraSize += bs.size();
          }
        }

        const uint32_t MAX_PES_SIZE = 65490 - 13;
        uint32_t ThisNaluSize = 0;
        uint32_t i = 0;
        int64_t offset = ((int64_t)thisPacket.getInt("offset") + (int64_t)presentationDelay) * 90;

        bs.clear();
        TS::Packet::getPESVideoLeadIn(bs,
            (((dataLen + extraSize) > MAX_PES_SIZE) ? 0 : dataLen + extraSize),
            packTime, offset, true, M.getBps(thisIdx));
        fillPacket(bs.data(), bs.size(), firstPack, video, keyframe, pkgPid, contPkg);

        // End of previous nal unit, if not already present
        if (addEndNal && codec == "H264"){
          fillPacket("\000\000\000\001\011\360", 6, firstPack, video, keyframe, pkgPid, contPkg);
        }
        // Init data, if keyframe and not already present
        if (addInit){
          if (codec == "H264"){
            MP4::AVCC avccbox;
            avccbox.setPayload(M.getInit(thisIdx));
            bs = avccbox.asAnnexB();
            fillPacket(bs.data(), bs.size(), firstPack, video, keyframe, pkgPid, contPkg);
          }
          /*LTS-START*/
          if (codec == "HEVC"){
            MP4::HVCC hvccbox;
            hvccbox.setPayload(M.getInit(thisIdx));
            bs = hvccbox.asAnnexB();
            fillPacket(bs.data(), bs.size(), firstPack, video, keyframe, pkgPid, contPkg);
          }
          /*LTS-END*/
        }
        size_t lenSize = 4;
        if (codec == "H264"){
          std::string init = M.getInit(thisIdx);
          if (init.size() > 4){lenSize = (init[4] & 3) + 1;}
        }
        while (i + lenSize < (unsigned int)dataLen){
          if (lenSize == 4){
            ThisNaluSize = Bit::btohl(dataPointer + i);
          }else if (lenSize == 2){
            ThisNaluSize = Bit::btohs(dataPointer + i);
          }else{
            ThisNaluSize = dataPointer[i];
          }
          if (ThisNaluSize + i + lenSize > dataLen){
            WARN_MSG("Too big NALU detected (%" PRIu32 " > %zu) - skipping!",
                     ThisNaluSize + i + 4, dataLen);
            break;
          }
          fillPacket("\000\000\000\001", 4, firstPack, video, keyframe, pkgPid, contPkg);
          fillPacket(dataPointer + i + lenSize, ThisNaluSize, firstPack, video, keyframe, pkgPid, contPkg);
          i += ThisNaluSize + lenSize;
        }
      }else{
        int64_t offset = ((int64_t)thisPacket.getInt("offset") + (int64_t)presentationDelay) * 90;
        bs.clear();
        TS::Packet::getPESVideoLeadIn(bs, 0, packTime, offset, true, M.getBps(thisIdx));
        fillPacket(bs.data(), bs.size(), firstPack, video, keyframe, pkgPid, contPkg);

        fillPacket(dataPointer, dataLen, firstPack, video, keyframe, pkgPid, contPkg);
      }
    }else if (type == "audio"){
      size_t tempLen = dataLen;
      if (codec == "AAC"){
        tempLen += 7;
        // Make sure TS timestamp is sample-aligned, if possible
        uint32_t freq = M.getRate(thisIdx);
        if (freq){
          uint64_t aacSamples = packTime * freq / 90000;
          //round to nearest packet, assuming all 1024 samples (probably wrong, but meh)
          aacSamples += 256;//Add a quarter frame of offset to encourage correct rounding
          aacSamples &= ~0x3FF;
          //Get closest 90kHz clock time to perfect sample alignment
          packTime = aacSamples * 90000 / freq;
        }
      }
      packTime += presentationDelay * 90;
      if (codec == "opus"){
        tempLen += 3 + (dataLen/255);
        bs = TS::Packet::getPESPS1LeadIn(tempLen, packTime, M.getBps(thisIdx));
        fillPacket(bs.data(), bs.size(), firstPack, video, keyframe, pkgPid, contPkg);
        bs = "\177\340";
        bs.append(dataLen/255, (char)255);
        bs.append(1, (char)(dataLen-255*(dataLen/255)));
        fillPacket(bs.data(), bs.size(), firstPack, video, keyframe, pkgPid, contPkg);
      }else{
        bs.clear();
        TS::Packet::getPESAudioLeadIn(bs, tempLen, packTime, M.getBps(thisIdx));
        fillPacket(bs.data(), bs.size(), firstPack, video, keyframe, pkgPid, contPkg);
        if (codec == "AAC"){
          bs = TS::getAudioHeader(dataLen, M.getInit(thisIdx));
          fillPacket(bs.data(), bs.size(), firstPack, video, keyframe, pkgPid, contPkg);
        }
      }
      fillPacket(dataPointer, dataLen, firstPack, video, keyframe, pkgPid, contPkg);
    }else if (type == "meta"){
      if (codec == "SCTE35") {
        if (dataLen && dataPointer[0] == '{') {
          uint64_t duration = 30000;
          JSON::Value cmd(PARSEJSON, dataPointer, dataLen);
          if (cmd.isMember("splice_out")) { duration = cmd["splice_out"].asInt(); }
          TS::SpliceInfoSection sis;
          sis.randomizeSpliceID();
          sis.writeSpliceDuration(duration);
          sis.writeCRC();
          fillPacket(sis.checkAndGetBuffer() + 4, 36, firstPack, false, true, pkgPid, contPkg);
        }
      } else {
        long unsigned int tempLen = dataLen;
        if (codec == "JSON") { tempLen += 2; }
        bs = TS::Packet::getPESMetaLeadIn(tempLen, packTime, M.getBps(thisIdx));
        fillPacket(bs.data(), bs.size(), firstPack, video, keyframe, pkgPid, contPkg);
        if (codec == "JSON") {
          char dLen[2];
          Bit::htobs(dLen, dataLen);
          fillPacket(dLen, 2, firstPack, video, keyframe, pkgPid, contPkg);
        }
        fillPacket(dataPointer, dataLen, firstPack, video, keyframe, pkgPid, contPkg);
      }
    }
    if (packData.getBytesFree() < 184){
      packData.addStuffing();
      fillPacket(0, 0, firstPack, video, keyframe, pkgPid, contPkg);
    }
  }
}// namespace Mist

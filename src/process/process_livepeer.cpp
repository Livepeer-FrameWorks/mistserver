#include "process_livepeer.h"

#include "../input/input.h"
#include "process.hpp"

#include <mist/downloader.h>
#include <mist/encode.h>
#include <mist/proc_stats.h>
#include <mist/procs.h>
#include <mist/shared_memory.h>
#include <mist/timing.h>
#include <mist/triggers.h>
#include <mist/util.h>

#include <atomic>
#include <mutex>
#include <ostream>
#include <sys/stat.h> //for stat
#include <sys/types.h> //for stat
#include <thread>
#include <unistd.h> //for stat

std::mutex segMutex;
std::mutex broadcasterMutex;
std::string cookie;

//Stat related stuff
JSON::Value pStat;
JSON::Value & pData = pStat["proc_status_update"]["status"];
std::mutex statsMutex;
uint64_t statSwitches = 0;
// Failure counters: written from uploadThread(s), read from main's periodic
// publisher for control decisions -> must be atomic.
std::atomic<uint64_t> statFailN200{0};
std::atomic<uint64_t> statFailTimeout{0};
std::atomic<uint64_t> statFailParse{0};
std::atomic<uint64_t> statFailOther{0};
uint64_t statSinkMs = 0;
uint64_t statSourceMs = 0;

// Pressure-publisher inputs. Updated from uploadThread(s); consumed by the
// periodic ProcState writer in main(). Atomics to avoid the statsMutex hot path.
std::atomic<uint64_t> statTotalExternalUs{0}; // sum of POST-to-response time (us)
std::atomic<uint64_t> statTotalLocalWorkUs{0}; // sum of local muxing/parse/insert time (us)
std::atomic<uint64_t> statTotalSourceWaitUs{0}; // sum of time uploadThreads waited for input
std::atomic<uint64_t> statTotalSinkWaitUs{0}; // sum of time uploadThreads waited their insert turn
std::atomic<uint32_t> statActiveUploads{0}; // # uploadThreads currently mid-POST
std::atomic<uint32_t> statLastSegSpeedQ16_16{0}; // last observed segDuration/turnaround (Q16.16)
// Slots currently occupied (filled by source, not yet released by upload).
// Read by the periodic publisher into ProcState.queueDepth without a lock;
// the previous approach of summing presegs[i].fullyWritten/fullyRead was a
// data race against the source and upload threads.
std::atomic<uint32_t> statQueueDepth{0};

std::string api_url;

Util::Config co;
Util::Config conf;

IPC::sharedPage procStatePage;
ProcExitState procExit;

size_t insertTurn = 0;
bool isStuck = false;
size_t sourceIndex = INVALID_TRACK_ID;

namespace Mist{

  void pickRandomBroadcaster(){
    std::string prevBroad = currBroadAddr;
    currBroadAddr.clear();
    std::set<std::string> validAddrs;
    jsonForEach(lpBroad, bCast){
      if (bCast->isMember("address")){
        validAddrs.insert((*bCast)["address"].asStringRef());
      }
    }
    if (validAddrs.size() > 1){validAddrs.erase(prevBroad);}
    if (!validAddrs.size()){
      FAIL_MSG("Could not select a new random broadcaster!");
      /// TODO Finish this function.
    }
    std::set<std::string>::iterator it = validAddrs.begin();
    for (size_t r = rand() % validAddrs.size(); r; --r){++it;}
    currBroadAddr = *it;
  }

  //Source process, takes data from input stream and sends to livepeer
  class ProcessSource : public TSOutput{
  public:
    bool isRecording(){return false;}
    bool isReadyForPlay() { return true; }
    ProcessSource(Socket::Connection & c, Util::Config & _cfg, JSON::Value & _capa) : TSOutput(c, _cfg, _capa) {
      meta.ignorePid(getpid());
      capa["name"] = "Livepeer";
      capa["codecs"][0u][0u].append("+H264");
      capa["codecs"][0u][0u].append("+HEVC");
      capa["codecs"][0u][0u].append("+MPEG2");
      capa["codecs"][0u][1u].append("+AAC");
      realTime = 0;
      wantRequest = false;
      parseData = true;
      currPreSeg = 0;
    }
    inline virtual bool keepGoing() { return config->is_active; }
    virtual bool onFinish(){
      if (opt.isMember("exit_unmask") && opt["exit_unmask"].asBool()){
        if (userSelect.size()){
          for (std::map<size_t, Comms::Users>::iterator it = userSelect.begin(); it != userSelect.end(); it++){
            INFO_MSG("Unmasking source track %zu" PRIu64, it->first);
            meta.validateTrack(it->first, TRACK_VALID_ALL);
          }
        }
      }
      return TSOutput::onFinish();
    }
    virtual void dropTrack(size_t trackId, const std::string &reason, bool probablyBad = true){
      if (opt.isMember("exit_unmask") && opt["exit_unmask"].asBool()){
        INFO_MSG("Unmasking source track %zu" PRIu64, trackId);
        meta.validateTrack(trackId, TRACK_VALID_ALL);
      }
      TSOutput::dropTrack(trackId, reason, probablyBad);
    }
    size_t currPreSeg;
    void sendTS(const char *tsData, size_t len = 188){
      if (!presegs[currPreSeg].data.size()){
        presegs[currPreSeg].time = thisPacket.getTime();
      }
      presegs[currPreSeg].data.append(tsData, len);
    };
    virtual void initialSeek(bool dryRun = false){
      if (!meta){return;}
      if (!dryRun){
        if (opt.isMember("source_mask")) {
          for (std::map<size_t, Comms::Users>::iterator ti = userSelect.begin(); ti != userSelect.end(); ++ti) {
            if (ti->first == INVALID_TRACK_ID) { continue; }
            INFO_MSG("Masking source track %zu with %zu", ti->first, (size_t)opt["source_mask"].asInt());
            meta.validateTrack(ti->first, meta.trackValid(ti->first) & opt["source_mask"].asInt());
          }
        }
        if (!meta.getLive() || opt["leastlive"].asBool()){
          INFO_MSG("Seeking to earliest point in stream");
          seek(0);
          return;
        }
      }
      Output::initialSeek(dryRun);
    }
    void sendNext(){
      {
        std::lock_guard<std::mutex> guard(statsMutex);
        if (pData["source_tracks"].size() != userSelect.size()){
          pData["source_tracks"].null();
          for (std::map<size_t, Comms::Users>::iterator it = userSelect.begin(); it != userSelect.end(); it++){
            pData["source_tracks"].append((uint64_t)it->first);
          }
        }
      }
      if (thisTime > statSourceMs){statSourceMs = thisTime;}

      // Split if we have a keyframe which contains new init data
      if (thisPacket.getFlag("keyframe") && M.trackLoaded(thisIdx) && M.getType(thisIdx) == "video") {
        // Always split if we current segment is long enough
        bool shouldSplit = (thisTime - presegs[currPreSeg].time) >= 1000;
        // Else check for changes to init data
        if (!shouldSplit) {
          char *dataPointer = 0;
          size_t dataLen = 0;
          thisPacket.getString("data", dataPointer, dataLen);
          std::string codec = M.getCodec(thisIdx);
          if (codec == "H264" && dataLen > 3) {
            uint8_t nalType = (dataPointer[4] & 0x1F);
            switch (nalType) {
              case 0x07: // sps
              case 0x08: // pps
                shouldSplit = true;
                HIGH_MSG("Switching to new segment since the current keyframe contains new init data");
              default: break;
            }
          } else if (codec == "HEVC" && dataLen > 3) {
            uint8_t nalType = (dataPointer[4] & 0x7E) >> 1;
            switch (nalType) {
              case 32: // vps
              case 33: // sps
              case 34: // pps
                shouldSplit = true;
                HIGH_MSG("Switching to new segment since the current keyframe contains new init data");
              default: break;
            }
          }
        }
        if (shouldSplit) {
          sourceIndex = getMainSelectedTrack();
          if (presegs[currPreSeg].data.size() > 187) {
            presegs[currPreSeg].keyNo = keyCount;
            presegs[currPreSeg].width = M.getWidth(thisIdx);
            presegs[currPreSeg].height = M.getHeight(thisIdx);
            presegs[currPreSeg].segDuration = thisTime - presegs[currPreSeg].time;
            presegs[currPreSeg].fullyRead = false;
            presegs[currPreSeg].fullyWritten = true;
            statQueueDepth.fetch_add(1, std::memory_order_relaxed);
            currPreSeg = (currPreSeg + 1) % PRESEG_COUNT;
          }
          while (!presegs[currPreSeg].fullyRead && conf.is_active) { Util::sleep(100); }
          presegs[currPreSeg].data.assign(0, 0);
          selectDefaultTracks();
          needsLookAhead = 0;
          maxSkipAhead = 0;
          packCounter = 0;
          ++keyCount;
          sendFirst = true;
        }
      }
      TSOutput::sendNext();
    }
  };

  //sink, takes data from livepeer and ingests
  class ProcessSink : public Input{
  public:
    ProcessSink(Util::Config *cfg) : Input(cfg){
      capa["name"] = "Livepeer";
      streamName = opt["sink"].asString();
      if (!streamName.size()){streamName = opt["source"].asString();}
      Util::streamVariables(streamName, opt["source"].asString());
      {
        std::lock_guard<std::mutex> guard(statsMutex);
        pStat["proc_status_update"]["sink"] = streamName;
        pStat["proc_status_update"]["source"] = opt["source"];
      }
      Util::setStreamName(opt["source"].asString() + "→" + streamName);
      if (opt.isMember("target_mask") && !opt["target_mask"].isNull() && opt["target_mask"].asString() != ""){
        DTSC::trackValidDefault = opt["target_mask"].asInt();
      } else {
        DTSC::trackValidDefault = TRACK_VALID_EXT_HUMAN | TRACK_VALID_EXT_PUSH;
      }
      preRun();
    };
    virtual bool needsLock(){return false;}
    bool isSingular(){return false;}
  private:
    std::map<std::string, readySegment>::iterator segIt;
    bool needHeader(){return false;}
    virtual void getNext(size_t idx = INVALID_TRACK_ID){
      thisPacket.null();
      int64_t timeOffset = 0;
      uint64_t trackId = 0;
      {
        std::lock_guard<std::mutex> guard(statsMutex);
        if (pData["sink_tracks"].size() != userSelect.size()){
          pData["sink_tracks"].null();
          for (std::map<size_t, Comms::Users>::iterator it = userSelect.begin(); it != userSelect.end(); it++){
            pData["sink_tracks"].append((uint64_t)it->first);
          }
        }
      }
      while (!thisPacket && conf.is_active){
        {
          std::lock_guard<std::mutex> guard(segMutex);
          std::string oRend;
          uint64_t lastPacket = 0xFFFFFFFFFFFFFFFFull;
          for (segIt = segs.begin(); segIt != segs.end(); ++segIt){
            if (isStuck){
              WARN_MSG("Considering %s: T%" PRIu64 ", fullyWritten: %s, fullyRead: %s", segIt->first.c_str(), segIt->second.lastPacket, segIt->second.fullyWritten?"Y":"N", segIt->second.fullyRead?"Y":"N");
            }
            if (!segIt->second.fullyWritten){continue;}
            if (segIt->second.lastPacket > lastPacket){continue;}
            oRend = segIt->first;
            lastPacket = segIt->second.lastPacket;
          }
          if (oRend.size()){
            if (isStuck){WARN_MSG("Picked %s!", oRend.c_str());}
            readySegment & S = segs[oRend];
            while (!S.S.hasPacket() && S.byteOffset <= S.data.size() - 188){
              S.S.parse(S.data + S.byteOffset, 0);
              S.byteOffset += 188;
              if (S.byteOffset > S.data.size() - 188){S.S.finish();}
            }
            if (S.S.hasPacket()){
              S.S.getEarliestPacket(thisPacket);
              if (!S.offsetCalcd){
                S.timeOffset = S.time - thisPacket.getTime();
                HIGH_MSG("First timestamp of %s at time %" PRIu64 " is %" PRIu64 ", adjusting by %" PRId64, oRend.c_str(), S.time, thisPacket.getTime(), S.timeOffset);
                S.offsetCalcd = true;
              }
              timeOffset = S.timeOffset;
              if (thisPacket){
                S.lastPacket = thisPacket.getTime() + timeOffset;
                if (S.lastPacket >= statSinkMs){statSinkMs = S.lastPacket;}
              }
              trackId = (S.ID << 16) + thisPacket.getTrackId();
              thisIdx = M.trackIDToIndex(trackId, getpid());
              if (thisIdx == INVALID_TRACK_ID || !M.getCodec(thisIdx).size()){
                INFO_MSG("Initializing track %zi as %" PRIu64 " for playlist %zu", thisPacket.getTrackId(), trackId, S.ID);
                S.S.initializeMetadata(meta, thisPacket.getTrackId(), trackId);
                thisIdx = M.trackIDToIndex(trackId, getpid());
                meta.setSourceTrack(thisIdx, sourceIndex);
                if (M.getType(thisIdx) == "audio") { meta.validateTrack(thisIdx, 0); }
              }
            }
            if (S.byteOffset >= S.data.size() && !S.S.hasPacket()){
              S.fullyWritten = false;
              S.fullyRead = true;
            }
          }
        }
        if (!thisPacket){
          Util::sleep(25);
          if (userSelect.size() && userSelect.begin()->second.getStatus() == COMM_STATUS_REQDISCONNECT){
            procExit.log(ER_CLEAN_LIVE_BUFFER_REQ, 0, "buffer requested shutdown");
            return;
          }
        }
      }

      if (thisPacket){
        char *data = thisPacket.getData();
        //overwrite trackID
        Bit::htobl(data + 8, trackId);
        //overwrite packettime
        thisTime = thisPacket.getTime() + timeOffset;
        Bit::htobll(data + 12, thisTime);
      }
    }
    bool checkArguments(){return true;}
    bool readHeader(){return true;}
    bool openStreamSource(){return true;}
    void parseStreamHeader(){}
    virtual bool publishesTracks(){return false;}
  };



}// namespace Mist



void sinkThread(){
  Mist::ProcessSink in(&co);
  co.activate();
  co.is_active = true;
  INFO_MSG("Running sink thread...");
  int rc = in.run();
  if (rc == 0) {
    procExit.log(ER_CLEAN_EOF, 0, "Sink thread finished");
  } else {
    procExit.log(Util::mRExitReason ? Util::mRExitReason : ER_UNKNOWN, rc, "%s",
                 Util::exitReason[0] ? Util::exitReason : "Sink thread failed");
  }
  INFO_MSG("Sink thread shutting down");
  conf.is_active = false;
  co.is_active = false;
}

void sourceThread(){
  conf.addOption("streamname", JSON::fromString("{\"arg\":\"string\",\"short\":\"s\",\"long\":"
                                                  "\"stream\",\"help\":\"The name of the stream "
                                                  "that this connector will transmit.\"}"));
  JSON::Value opt;
  opt["arg"] = "string";
  opt["default"] = "";
  opt["arg_num"] = 1;
  opt["help"] = "Target filename to store EBML file as, or - for stdout.";
  //Check for audio selection, default to none
  std::string audio_select = "none";
  if (Mist::opt.isMember("audio_select") && Mist::opt["audio_select"].isString() && Mist::opt["audio_select"]){
    audio_select = Mist::opt["audio_select"].asStringRef();
  }
  //Check for source track selection, default to maxbps
  std::string video_select = "maxbps";
  if (Mist::opt.isMember("source_track") && Mist::opt["source_track"].isString() && Mist::opt["source_track"]){
    video_select = Mist::opt["source_track"].asStringRef();
  }
  conf.addOption("target", opt);
  conf.getOption("streamname", true).append(Mist::opt["source"].c_str());
  conf.getOption("target", true).append("-?audio="+audio_select+"&video="+video_select);
  JSON::Value capa;
  Mist::ProcessSource::init(&conf, capa);
  conf.is_active = true;
  Socket::Connection c;
  Mist::ProcessSource out(c, conf, capa);
  if (conf.is_active){
    INFO_MSG("Running source thread...");
    int rc = out.run();
    if (rc == 0) {
      procExit.log(ER_CLEAN_EOF, 0, "Source thread finished");
    } else {
      procExit.log(Util::mRExitReason ? Util::mRExitReason : ER_UNKNOWN, rc, "%s",
                   Util::exitReason[0] ? Util::exitReason : "Source thread failed");
    }
    INFO_MSG("Stopping source thread");
  }else{
    procExit.log(ER_READ_START_FAILURE, 2, "Source thread failed to initialize");
  }
  conf.is_active = false;
  co.is_active = false;
}

/// Structure to hold per-rendition information
struct RenditionInfo {
  std::string name;
  size_t bytes;
};

/// Result from parsing multipart response
struct MultipartResult {
  size_t renditionCount;
  size_t totalOutputBytes;
  std::vector<RenditionInfo> renditions;
};

///Inserts a part into the queue of parts to parse
void insertPart(const Mist::preparedSegment & mySeg, const std::string & rendition, void * ptr, size_t len){
  uint64_t waitTime = Util::bootMS();
  uint64_t lastAlert = waitTime;
  while (conf.is_active){
    {
      std::lock_guard<std::mutex> guard(segMutex);
      if (Mist::segs[rendition].fullyRead){
        HIGH_MSG("Inserting %zi bytes of %s, originally for time %" PRIu64, len, rendition.c_str(), mySeg.time);
        Mist::segs[rendition].set(mySeg.time, ptr, len);
        return;
      }
    }
    uint64_t currMs = Util::bootMS();
    isStuck = false;
    if (currMs-waitTime > 5000 && currMs-lastAlert > 1000){
      lastAlert = currMs;
      INFO_MSG("Waiting for %s to finish parsing current part (%" PRIu64 "ms)...", rendition.c_str(), currMs-waitTime);
      isStuck = true;
    }
    Util::sleep(100);
  }
}

///Parses a multipart response, returns rendition details
MultipartResult parseMultipart(const Mist::preparedSegment & mySeg, const std::string & cType, const std::string & d){
  MultipartResult result;
  result.renditionCount = 0;
  result.totalOutputBytes = 0;
  std::string bound;
  if (cType.find("boundary=") != std::string::npos){
    bound = "--"+cType.substr(cType.find("boundary=")+9);
  }
  if (!bound.size()){
    FAIL_MSG("Could not parse boundary string from Content-Type header!");
    return result;
  }
  size_t startPos = 0;
  size_t nextPos = d.find(bound, startPos);
  //While there is at least one boundary to be found
  while (nextPos != std::string::npos){
    startPos = nextPos+bound.size()+2;
    nextPos = d.find(bound, startPos);
    if (nextPos != std::string::npos){
      //We have a start and end position, looking good so far...
      size_t headEnd = d.find("\r\n\r\n", startPos);
      if (headEnd == std::string::npos || headEnd > nextPos){
        FAIL_MSG("Could not find end of headers for multi-part part; skipping to next part");
        continue;
      }
      //Alright, we know where our headers and data are. Parse the headers
      std::map<std::string, std::string> partHeaders;
      size_t headPtr = startPos;
      size_t nextNL = d.find("\r\n", headPtr);
      while (nextNL != std::string::npos && nextNL <= headEnd){
        size_t col = d.find(":", headPtr);
        if (col != std::string::npos && col < nextNL){
          partHeaders[d.substr(headPtr, col-headPtr)] = d.substr(col+2, nextNL-col-2);
        }
        headPtr = nextNL+2;
        nextNL = d.find("\r\n", headPtr);
      }
      for (std::map<std::string, std::string>::iterator it = partHeaders.begin(); it != partHeaders.end(); ++it){
        VERYHIGH_MSG("Header %s = %s", it->first.c_str(), it->second.c_str());
      }
      size_t bodyLen = nextPos - headEnd - 6;
      VERYHIGH_MSG("Body has length %zi", bodyLen);
      std::string preType = partHeaders["Content-Type"].substr(0, 10);
      Util::stringToLower(preType);
      if (preType == "video/mp2t"){
        insertPart(mySeg, partHeaders["Rendition-Name"], (void*)(d.data()+headEnd+4), bodyLen);
        ++result.renditionCount;
        result.totalOutputBytes += bodyLen;
        RenditionInfo rInfo;
        rInfo.name = partHeaders["Rendition-Name"];
        rInfo.bytes = bodyLen;
        result.renditions.push_back(rInfo);
      }
    }
  }
  return result;
}

void segmentRejectedTrigger(Mist::preparedSegment & mySeg, const std::string & bc1, const std::string & bc2){
  if (Triggers::shouldTrigger("LIVEPEER_SEGMENT_REJECTED", Util::streamName)){
    FAIL_MSG("Segment could not be transcoded, skipping to next and submitting for analysis");
    JSON::Value trackInfo;
    trackInfo["width"] = mySeg.width;
    trackInfo["height"] = mySeg.height;
    trackInfo["duration"] = mySeg.segDuration;
    std::string payload = Mist::opt.toString()+"\n"+Encodings::Base64::encode(std::string(mySeg.data, mySeg.data.size()))+"\n"+trackInfo.toString()+"\n"+bc1+"\n"+bc2;
    Triggers::doTrigger("LIVEPEER_SEGMENT_REJECTED", payload, Util::streamName);
  }else{
    FAIL_MSG("Segment could not be transcoded, skipping to next");
  }
  mySeg.fullyWritten = false;
  mySeg.fullyRead = true;
  statQueueDepth.fetch_sub(1, std::memory_order_relaxed);
  insertTurn = (insertTurn + 1) % PRESEG_COUNT;
}

bool fatalUploadStatus(uint32_t status) {
  return status == 401 || status == 403 || status == 503;
}

void uploadThread(size_t myNum){
  Mist::preparedSegment & mySeg = Mist::presegs[myNum];
  HTTP::Downloader upper;
  bool was422 = false;
  std::string prevURL;
  while (conf.is_active){
    // Block until source thread has prepared this slot. Time spent here counts
    // as "source wait"; we are starved for input.
    uint64_t srcWaitStart = Util::getMicros();
    while (conf.is_active && !mySeg.fullyWritten){Util::sleep(100);}
    statTotalSourceWaitUs.fetch_add(Util::getMicros(srcWaitStart), std::memory_order_relaxed);
    if (!conf.is_active){return;}//Exit early on shutdown
    size_t attempts = 0;
    do{
      HTTP::URL target;
      {
        std::lock_guard<std::mutex> guard(broadcasterMutex);
        target = HTTP::URL(Mist::currBroadAddr+"/live/"+Mist::lpID+"/"+JSON::Value(mySeg.keyNo).asString()+".ts");
        upper.setHeader("Cookie", cookie);
      }
      upper.dataTimeout = mySeg.segDuration/1000 + 2;
      upper.retryCount = 2;
      upper.setHeader("Accept", "multipart/mixed");
      upper.setHeader("Content-Duration", JSON::Value(mySeg.segDuration).asString());
      upper.setHeader("Content-Resolution", JSON::Value(mySeg.width).asString()+"x"+JSON::Value(mySeg.height).asString());

      // If the Livepeer API Key hasn't been set then we send the configuration as an HTTP header rather than pushing to the API
      if (!Mist::opt.isMember("access_token") || !Mist::opt["access_token"] || !Mist::opt["access_token"].isString()) {
        JSON::Value tc;
        tc["profiles"] = Mist::opt["target_profiles"];
        upper.setHeader("Livepeer-Transcode-Configuration", tc.toString());
      }

      uint64_t uplTime = Util::getMicros();
      statActiveUploads.fetch_add(1, std::memory_order_relaxed);
      bool postOk = upper.post(target, mySeg.data, mySeg.data.size());
      statActiveUploads.fetch_sub(1, std::memory_order_relaxed);
      if (postOk) {
        uplTime = Util::getMicros(uplTime);
        statTotalExternalUs.fetch_add(uplTime, std::memory_order_relaxed);
        if (upper.getStatusCode() == 200){
          MEDIUM_MSG("Uploaded %zu bytes (time %" PRIu64 "-%" PRIu64 " = %" PRIu64 " ms) to %s in %.2f ms", mySeg.data.size(), mySeg.time, mySeg.time+mySeg.segDuration, mySeg.segDuration, target.getUrl().c_str(), uplTime/1000.0);
          was422 = false;
          prevURL.clear();
          mySeg.fullyWritten = false;
          {
            std::lock_guard<std::mutex> guard(broadcasterMutex);
            std::string newCookie = upper.getCookie();
            if (newCookie.size() && newCookie != cookie) { cookie = newCookie; }
          }
          // Sink wait: blocked waiting for our insertion turn (serialized
          // across uploadThreads).
          uint64_t sinkWaitStart = Util::getMicros();
          while (myNum != insertTurn && conf.is_active){Util::sleep(100);}
          statTotalSinkWaitUs.fetch_add(Util::getMicros(sinkWaitStart), std::memory_order_relaxed);
          if (!conf.is_active){return;}//Exit early on shutdown
          // Local work: multipart parse + per-rendition insert. Whatever
          // happens here is CPU we're spending on this proc.
          uint64_t workStart = Util::getMicros();
          MultipartResult mpResult;
          mpResult.renditionCount = 0;
          mpResult.totalOutputBytes = 0;
          if (upper.getHeader("Content-Type").substr(0, 10) == "multipart/"){
            mpResult = parseMultipart(mySeg, upper.getHeader("Content-Type"), upper.const_data());
          }else{
            ++statFailParse;
            FAIL_MSG("Non-multipart response (%s, %zu bytes) received - this version only works "
                     "with multipart!",
                     upper.getHeader("Content-Type").c_str(), upper.const_data().size());
          }
          mySeg.fullyRead = true;
          statQueueDepth.fetch_sub(1, std::memory_order_relaxed);
          insertTurn = (insertTurn + 1) % PRESEG_COUNT;
          statTotalLocalWorkUs.fetch_add(Util::getMicros(workStart), std::memory_order_relaxed);

          // Always publish observed speed to ProcState; the controller needs
          // this independent of whether LIVEPEER_SEGMENT_COMPLETE is wired up.
          // Only the trigger payload emission lives behind shouldTrigger().
          uint64_t turnaroundMs = uplTime / 1000;
          double speedFactor = turnaroundMs > 0 ? (double)mySeg.segDuration / (double)turnaroundMs : 0.0;
          {
            double sf = speedFactor;
            if (sf < 0) sf = 0;
            if (sf > 65535.0) sf = 65535.0;
            statLastSegSpeedQ16_16.store((uint32_t)(sf * 65536.0), std::memory_order_relaxed);
          }

          if (Triggers::shouldTrigger("LIVEPEER_SEGMENT_COMPLETE", Util::streamName)){
            // Build renditions JSON array (only this field is JSON since it's variable-length)
            JSON::Value renditionsJson;
            for (size_t i = 0; i < mpResult.renditions.size(); ++i){
              JSON::Value rend;
              rend["name"] = mpResult.renditions[i].name;
              rend["bytes"] = (uint64_t)mpResult.renditions[i].bytes;
              renditionsJson.append(rend);
            }

            std::string payload = std::string(Util::streamName) + "\n" +      // 1. stream name
              Mist::lpID + "\n" +                                              // 2. livepeer session ID
              JSON::Value(mySeg.keyNo).asString() + "\n" +                     // 3. segment number
              JSON::Value(mySeg.time).asString() + "\n" +                      // 4. segment start ms
              JSON::Value(mySeg.segDuration).asString() + "\n" +               // 5. segment duration ms
              JSON::Value(mySeg.width).asString() + "\n" +                     // 6. source width
              JSON::Value(mySeg.height).asString() + "\n" +                    // 7. source height
              JSON::Value((uint64_t)mySeg.data.size()).asString() + "\n" +     // 8. input bytes
              JSON::Value((uint64_t)mpResult.totalOutputBytes).asString() + "\n" + // 9. output bytes total
              JSON::Value(mpResult.renditionCount).asString() + "\n" +         // 10. rendition count
              JSON::Value((uint64_t)attempts).asString() + "\n" +              // 11. attempt count
              target.getUrl() + "\n" +                                         // 12. broadcaster URL
              JSON::Value(turnaroundMs).asString() + "\n" +                    // 13. turnaround ms
              JSON::Value(speedFactor).asString() + "\n" +                     // 14. speed factor
              renditionsJson.toString();                                       // 15. renditions (JSON array)
            Triggers::doTrigger("LIVEPEER_SEGMENT_COMPLETE", payload, Util::streamName);
          }
          break;//Success: no need to retry
        }else if (upper.getStatusCode() == 422){
          //segment rejected by broadcaster node; try a different broadcaster at most once and keep track
          ++statFailN200;
          WARN_MSG("Rejected upload of %zu bytes to %s after %.2f ms: %" PRIu32 " %s", mySeg.data.size(), target.getUrl().c_str(), uplTime/1000.0, upper.getStatusCode(), upper.getStatusText().c_str());
          if (was422){
            //second error in a row, fire off LIVEPEER_SEGMENT_REJECTED trigger
            segmentRejectedTrigger(mySeg, prevURL, target.getUrl());
            was422 = false;
            prevURL.clear();
            break;
          }else{
            prevURL = target.getUrl();
            was422 = true;
          }
        }else{
          //Failure due to non-200/422 status code
          ++statFailN200;
          WARN_MSG("Failed to upload %zu bytes to %s in %.2f ms: %" PRIu32 " %s", mySeg.data.size(), target.getUrl().c_str(), uplTime/1000.0, upper.getStatusCode(), upper.getStatusText().c_str());
          if (fatalUploadStatus(upper.getStatusCode())) {
            procExit.log(ER_FORMAT_SPECIFIC, 2, "Livepeer upload fatal HTTP status %" PRIu32 " %s",
                         upper.getStatusCode(), upper.getStatusText().c_str());
            conf.is_active = false;
            return;
          }
        }
      } else {
        //other failures and aborted uploads
        if (!conf.is_active){return;}//Exit early on shutdown
        uplTime = Util::getMicros(uplTime);
        statTotalExternalUs.fetch_add(uplTime, std::memory_order_relaxed);
        ++statFailTimeout;
        WARN_MSG("Failed to upload %zu bytes to %s in %.2f ms", mySeg.data.size(), target.getUrl().c_str(), uplTime/1000.0);
      }
      //Error handling
      attempts++;
      Util::sleep(100);//Rate-limit retries
      if (attempts > 4){
        procExit.log(ER_FORMAT_SPECIFIC, 2, "too many upload failures");
        conf.is_active = false;
        return;
      }
      bool switchSuccess = false;
      {
        std::lock_guard<std::mutex> guard(broadcasterMutex);
        cookie.clear();
        std::string prevBroadAddr = Mist::currBroadAddr;
        Mist::pickRandomBroadcaster();
        if (!Mist::currBroadAddr.size()){
          FAIL_MSG("Cannot switch to new broadcaster: none available");
          procExit.log(ER_FORMAT_SPECIFIC, 2, "no Livepeer broadcasters available");
          conf.is_active = false;
          return;
        }
        if (Mist::currBroadAddr != prevBroadAddr){
          ++statSwitches;
          switchSuccess = true;
          WARN_MSG("Switched to new broadcaster: %s", Mist::currBroadAddr.c_str());
        }else{
          WARN_MSG("Cannot switch broadcaster; only a single option is available");
        }
      }
      if (!switchSuccess && was422){
        //no switch possible, fire off LIVEPEER_SEGMENT_REJECTED trigger
        segmentRejectedTrigger(mySeg, prevURL, "N/A");
        was422 = false;
        prevURL.clear();
        break;
      }
    }while(conf.is_active);
  }
}

int main(int argc, char *argv[]){
  DTSC::trackValidMask = TRACK_VALID_INT_PROCESS;
  Util::Config config(argv[0]);
  Util::Config::binaryType = Util::PROCESS;

  // Initialize SHM early so exit reasons are available even for config errors
  {
    char shmName[NAME_BUFFER_SIZE];
    snprintf(shmName, NAME_BUFFER_SIZE, SHM_PROC_STATE, getpid());
    procStatePage.init(shmName, sizeof(ProcState), true, false);
    ProcState::initPage(procStatePage);
  }

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
    opt["value"].append(false);
    config.addOption("json", opt);
    opt.null();
    opt["long"] = "kickoff";
    opt["short"] = "K";
    opt["help"] = "Kick off source if not already active";
    opt["value"].append(false);
    config.addOption("kickoff", opt);
  }

  capa["codecs"][0u][0u].append("H264");

  if (!(config.parseArgs(argc, argv))) {
    procExit.log(ER_FORMAT_SPECIFIC, 2, "Failed to parse command-line arguments");
    return procExit.flush(procStatePage);
  }
  if (config.getBool("json")){

    capa["name"] = "Livepeer";
    capa["hrn"] = "Encoder: Livepeer network encoding";
    capa["desc"] = "Use livepeer to transcode video.";
    addGenericProcessOptions(capa);
    {
      JSON::Value & genopts = capa["optional"]["general_process_options"]["options"];

      genopts["source_track"]["name"] = "Video input selection";
      genopts["source_track"]["help"] =
        "Track selector(s) of the video portion of the source stream. Defaults to highest bit rate video track.";
      genopts["source_track"]["type"] = "track_selector_parameter";
      genopts["source_track"]["default"] = "maxbps";
      genopts["source_track"]["sort"] = "a";

      genopts["audio_select"]["name"] = "Audio streams";
      genopts["audio_select"]["help"] = "Track selector(s) for the audio portion of the source stream. Defaults to "
                                        "'none' so no audio is passed at all.";
      genopts["audio_select"]["type"] = "track_selector_parameter";
      genopts["audio_select"]["default"] = "none";

      genopts["sink"]["name"] = "Target stream";
      genopts["sink"]["help"] =
        "What stream the encoded track should be added to. Defaults to source stream. May contain variables.";
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
      genopts["source_mask"]["select"][2u][0u] = 6;
      genopts["source_mask"]["select"][2u][1u] = "Processing and pushing tasks (not viewers)";
      genopts["source_mask"]["select"][3u][0u] = 5;
      genopts["source_mask"]["select"][3u][1u] = "Processing and viewer tasks (not pushes)";
      genopts["source_mask"]["default"] = "Keep original value";
      genopts["source_mask"]["sort"] = "c";

      genopts["target_mask"]["name"] = "Output track mask";
      genopts["target_mask"]["help"] = "What internal processes should have access to the output track(s)";
      genopts["target_mask"]["type"] = "select";
      genopts["target_mask"]["select"][0u][0u] = 255;
      genopts["target_mask"]["select"][0u][1u] = "Everything";
      genopts["target_mask"]["select"][1u][0u] = 1;
      genopts["target_mask"]["select"][1u][1u] = "Viewer tasks (not processing, not pushes)";
      genopts["target_mask"]["select"][2u][0u] = 2;
      genopts["target_mask"]["select"][2u][1u] = "Pushing tasks (not processing, not viewers)";
      genopts["target_mask"]["select"][3u][0u] = 4;
      genopts["target_mask"]["select"][3u][1u] = "Processing tasks (not pushes, not viewers)";
      genopts["target_mask"]["select"][4u][0u] = 3;
      genopts["target_mask"]["select"][4u][1u] = "Viewer and pushing tasks (not processing)";
      genopts["target_mask"]["select"][5u][0u] = 5;
      genopts["target_mask"]["select"][5u][1u] = "Viewer and processing tasks (not pushes)";
      genopts["target_mask"]["select"][6u][0u] = 6;
      genopts["target_mask"]["select"][6u][1u] = "Pushing and processing tasks (not viewers)";
      genopts["target_mask"]["select"][7u][0u] = 0;
      genopts["target_mask"]["select"][7u][1u] = "Nothing";
      genopts["target_mask"]["default"] = "Keep original value";
      genopts["target_mask"]["sort"] = "d";

      genopts["exit_unmask"]["name"] = "Undo masks on process exit/fail";
      genopts["exit_unmask"]["help"] = "If/when the process exits or fails, the masks for input tracks will be reset "
                                       "to defaults. (NOT to previous value, but to defaults!)";
      genopts["exit_unmask"]["default"] = false;
      genopts["exit_unmask"]["sort"] = "e";
    }

    capa["required"]["access_token"]["name"] = "Access token";
    capa["required"]["access_token"]["help"] = "Your livepeer access token";
    capa["required"]["access_token"]["type"] = "string";

    capa["required"]["target_profiles"]["name"] = "Profiles";
    capa["required"]["target_profiles"]["type"] = "sublist";
    capa["required"]["target_profiles"]["itemLabel"] = "profile";
    capa["required"]["target_profiles"]["help"] = "Tracks to transcode the source into";
    capa["required"]["target_profiles"]["sort"] = "f";
    {
      JSON::Value &grp = capa["required"]["target_profiles"]["required"];
      grp["name"]["name"] = "Name";
      grp["name"]["help"] = "Name for the profile. Must be unique within this transcode.";
      grp["name"]["type"] = "str";
      grp["name"]["sort"] = 0;
      grp["bitrate"]["name"] = "Bitrate";
      grp["bitrate"]["help"] = "Target bit rate of the output";
      grp["bitrate"]["type"] = "int";
      grp["bitrate"]["sort"] = 1;
      grp["bitrate"]["unit"][0u][0u] = "1";
      grp["bitrate"]["unit"][0u][1u] = "bit/s";
      grp["bitrate"]["unit"][1u][0u] = "1000";
      grp["bitrate"]["unit"][1u][1u] = "kbit/s";
      grp["bitrate"]["unit"][2u][0u] = "1000000";
      grp["bitrate"]["unit"][2u][1u] = "Mbit/s";
    }{
      JSON::Value &grp = capa["required"]["target_profiles"]["sublist"];
      grp["width"]["name"] = "Width";
      grp["width"]["help"] = "Width in pixels of the output. Defaults to match aspect with height, or source width if both are default.";
      grp["width"]["unit"] = "px";
      grp["width"]["type"] = "int";
      grp["width"]["sort"] = 2;
      grp["height"]["name"] = "Height";
      grp["height"]["help"] = "Height in pixels of the output. Defaults to match aspect with width, or source height if both are default. If only height is given and the source height is greater than the source width, width and height will swap and do what you most likely wanted to do (e.g. follow your config in portrait mode instead of landscape mode).";
      grp["height"]["unit"] = "px";
      grp["height"]["type"] = "int";
      grp["height"]["sort"] = 3;
      grp["fps"]["name"] = "Framerate";
      grp["fps"]["help"] = "Framerate of the output. Zero means to match the input (= the default).";
      grp["fps"]["unit"] = "frames per second";
      grp["fps"]["default"] = 0;
      grp["fps"]["type"] = "int";
      grp["fps"]["sort"] = 4;
      grp["gop"]["name"] = "Keyframe interval / GOP size";
      grp["gop"]["help"] = "Interval of keyframes / duration of GOPs for the transcode. \"0.0\" means to match input (= the default), 'intra' means to send only key frames. Otherwise, fractional seconds between keyframes.";
      grp["gop"]["unit"] = "seconds";
      grp["gop"]["default"] = "0.0";
      grp["gop"]["type"] = "str";
      grp["gop"]["sort"] = 5;

      grp["profile"]["name"] = "H264 Profile";
      grp["profile"]["help"] = "Profile to use. Defaults to \"High\".";
      grp["profile"]["type"] = "select";
      grp["profile"]["select"][0u][0u] = "H264High";
      grp["profile"]["select"][0u][1u] = "High";
      grp["profile"]["select"][1u][0u] = "H264Baseline";
      grp["profile"]["select"][1u][1u] = "Baseline";
      grp["profile"]["select"][2u][0u] = "H264Main";
      grp["profile"]["select"][2u][1u] = "Main";
      grp["profile"]["select"][3u][0u] = "H264ConstrainedHigh";
      grp["profile"]["select"][3u][1u] = "High, without b-frames";
      grp["profile"]["default"] = "H264High";

      grp["track_inhibit"]["name"] = "Track inhibitor(s)";
      grp["track_inhibit"]["help"] =
          "What tracks to use as inhibitors. If this track selector is able to select a track, the profile is not used. Only verified on initial boot of the process and then never again. Defaults to none.";
      grp["track_inhibit"]["type"] = "string";
      grp["track_inhibit"]["validate"][0u] = "track_selector";
      grp["track_inhibit"]["default"] = "audio=none&video=none&subtitle=none";
    }

    capa["optional"]["leastlive"]["name"] = "Start in the past";
    capa["optional"]["leastlive"]["help"] =
      "Start the transcode as far back in the past as possible, instead of at the most-live point of the stream.";
    capa["optional"]["leastlive"]["type"] = "bool";
    capa["optional"]["leastlive"]["default"] = false;

    capa["optional"]["min_viewers"]["name"] = "Minimum viewers";
    capa["optional"]["min_viewers"]["help"] =
      "Transcode will only be active while this many viewers are watching the stream.";
    capa["optional"]["min_viewers"]["type"] = "int";
    capa["optional"]["min_viewers"]["default"] = 0;

    capa["optional"]["custom_url"]["name"] = "Custom API URL";
    capa["optional"]["custom_url"]["help"] = "Alternative API URL path";
    capa["optional"]["custom_url"]["type"] = "string";
    capa["optional"]["custom_url"]["default"] = "https://livepeer.live/api";

    capa["optional"]["hardcoded_broadcasters"]["name"] = "Hardcoded broadcasters";
    capa["optional"]["hardcoded_broadcasters"]["help"] =
      "Use hardcoded broadcasters, rather than using Livepeer's gateway.";
    capa["optional"]["hardcoded_broadcasters"]["type"] = "string"; // TODO change to "inputlist" when this process supports it

    capa["ainfo"]["lp_id"]["name"] = "Livepeer transcode ID";
    capa["ainfo"]["switches"]["name"] = "Broadcaster switches since start";
    capa["ainfo"]["fail_non200"]["name"] = "Failures due to non-200 response codes";
    capa["ainfo"]["fail_timeout"]["name"] = "Failures due to timeout";
    capa["ainfo"]["fail_parse"]["name"] = "Failures due to parse errors in TS response data";
    capa["ainfo"]["fail_other"]["name"] = "Failures due to other reasons";
    capa["ainfo"]["bc"]["name"] = "Currently used broadcaster";
    capa["ainfo"]["sinkTime"]["name"] = "Sink timestamp";
    capa["ainfo"]["sourceTime"]["name"] = "Source timestamp";
    capa["ainfo"]["percent_done"]["name"] = "Percentage for VoD transcodes";

    std::cout << capa.toString() << std::endl;
    return -1;
  }

  Util::redirectLogsIfNeeded();

  // read configuration
  if (config.getString("configuration") != "-"){
    Mist::opt = JSON::fromString(config.getString("configuration"));
  }else{
    std::string json, line;
    INFO_MSG("Reading configuration from standard input");
    while (std::getline(std::cin, line)){json.append(line);}
    Mist::opt = JSON::fromString(json.c_str());
  }

  // check config for generic options
  srand(getpid());
  // Check generic configuration variables
  if (!Mist::opt.isMember("source") || !Mist::opt["source"] || !Mist::opt["source"].isString()){
    procExit.log(ER_FORMAT_SPECIFIC, 2, "Missing or blank source in config");
    return procExit.flush(procStatePage);
  }

  if (!Mist::opt.isMember("sink") || !Mist::opt["sink"] || !Mist::opt["sink"].isString()){
    INFO_MSG("No sink explicitly set, using source as sink");
  }
  if (!Mist::opt.isMember("custom_url") || !Mist::opt["custom_url"] || !Mist::opt["custom_url"].isString()){
    api_url = "https://livepeer.live/api";
  }else{
    api_url = Mist::opt["custom_url"].asStringRef();
  }


  {
    //Ensure stream name is set in all threads
    std::string streamName = Mist::opt["sink"].asString();
    if (!streamName.size()){streamName = Mist::opt["source"].asString();}
    Util::streamVariables(streamName, Mist::opt["source"].asString());
    Util::setStreamName(Mist::opt["source"].asString() + "→" + streamName);
  }


  const std::string & srcStrm = Mist::opt["source"].asStringRef();
  if (config.getBool("kickoff")){
    if (!Util::startInput(srcStrm, "")){
      procExit.log(ER_READ_START_FAILURE, 1, "Could not start source stream");
      return procExit.flush(procStatePage);
    }
    uint8_t streamStat = Util::getStreamStatus(srcStrm);
    size_t sleeps = 0;
    while (++sleeps < 2400 && streamStat != STRMSTAT_OFF && streamStat != STRMSTAT_READY){
      if (sleeps >= 16 && (sleeps % 4) == 0){
        INFO_MSG("Waiting for stream to boot... (" PRETTY_PRINT_TIME " / " PRETTY_PRINT_TIME ")", PRETTY_ARG_TIME(sleeps/4), PRETTY_ARG_TIME(2400/4));
      }
      Util::sleep(250);
      streamStat = Util::getStreamStatus(srcStrm);
    }
    if (streamStat != STRMSTAT_READY){
      procExit.log(ER_READ_START_FAILURE, 1, "Source stream not available after kickoff");
      return procExit.flush(procStatePage);
    }
  }

  //connect to source metadata
  DTSC::Meta M(srcStrm, false);

  //find source video track
  std::map<std::string, std::string> targetParams;
  targetParams["video"] = "maxbps";
  JSON::Value sourceCapa;
  sourceCapa["name"] = "Livepeer";
  sourceCapa["codecs"][0u][0u].append("+H264");
  sourceCapa["codecs"][0u][0u].append("+HEVC");
  sourceCapa["codecs"][0u][0u].append("+MPEG2");
  if (Mist::opt.isMember("source_track") && Mist::opt["source_track"].isString() && Mist::opt["source_track"]){
    targetParams["video"] = Mist::opt["source_track"].asStringRef();
  }
  size_t sourceIdx = INVALID_TRACK_ID;
  size_t sleeps = 0;
  while (++sleeps < 60 && (sourceIdx == INVALID_TRACK_ID || !M.getWidth(sourceIdx) || !M.getHeight(sourceIdx))){
    M.reloadReplacedPagesIfNeeded();
    std::set<size_t> vidTrack = Util::wouldSelect(M, targetParams, sourceCapa);
    sourceIdx = vidTrack.size() ? (*(vidTrack.begin())) : INVALID_TRACK_ID;
    if (sourceIdx == INVALID_TRACK_ID || !M.getWidth(sourceIdx) || !M.getHeight(sourceIdx)){
      Util::sleep(250);
    }
  }
  if (sourceIdx == INVALID_TRACK_ID || !M.getWidth(sourceIdx) || !M.getHeight(sourceIdx)){
    procExit.log(ER_FORMAT_SPECIFIC, 2, "No valid source video track found");
    return procExit.flush(procStatePage);
  }

  //build transcode request
  JSON::Value pl;
  pl["name"] = Mist::opt["source"];
  pl["profiles"] = Mist::opt["target_profiles"];
  jsonForEach(pl["profiles"], prof){
    if (!prof->isMember("gop")){(*prof)["gop"] = "0.0";}
    //no or automatic framerate? default to source rate, if set, or 25 otherwise
    if (!prof->isMember("fps") || (*prof)["fps"].asDouble() == 0.0){
      (*prof)["fps"] = M.getFpks(sourceIdx);
      if (!(*prof)["fps"].asInt()){(*prof)["fps"] = 25000;}
      (*prof)["fpsDen"] = 1000;
    }
    if (!prof->isMember("profile")){(*prof)["profile"] = "H264High";}
    if ((!prof->isMember("height") || !(*prof)["height"].asInt()) && (!prof->isMember("width") || !(*prof)["width"].asInt())){
      //no width and no height
      (*prof)["width"] = M.getWidth(sourceIdx);
      (*prof)["height"] = M.getHeight(sourceIdx);
    }
    if (!prof->isMember("width") || !(*prof)["width"].asInt()){
      //no width, but we have height
      //first, check if our source is in portrait mode, if so, we assume they meant width instead of height
      if (M.getWidth(sourceIdx) < M.getHeight(sourceIdx)){
        //portrait mode
        uint32_t heightSetting = (*prof)["height"].asInt();
        (*prof)["width"] = heightSetting;
        (*prof)["height"] = M.getHeight(sourceIdx) * heightSetting / M.getWidth(sourceIdx);
      }else{
        //landscape mode
        uint32_t heightSetting = (*prof)["height"].asInt();
        (*prof)["width"] = M.getWidth(sourceIdx) * heightSetting / M.getHeight(sourceIdx);
      }
    }
    if (!prof->isMember("height") || !(*prof)["height"].asInt()){
      //no height, but we have width
      //No portrait/landscape check, as per documentation
      uint32_t widthSetting = (*prof)["width"].asInt();
      (*prof)["height"] = M.getHeight(sourceIdx) * widthSetting / M.getWidth(sourceIdx);
    }
    //force width/height to multiples of 16
    (*prof)["width"] = ((*prof)["width"].asInt() / 16) * 16;
    (*prof)["height"] = ((*prof)["height"].asInt() / 16) * 16;
    
    if (prof->isMember("track_inhibit")){
      std::set<size_t> wouldSelect = Util::wouldSelect(
          M, std::string("audio=none&video=none&subtitle=none&") + (*prof)["track_inhibit"].asStringRef());
      if (wouldSelect.size()){
        if (prof->isMember("name")){
          INFO_MSG("Removing profile because track inhibitor matches: %s", (*prof)["name"].asStringRef().c_str());
        }else{
          INFO_MSG("Removing profile because track inhibitor matches: %s", prof->toString().c_str());
        }
        prof.remove();
        continue;
      }else{
        prof->removeMember("track_inhibit");
      }
    }
    INFO_MSG("Profile parsed: %s", prof->toString().c_str());
  }
  Mist::opt["target_profiles"] = pl["profiles"];

  //Connect to livepeer API
  HTTP::Downloader dl;
  if (!Mist::opt.isMember("access_token") || !Mist::opt["access_token"] || !Mist::opt["access_token"].isString()) {
    dl.setHeader("Authorization", "Bearer " + Mist::opt["access_token"].asStringRef());
  }

  if (Mist::opt.isMember("hardcoded_broadcasters") && Mist::opt["hardcoded_broadcasters"] &&
      Mist::opt["hardcoded_broadcasters"].isString()) {
    const std::string & hcbc = Mist::opt["hardcoded_broadcasters"].asStringRef();
    // Detect array
    if (hcbc.size() && hcbc[0] == '[') {
      Mist::lpBroad = JSON::fromString(hcbc);
      // If an array element is a string, assume it's the address field only
      jsonForEach (Mist::lpBroad, it) {
        if (it->isString()) {
          std::string tmp = it->asStringRef();
          (*it)["address"] = tmp;
        }
      }
    }
    // If the first letter is H, assume it's a single broadcaster's address field.
    if (hcbc.size() && hcbc[0] == 'h') {
      Mist::lpBroad.null();
      Mist::lpBroad[0u]["address"] = hcbc;
    }
  } else {
    // Get broadcaster list, pick first valid address
    if (!dl.get(HTTP::URL(api_url + "/broadcaster"))) {
      procExit.log(ER_FORMAT_SPECIFIC, 2, "Livepeer API rejected broadcaster list request");
      return procExit.flush(procStatePage);
    }
    Mist::lpBroad = JSON::fromString(dl.data());
  }
  if (!Mist::lpBroad || !Mist::lpBroad.isArray()){
    procExit.log(ER_FORMAT_SPECIFIC, 2, "No Livepeer broadcasters available (invalid response)");
    return procExit.flush(procStatePage);
  }
  Mist::pickRandomBroadcaster();
  if (!Mist::currBroadAddr.size()){
    procExit.log(ER_FORMAT_SPECIFIC, 2, "No Livepeer broadcasters available (empty list)");
    return procExit.flush(procStatePage);
  }
  INFO_MSG("Using broadcaster: %s", Mist::currBroadAddr.c_str());
  if (Mist::opt.isMember("access_token") && Mist::opt["access_token"] && Mist::opt["access_token"].isString()) {
    // send transcode request
    dl.setHeader("Content-Type", "application/json");
    dl.setHeader("Authorization", "Bearer " + Mist::opt["access_token"].asStringRef());
    if (!dl.post(HTTP::URL(api_url + "/stream"), pl.toString())) {
      procExit.log(ER_FORMAT_SPECIFIC, 2, "Livepeer API rejected encode request");
      return procExit.flush(procStatePage);
    }
    Mist::lpEnc = JSON::fromString(dl.data());
    if (!Mist::lpEnc) {
      procExit.log(ER_FORMAT_SPECIFIC, 2, "Livepeer API did not respond with JSON");
      return procExit.flush(procStatePage);
    }
    if (!Mist::lpEnc.isMember("id")) {
      procExit.log(ER_FORMAT_SPECIFIC, 2, "Livepeer API response missing stream ID");
      return procExit.flush(procStatePage);
    }
    Mist::lpID = Mist::lpEnc["id"].asStringRef();
  } else {
    // We don't want to use the same manifest ids for multiple proceses on the same stream
    // name, so we append a random string to the upload URL.
    Mist::lpID = Mist::opt["source"].asStringRef() + "-" + Util::getRandomAlphanumeric(8);
  }

  INFO_MSG("Livepeer transcode ID: %s", Mist::lpID.c_str());
  uint64_t lastProcUpdate = Util::bootSecs();
  pStat["proc_status_update"]["id"] = getpid();
  pStat["proc_status_update"]["proc"] = "Livepeer";
  pData["ainfo"]["lp_id"] = Mist::lpID;
  uint64_t startTime = Util::bootSecs();

  //Here be threads.

  //Source thread, from Mist to LP.
  std::thread source(sourceThread);
  while (!conf.is_active && Util::bootSecs() < lastProcUpdate + 5){Util::sleep(50);}
  if (!conf.is_active){WARN_MSG("Timeout waiting for source thread to boot!");}
  lastProcUpdate = Util::bootSecs();

  //Sink thread, from LP to Mist
  std::thread sink(sinkThread);
  while (!co.is_active && Util::bootSecs() < lastProcUpdate + 5){Util::sleep(50);}
  if (!co.is_active){WARN_MSG("Timeout waiting for sink thread to boot!");}
  lastProcUpdate = Util::bootSecs();

  // These threads upload prepared segments
  std::thread uploader0(uploadThread, 0);
  std::thread uploader1(uploadThread, 1);

  // Previous-window snapshots for pressure derivation.
  uint64_t prevTotalFails = 0;
  while (conf.is_active && co.is_active){
    Util::sleep(200);
    if (lastProcUpdate + 5 <= Util::bootSecs()){
      std::lock_guard<std::mutex> guard(statsMutex);
      pData["active_seconds"] = (Util::bootSecs() - startTime);
      pData["ainfo"]["switches"] = statSwitches;
      pData["ainfo"]["fail_non200"] = statFailN200.load();
      pData["ainfo"]["fail_timeout"] = statFailTimeout.load();
      pData["ainfo"]["fail_parse"] = statFailParse.load();
      pData["ainfo"]["fail_other"] = statFailOther.load();
      pData["ainfo"]["sourceTime"] = statSourceMs;
      pData["ainfo"]["sinkTime"] = statSinkMs;
      M.reloadReplacedPagesIfNeeded();
      if (M.getVod()) {
        uint64_t start = M.getFirstms(sourceIdx);
        uint64_t end = M.getLastms(sourceIdx);
        pData["ainfo"]["percent_done"] = 100 * (statSinkMs - start) / (end - start);
      }
      {
        std::lock_guard<std::mutex> guard(broadcasterMutex);
        pData["ainfo"]["bc"] = Mist::currBroadAddr;
      }
      Util::sendUDPApi(pStat);

      // Publish ProcState v2: timing + normalized pressure for the rate controller.
      if (procStatePage.mapped && ProcState::isValid(procStatePage)) {
        ProcState *s = (ProcState *)procStatePage.mapped;
        uint64_t nowBootMs = Util::bootMS();

        // Backlog: pre-segments that are written but not yet inserted.
        // Maintained as an atomic counter (incremented by the source thread
        // when filling a slot, decremented by the upload thread on insert /
        // reject); free of the data race the previous flag-scan had.
        uint32_t queueDepth = statQueueDepth.load(std::memory_order_relaxed);
        uint32_t inflight = statActiveUploads.load(std::memory_order_relaxed);
        uint64_t totExtUs = statTotalExternalUs.load(std::memory_order_relaxed);
        uint64_t totWorkUs = statTotalLocalWorkUs.load(std::memory_order_relaxed);
        uint64_t totSrcWaitUs = statTotalSourceWaitUs.load(std::memory_order_relaxed);
        uint64_t totSnkWaitUs = statTotalSinkWaitUs.load(std::memory_order_relaxed);
        uint64_t totalFails = statFailN200.load() + statFailTimeout.load() + statFailParse.load() + statFailOther.load();
        uint32_t retryDelta = (uint32_t)(totalFails - prevTotalFails);
        uint32_t obsSpeed = statLastSegSpeedQ16_16.load(std::memory_order_relaxed);

        // Only retry/queue_full are reported as proc-side hard signals.
        // External-gateway "can't keep up" pressure (observedSpeed < feederSpeed)
        // is computed by the controller, which is the only side that knows the
        // current feeder speed.
        uint8_t reason = PRC_REASON_UNKNOWN;
        uint16_t pressureQ = 0;
        uint8_t accept = 1;
        if (retryDelta > 0) {
          reason = PRC_REASON_RETRY;
          double p = 0.25 * (double)retryDelta;
          if (p > 1.0) p = 1.0;
          pressureQ = (uint16_t)(p * 65535.0);
          accept = 0;
        } else if (queueDepth >= PRESEG_COUNT) {
          reason = PRC_REASON_QUEUE_FULL;
          pressureQ = 60000; // ~0.92
          accept = 0;
        }

        s->totalWork = totWorkUs;
        s->totalSourceWait = totSrcWaitUs;
        s->totalSinkWait = totSnkWaitUs;
        s->totalExternalWait = totExtUs;
        s->frameCount = 0; // proc-defined; segments are not frames
        s->lastUpdateMs = nowBootMs;
        s->observedSpeedQ16_16 = obsSpeed;
        s->pressureQ0_16 = pressureQ;
        s->canAcceptMore = accept;
        s->reasonCode = reason;
        s->queueDepth = queueDepth;
        s->inflight = inflight;
        s->retryCount = retryDelta;
        s->negotiatedKind = PRC_KIND_LIVEPEER;

        prevTotalFails = totalFails;
      }

      lastProcUpdate = Util::bootSecs();
    }
  }
  INFO_MSG("Clean shutdown; joining threads");

  co.is_active = false;
  conf.is_active = false;

  sink.join();
  source.join();
  uploader0.join();
  uploader1.join();

  INFO_MSG("Shutdown reason: %s", Util::exitReason);

  return procExit.flush(procStatePage);
}

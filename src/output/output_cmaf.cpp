#include "output_cmaf.h"

#include <mist/bitfields.h>
#include <mist/checksum.h>
#include <mist/cmaf.h>
#include <mist/cmaf_live.h>
// #include <mist/defines.h>
// #include <mist/encode.h>
#include <mist/hls_support.h>
// #include <mist/mp4.h>
// #include <mist/mp4_dash.h>
// #include <mist/mp4_encryption.h>
// #include <mist/mp4_generic.h>
// #include <mist/timing.h>

int64_t bootMsOffset; // boot time in ms
uint64_t systemBoot;  // time since boot in ms
const std::string hlsMediaFormat = ".m4s";

uint64_t cmafBoot = Util::bootSecs();
uint64_t dataUp = 0;
uint64_t dataDown = 0;

namespace Mist{
  void CMAFPushTrack::connect(std::string debugParam){
    D.setHeader("Transfer-Encoding", "chunked");
    D.prepareRequest(url, "POST", D.getSocket());

    HTTP::Parser &http = D.getHTTP();
    http.sendingChunks = true;
    http.SendRequest(D.getSocket());

    if (debugParam.length()){
      if (debugParam[debugParam.length() - 1] != '/'){debugParam += '/';}
      debug = true;
      std::string filename = url.getUrl();
      filename.erase(0, filename.rfind("/") + 1);
      snprintf(debugName, 500, "%s%s-%" PRIu64, debugParam.c_str(), filename.c_str(),
               Util::bootMS());
      INFO_MSG("CMAF DEBUG FILE: %s", debugName);
      debugFile = fopen(debugName, "wb");
    }
  }

  void CMAFPushTrack::disconnect(){
    Socket::Connection &sock = D.getSocket();

    MP4::MFRA mfraBox;
    send(mfraBox.asBox(), mfraBox.boxedSize());
    send("");
    sock.close();

    if (debugFile){
      fclose(debugFile);
      debugFile = 0;
    }
  }

  void CMAFPushTrack::send(const char *data, size_t len){
    uint64_t preUp = D.getSocket().dataUp();
    uint64_t preDown = D.getSocket().dataDown();
    D.getHTTP().Chunkify(data, len, D.getSocket());
    if (debug && debugFile){fwrite(data, 1, len, debugFile);}
    dataUp += D.getSocket().dataUp() - preUp;
    dataDown += D.getSocket().dataDown() - preDown;
  }

  void CMAFPushTrack::send(const std::string &data){send(data.data(), data.size());}

  bool OutCMAF::isReadyForPlay(){
    if (!isInitialized){initialize();}
    meta.reloadReplacedPagesIfNeeded();
    if (!M.getValidTracks().size()){return false;}
    uint32_t mainTrack = M.mainTrack();
    if (mainTrack == INVALID_TRACK_ID){return false;}
    DTSC::Fragments fragments(M.fragments(mainTrack));
    return fragments.getValidCount() > 1;
  }

  OutCMAF::OutCMAF(Socket::Connection & conn, Util::Config & _cfg, JSON::Value & _capa)
    : HTTPOutput(conn, _cfg, _capa) {
    // load from global config
    systemBoot = Util::getGlobalConfig("systemBoot").asInt();
    // fall back to local calculation if loading from global config fails
    if (!systemBoot){systemBoot = (Util::unixMS() - Util::bootMS());}

    uaDelay = 0;
    realTime = 0;
    cmafLLStream = false;
    if (config->getString("target").size()){
      needsLookAhead = 5000;

      streamName = config->getString("streamname");
      std::string target = config->getString("target");
      target.replace(0, 4, "http"); // Translate to http for cmaf:// or https for cmafs://
      pushUrl = HTTP::URL(target);

      INFO_MSG("About to push stream %s out. Host: %s, port: %" PRIu32 ", location: %s",
               streamName.c_str(), pushUrl.host.c_str(), pushUrl.getPort(), pushUrl.path.c_str());
      myConn.setHost(pushUrl.host);
      initialize();
      initialSeek();
      startPushOut();
    }else{
      realTime = 0;
    }
  }

  void OutCMAF::connStats(uint64_t now, Comms::Connections &statComm){
    // For non-push usage, call usual function.
    if (!isRecording()){
      Output::connStats(now, statComm);
      return;
    }
    // For push output, this data is not coming from the usual place as we have multiple
    // connections to worry about.
    statComm.setUp(dataUp);
    statComm.setDown(dataDown);
    statComm.setTime(now - cmafBoot);
  }

  // Properly end all tracks on shutdown.
  OutCMAF::~OutCMAF(){
    for (std::map<size_t, Comms::Users>::iterator it = userSelect.begin(); it != userSelect.end();
         it++){
      onTrackEnd(it->first);
    }
  }

  void OutCMAF::init(Util::Config *cfg, JSON::Value & capa) {
    HTTPOutput::init(cfg, capa);
    capa["name"] = "CMAF";
    capa["friendly"] = "CMAF (fMP4) over HTTP (DASH, HLS7, HSS)";
    capa["desc"] = "Segmented streaming in CMAF (fMP4) format over HTTP";
    capa["url_rel"] = "/cmaf/$/";
    capa["url_prefix"] = "/cmaf/$/";
    capa["socket"] = "http_dash_mp4";
    capa["codecs"][0u][0u].append("+H264");
    capa["codecs"][0u][1u].append("+HEVC");
    capa["codecs"][0u][2u].append("+AAC");
    capa["codecs"][0u][3u].append("+AC3");
    capa["codecs"][0u][4u].append("+MP3");
    capa["codecs"][0u][5u].append("+subtitle");
    capa["encryption"].append("CTR128");

    capa["methods"][0u]["handler"] = "http";
    capa["methods"][0u]["type"] = "dash/video/mp4";
    capa["methods"][0u]["hrn"] = "DASH";
    capa["methods"][0u]["url_rel"] = "/cmaf/$/index.mpd";
    capa["methods"][0u]["priority"] = 8;

    capa["methods"][1u]["handler"] = "http";
    capa["methods"][1u]["type"] = "html5/application/vnd.apple.mpegurl;version=7";
    capa["methods"][1u]["hrn"] = "HLS (CMAF)";
    capa["methods"][1u]["url_rel"] = "/cmaf/$/index.m3u8";
    capa["methods"][1u]["priority"] = 8;

    capa["methods"][2u]["handler"] = "http";
    capa["methods"][2u]["type"] = "html5/application/vnd.ms-sstr+xml";
    capa["methods"][2u]["hrn"] = "MS Smooth Streaming";
    capa["methods"][2u]["url_rel"] = "/cmaf/$/Manifest";
    capa["methods"][2u]["priority"] = 8;

    // MP3 does not work in browsers
    capa["exceptions"]["codec:MP3"] = JSON::fromString("[[\"blacklist\",[\"Mozilla/\"]]]");

    cfg->addOption(
        "listlimit",
        JSON::fromString(
            "{\"arg\":\"integer\",\"default\":0,\"short\":\"y\",\"long\":\"list-limit\","
            "\"help\":\"Maximum number of segments in live playlists (0 = infinite).\"}"));
    capa["optional"]["listlimit"]["name"] = "Live playlist limit";
    capa["optional"]["listlimit"]["help"] =
        "Maximum number of parts in live playlists. (0 = infinite)";
    capa["optional"]["listlimit"]["default"] = 0;
    capa["optional"]["listlimit"]["type"] = "uint";
    capa["optional"]["listlimit"]["option"] = "--list-limit";

    cfg->addOption("chunkedsegments",
                   JSON::fromString("{\"short\":\"C\",\"long\":\"chunked-segments\","
                                    "\"help\":\"Use Transfer-Encoding: chunked for completed CMAF "
                                    "objects instead of buffering whole objects with Content-Length.\"}"));
    capa["optional"]["chunkedsegments"]["name"] = "Chunked segments";
    capa["optional"]["chunkedsegments"]["help"] =
      "Uses Transfer-Encoding: chunked for completed CMAF objects (init segments and finished "
      "media segments/parts). By default, completed objects are buffered and sent with a "
      "Content-Length for maximum compatibility. Does not affect the low-latency DASH "
      "forming-segment path, which is always chunked.";
    capa["optional"]["chunkedsegments"]["option"] = "--chunked-segments";
    capa["optional"]["chunkedsegments"]["short"] = "C";
    capa["optional"]["chunkedsegments"]["default"] = false;

    cfg->addOption("dashlowlatency",
                   JSON::fromString("{\"short\":\"D\",\"long\":\"dash-low-latency\","
                                    "\"help\":\"Enable experimental low-latency DASH (LL-DASH).\"}"));
    capa["optional"]["dashlowlatency"]["name"] = "Low-latency DASH (LL-DASH)";
    capa["optional"]["dashlowlatency"]["help"] =
      "Experimental. Enables DASH-IF low-latency DASH: advertises the still-forming segment in the "
      "MPD (availabilityTimeOffset + availabilityTimeComplete=false) and streams it over HTTP "
      "chunked transfer. Requires an LL-DASH-capable player. Independent of "
      "--chunked-segments; the forming segment is always chunked.";
    capa["optional"]["dashlowlatency"]["option"] = "--dash-low-latency";
    capa["optional"]["dashlowlatency"]["short"] = "D";
    capa["optional"]["dashlowlatency"]["default"] = false;

    cfg->addOption("mergesessions",
                   JSON::fromString("{\"short\":\"M\",\"long\":\"mergesessions\",\"help\":\"Merge "
                                    "together sessions from one user into a single session.\"}"));
    capa["optional"]["mergesessions"]["name"] = "Merge sessions";
    capa["optional"]["mergesessions"]["help"] =
        "If enabled, merges together all views from a single user into a single combined session. "
        "If disabled, each view (main playlist request) is a separate session.";
    capa["optional"]["mergesessions"]["option"] = "--mergesessions";

    cfg->addOption("chunkpath",
                   JSON::fromString("{\"arg\":\"string\",\"default\":\"\",\"short\":\"e\",\"long\":"
                                    "\"chunkpath\",\"help\":\"Alternate URL path to "
                                    "prepend to chunk paths, for serving through e.g. a CDN\"}"));
    capa["optional"]["chunkpath"]["name"] = "Prepend path for chunks";
    capa["optional"]["chunkpath"]["help"] =
        "Chunks will be served from this path. This also disables sessions IDs for chunks.";
    capa["optional"]["chunkpath"]["default"] = "";
    capa["optional"]["chunkpath"]["type"] = "str";
    capa["optional"]["chunkpath"]["option"] = "--chunkpath";
    capa["optional"]["chunkpath"]["short"] = "e";
    capa["optional"]["chunkpath"]["default"] = "";

    cfg->addStandardPushCapabilities(capa);
    capa["push_urls"].append("cmaf://*");
    capa["push_urls"].append("cmafs://*");

    JSON::Value opt;
    opt["arg"] = "string";
    opt["default"] = "";
    opt["arg_num"] = 1;
    opt["help"] = "Target CMAF URL to push out towards.";
    cfg->addOption("target", opt);
  }

  /******************************/
  /* HLS Manifest Generation */
  /******************************/

  /// \brief Builds master playlist for (LL)HLS.
  ///\return The master playlist file for (LL)HLS.
  void OutCMAF::sendHlsMasterManifest(){
    selectDefaultTracks();

    // check for forced "no low latency" parameter
    bool noLLHLS = H.GetVar("llhls").size() ? H.GetVar("llhls") == "0" : false;

    // Populate the struct that will help generate the master playlist
    const HLS::MasterData masterData ={
        false,//hasSessionIDs, unused
        noLLHLS,
        hlsMediaFormat == ".ts",
        getMainSelectedTrack(),
        H.GetHeader("User-Agent"),
        (Comms::tknMode & 0x04)?tkn:"",
        systemBoot,
        bootMsOffset,
    };

    std::stringstream result;
    HLS::addMasterManifest(result, M, userSelect, masterData);

    H.SetBody(result.str());
    H.SendResponse("200", "OK", myConn);
  }

  /// \brief Builds media playlist to (LL)HLS
  ///\return The media playlist file to (LL)HLS
  void OutCMAF::sendHlsMediaManifest(const size_t requestTid){
    if (!M.getValidTracks().count(requestTid)) {
      H.SendResponse("404", "Track not found", myConn);
      return;
    }

    const HLS::HlsSpecData hlsSpec ={H.GetVar("_HLS_skip"), H.GetVar("_HLS_msn"),
                                      H.GetVar("_HLS_part")};

    size_t timingTid = HLS::getTimingTrackId(M, H.GetVar("mTrack"), getMainSelectedTrack());

    // Chunkpath & Session ID logic
    std::string urlPrefix = "";
    if (config->getString("chunkpath").size()){
      urlPrefix = HTTP::URL(config->getString("chunkpath")).link("./" + H.url).link("./").getUrl();
    }

    // check for forced "no low latency" parameter
    bool noLLHLS = H.GetVar("llhls").size() ? H.GetVar("llhls") == "0" : false;
    // override if valid header forces "no low latency"
    noLLHLS = H.GetHeader("X-Mist-LLHLS").size() ? H.GetHeader("X-Mist-LLHLS") == "0" : noLLHLS;

    uint32_t targetDurationMax = (M.biggestFragment(timingTid) + 500) / 1000;
    if (!targetDurationMax) { targetDurationMax = 1; }

    const HLS::TrackData trackData = {
      M.getLive(),
      M.getType(requestTid) == "video",
      noLLHLS,
      hlsMediaFormat,
      M.getEncryption(requestTid),
      (Comms::tknMode & 0x04) ? tkn : "",
      timingTid,
      requestTid,
      targetDurationMax,
      (uint64_t)atol(H.GetVar("iMsn").c_str()),
      (uint64_t)(M.getLive() ? config->getInteger("listlimit") : 0),
      urlPrefix,
      systemBoot,
      bootMsOffset,
    };

    // Fragment & Key handlers
    DTSC::Fragments fragments(M.fragments(trackData.timingTrackId));
    DTSC::Keys keys(M.getKeys(trackData.timingTrackId));

    uint32_t bprErrCode = HLS::blockPlaylistReload(M, userSelect, trackData, hlsSpec, fragments, keys);
    if (bprErrCode == 400){
      H.SendResponse("400", "Bad Request: Invalid LLHLS parameter", myConn);
      return;
    }else if (bprErrCode == 503){
      H.SendResponse("503", "Service Unavailable", myConn);
      return;
    }

    HLS::FragmentData fragData;
    HLS::populateFragmentData(M, userSelect, fragData, trackData, fragments, keys);

    std::stringstream result;
    HLS::addStartingMetaTags(result, fragData, trackData, hlsSpec);
    HLS::addMediaFragments(result, M, fragData, trackData, fragments, keys);
    HLS::addEndingTags(result, M, userSelect, fragData, trackData);

    H.SetBody(result.str());
    H.SendResponse("200", "OK", myConn);
  }// namespace Mist

  void OutCMAF::sendHlsManifest(const std::string url){
    H.setCORSHeaders();
    H.SetHeader("Content-Type", "application/vnd.apple.mpegurl;version=7"); // for .m3u8
    H.SetHeader("Cache-Control", "no-store");
    if (H.method == "OPTIONS" || H.method == "HEAD"){
      H.SetBody("");
      H.SendResponse("200", "OK", myConn);
      return;
    }

    if (url.find("/") == std::string::npos){
      sendHlsMasterManifest();
    }else{
      sendHlsMediaManifest(atoll(url.c_str()));
    }
  }

  void OutCMAF::sendCmafError(const std::string & code, const std::string & message) {
    H.SetHeader("Content-Type", "text/plain; charset=utf-8");
    H.SetHeader("Cache-Control", "no-store");
    H.SetBody(message + "\n");
    H.SendResponse(code, message, myConn);
  }

  void OutCMAF::onHTTP(){
    initialize();
    bootMsOffset = 0;
    if (M.getLive()){bootMsOffset = M.getBootMsOffset();}

    if (H.url.find('/', 6) == std::string::npos){
      H.SendResponse("404", "Stream not found", myConn);
      return;
    }

    // Strip /cmaf/<streamname>/ from url
    std::string url = H.url.substr(H.url.find('/', 6) + 1);
    HTTP::URL req(reqUrl);


    if (tkn.size()){
      if (Comms::tknMode & 0x08){
        const std::string koekjes = H.GetHeader("Cookie");
        std::stringstream cookieHeader;
        cookieHeader << "tkn=" << tkn << "; Max-Age=" << SESS_TIMEOUT;
        H.SetHeader("Set-Cookie", cookieHeader.str()); 
      }
    }

    // Send a dash manifest for any URL with .mpd in the path
    if (req.getExt() == "mpd"){
      sendDashManifest();
      return;
    }

    // Send a hls manifest for any URL with index.m3u8 in the path
    if (req.getExt() == "m3u8"){
      sendHlsManifest(url);
      return;
    }

    // Send a smooth manifest for any URL with .mpd in the path
    if (url.find("Manifest") != std::string::npos){
      sendSmoothManifest();
      return;
    }

    const uint64_t msn = atoll(H.GetVar("msn").c_str());
    const uint64_t dur = atoll(H.GetVar("dur").c_str());
    const std::string mTrackParam = H.GetVar("mTrack");
    const uint64_t requestedMTrack = atoll(mTrackParam.c_str());

    H.SetHeader("Content-Type", "video/mp4"); // For .m4s
    if (hasSessionIDs() && !config->getOption("chunkpath")){
      H.SetHeader("Cache-Control", "no-store");
    }else{
      H.SetHeader("Cache-Control",
                  "public, max-age=" +
                      JSON::Value(M.getDuration(getMainSelectedTrack()) / 1000).asString() +
                      ", immutable");
      H.SetHeader("Pragma", "");
      H.SetHeader("Expires", "");
    }
    H.setCORSHeaders();
    if (H.method == "OPTIONS" || H.method == "HEAD"){
      H.SendResponse("200", "OK", myConn);
      return;
    }

    size_t idx = atoll(url.c_str());
    if (url.find("Q(") != std::string::npos){
      idx = atoll(url.c_str() + url.find("Q(") + 2) % 100;
    }
    if (!M.getValidTracks().count(idx)){
      sendCmafError("404", "Track not found");
      return;
    }

    const uint64_t mTrack = (mTrackParam.size() && M.getValidTracks().count(requestedMTrack)) ? requestedMTrack : idx;
    if (requestedMTrack != mTrack) {
      DEBUG_MSG(5, "CMAF segment request %s has no valid mTrack=%s, using request track %zu", url.c_str(),
                mTrackParam.size() ? mTrackParam.c_str() : "(missing)", idx);
    }

    if (url.find(hlsMediaFormat) == std::string::npos){
      sendCmafError("404", "File not found");
      return;
    }

    if (url.find("init" + hlsMediaFormat) != std::string::npos){
      std::string headerData = CMAF::trackHeader(M, idx);
      H.StartResponse(H, myConn, !config->getBool("chunkedsegments"));
      H.Chunkify(headerData.c_str(), headerData.size(), myConn);
      H.Chunkify("", 0, myConn);
      return;
    }

    // Select the right track
    userSelect.clear();
    userSelect[idx].reload(streamName, idx);

    uint64_t fragmentIndex;
    uint64_t startTime;
    uint32_t part;

    // set targetTime
    if (sscanf(url.c_str(), "%*d/chunk_%" PRIu64 ".%" PRIu32 ".*", &startTime, &part) == 2){
      // Logic: calculate targetTime for partial segments
      targetTime = CMAF::Live::getPartTargetTime(M, idx, mTrack, startTime, msn, part);
      if (!targetTime){
        sendCmafError("404", "Partial fragment does not exist");
        return;
      }
      startTime += part * CMAF::Live::partDurationMaxMs;
      fragmentIndex = M.getFragmentIndexForTime(mTrack, startTime);
      DEBUG_MSG(5, "partial segment requested: %s st %" PRIu64 " et %" PRIu64, url.c_str(),
                startTime, targetTime);
    }else if (sscanf(url.c_str(), "%*d/chunk_%" PRIu64 ".*", &startTime) == 1){
      // Logic: calculate targetTime for full segments
      if (M.getVod()){startTime += M.getFirstms(idx);}
      DTSC::Fragments fragments(M.fragments(mTrack));
      fragmentIndex = M.getFragmentIndexForTime(mTrack, startTime);
      // A request for the still-forming fragment (duration not yet known) is served
      // over chunked transfer: the fragment is streamed as a sequence of per-part
      // [moof][mdat] CMAF chunks into one open response (see sendNextLL). Only the
      // opt-in low-latency DASH path advertises such requests, and HLS never asks for
      // an incomplete fragment as a full segment, so complete-segment and HLS serving
      // are left untouched.
      if (config->getBool("dashlowlatency") && !dur && M.getLive()) {
        const uint64_t waitUntil = Util::bootMS() + (CMAF::Live::partDurationMaxMs * 4);
        while (true) {
          const bool exactFragment = fragmentIndex >= fragments.getFirstValid() &&
            fragmentIndex < fragments.getEndValid() && M.getTimeForFragmentIndex(mTrack, fragmentIndex) == startTime;
          if (exactFragment && !fragments.getDuration(fragmentIndex)) {
            const uint64_t firstPartEnd = CMAF::Live::getPartTargetTime(M, idx, mTrack, startTime, fragmentIndex, 0);
            if (firstPartEnd && CMAF::Live::isRangeServable(M, idx, startTime, firstPartEnd).ok) {
              const uint64_t segmentDuration = M.biggestFragment(mTrack);
              if (!segmentDuration) { break; }
              DEBUG_MSG(5, "Low-latency DASH chunked transfer for forming fragment %s track=%zu start=%" PRIu64,
                        url.c_str(), idx, startTime);
              cmafLLStream = true;
              cmafLLFragStart = startTime;
              cmafLLFragEnd = startTime + segmentDuration;
              cmafLLMsn = fragmentIndex;
              cmafLLmTrack = mTrack;
              cmafLLPartEnd = 0;
              cmafLLPartLeft = 0;
              cmafLLSeq = fragmentIndex;
              // Always chunked, independent of --chunked-segments: the final
              // Content-Length is unknowable while the segment is still being produced.
              H.StartResponse(H, myConn, false);
              seek(startTime);
              wantRequest = false;
              parseData = true;
              return;
            }
          }
          if (exactFragment || fragments.getEndValid() <= fragments.getFirstValid()) { break; }
          const uint64_t segmentDuration = M.biggestFragment(mTrack);
          if (!segmentDuration) { break; }
          const uint32_t previousFragment = fragments.getEndValid() - 1;
          const uint64_t previousStart = M.getTimeForFragmentIndex(mTrack, previousFragment);
          const uint64_t previousDuration = fragments.getDuration(previousFragment);
          const uint64_t expectedStart = previousStart + (previousDuration ? previousDuration : segmentDuration);
          if (startTime != expectedStart || Util::bootMS() >= waitUntil) { break; }
          Util::wait(25);
          meta.reloadReplacedPagesIfNeeded();
          fragmentIndex = M.getFragmentIndexForTime(mTrack, startTime);
        }
      }
      if (dur) {
        targetTime = startTime + dur;
      } else if (fragmentIndex >= fragments.getFirstValid() && fragmentIndex < fragments.getEndValid()) {
        if (M.getTimeForFragmentIndex(mTrack, fragmentIndex) != startTime) {
          sendCmafError("404", "Segment does not exist");
          return;
        }
        const uint64_t fragmentDuration = fragments.getDuration(fragmentIndex);
        if (fragmentDuration) {
          targetTime = startTime + fragmentDuration;
        } else if (fragmentIndex + 1 < fragments.getEndValid()) {
          targetTime = M.getTimeForFragmentIndex(mTrack, fragmentIndex + 1);
        } else {
          sendCmafError("404", "Segment does not exist");
          return;
        }
      } else {
        sendCmafError("404", "Segment outside live window");
        return;
      }
      DEBUG_MSG(5,
                "full segment requested: %s track=%zu mTrack=%" PRIu64 " msn=%" PRIu64 " dur=%" PRIu64 " st=%" PRIu64
                " et=%" PRIu64 " fragment=%" PRIu64,
                url.c_str(), idx, mTrack, msn, dur, startTime, targetTime, fragmentIndex);
    }else{
      sendCmafError("400", "Bad Request: Could not parse the url");
      return;
    }

    const uint64_t requestedStartTime = startTime;
    // Single shared servability check: identical rules to what manifests advertise.
    CMAF::Live::RangeServability serv = CMAF::Live::isRangeServable(M, idx, startTime, targetTime);
    switch (serv.reason) {
      case CMAF::Live::Reason::NO_VALID_PARTS:
        WARN_MSG("Refusing CMAF segment for track %zu: no media parts available url=%s", idx, url.c_str());
        sendCmafError("404", "Segment has no media data");
        return;
      case CMAF::Live::Reason::EXPIRED:
        WARN_MSG("Refusing expired CMAF segment for track %zu: requested=%" PRIu64 " first=%" PRIu64 " url=%s", idx,
                 requestedStartTime, M.getPartTime(serv.firstValidPart, idx), url.c_str());
        sendCmafError("404", "Segment expired");
        return;
      case CMAF::Live::Reason::OUTSIDE_WINDOW:
        WARN_MSG("Refusing out-of-range CMAF segment for track %zu: start=%" PRIu64 " target=%" PRIu64
                 " firstPart=%zu endPart=%zu valid=%zu-%zu url=%s",
                 idx, startTime, targetTime, serv.firstPart, serv.endPart, serv.firstValidPart, serv.endValidPart, url.c_str());
        sendCmafError("404", "Segment outside live window");
        return;
      case CMAF::Live::Reason::EMPTY_RANGE:
        WARN_MSG("Refusing invalid CMAF segment for track %zu: start=%" PRIu64 " target=%" PRIu64 " url=%s", idx,
                 serv.snappedStart, targetTime, url.c_str());
        sendCmafError("404", "Segment does not exist");
        return;
      case CMAF::Live::Reason::NO_PAYLOAD:
        WARN_MSG("Refusing empty CMAF segment for track %zu: start=%" PRIu64 " target=%" PRIu64 " url=%s", idx,
                 serv.snappedStart, targetTime, url.c_str());
        sendCmafError("404", "Segment has no media data");
        return;
      case CMAF::Live::Reason::OK: break;
    }

    if (serv.snappedStart != requestedStartTime) {
      DEBUG_MSG(5,
                "CMAF segment snapped to media boundary track=%zu requested=%" PRIu64 " mediaStart=%" PRIu64
                " target=%" PRIu64 " firstPart=%zu endPart=%zu url=%s",
                idx, requestedStartTime, serv.snappedStart, targetTime, serv.firstPart, serv.endPart, url.c_str());
    }
    startTime = serv.snappedStart;
    uint64_t payloadSize = serv.payloadSize;

    std::string headerData =
        CMAF::keyHeader(M, idx, startTime, targetTime, fragmentIndex, false, false);

    uint64_t mdatSize = 8 + payloadSize;
    DEBUG_MSG(5, "CMAF segment payload track=%zu start=%" PRIu64 " target=%" PRIu64 " header=%zu payload=%" PRIu64, idx,
              startTime, targetTime, headerData.size(), payloadSize);
    char mdatHeader[] ={0x00, 0x00, 0x00, 0x00, 'm', 'd', 'a', 't'};
    Bit::htobl(mdatHeader, mdatSize);

    H.StartResponse(H, myConn, !config->getBool("chunkedsegments"));
    H.Chunkify(headerData.c_str(), headerData.size(), myConn);
    H.Chunkify(mdatHeader, 8, myConn);

    seek(startTime);

    wantRequest = false;
    parseData = true;
  }

  /// Streams the in-progress fragment as a sequence of per-part [moof][mdat] CMAF
  /// chunks into one open response (low-latency DASH chunked transfer). The chunk a
  /// packet belongs to is derived from the packet's own timestamp, so the bytes
  /// written always land in the chunk whose moof/mdat describe them - robust to gaps
  /// or packets jumping across part boundaries. A chunk's moof is emitted only once
  /// the part is complete (getPartTargetTime blocks for it), so every mdat size
  /// matches its data. The response closes as soon as the final advertised byte of
  /// the fragment has been written, without waiting for a packet from the next
  /// fragment to arrive.
  void OutCMAF::sendNextLL() {
    const uint64_t t = thisPacket.getTime();
    // Stop once playback leaves the segment URL we are streaming. The time
    // boundary is authoritative here: live fragment-index updates may lag the
    // packet stream, but bytes from the next segment must never be emitted under
    // the previous segment URL.
    if (t < cmafLLFragStart || t >= cmafLLFragEnd || M.getFragmentIndexForTime(cmafLLmTrack, t) != cmafLLMsn) {
      DEBUG_MSG(5, "Low-latency DASH closing fragment at packet boundary track=%zu start=%" PRIu64 " end=%" PRIu64 " packet=%" PRIu64,
                thisIdx, cmafLLFragStart, cmafLLFragEnd, t);
      H.Chunkify("", 0, myConn);
      wantRequest = true;
      parseData = false;
      cmafLLStream = false;
      return;
    }
    // Index of the part-grid cell this packet falls in, relative to the fragment.
    const uint32_t chunkIdx = (uint32_t)((t - cmafLLFragStart) / CMAF::Live::partDurationMaxMs);
    // Open a new chunk only when this packet belongs to a part we have not opened yet.
    // Skipped (empty) cells get no chunk; a packet always lands in the chunk that
    // covers its own timestamp.
    if (!cmafLLPartEnd || t >= cmafLLPartEnd) {
      const uint64_t pStart = cmafLLFragStart + (uint64_t)chunkIdx * CMAF::Live::partDurationMaxMs;
      // Blocks until the part is complete; returns 0 / a clamped end at fragment end.
      const uint64_t pEnd = CMAF::Live::getPartTargetTime(M, thisIdx, cmafLLmTrack, cmafLLFragStart, cmafLLMsn, chunkIdx);
      CMAF::Live::RangeServability ps =
        (pEnd > pStart) ? CMAF::Live::isRangeServable(M, thisIdx, pStart, pEnd) : CMAF::Live::RangeServability();
      if (!pEnd || pEnd <= pStart || !ps.ok) {
        DEBUG_MSG(5,
                  "Low-latency DASH closing fragment on unavailable part track=%zu start=%" PRIu64 " end=%" PRIu64
                  " part=%u partStart=%" PRIu64 " partEnd=%" PRIu64,
                  thisIdx, cmafLLFragStart, cmafLLFragEnd, chunkIdx, pStart, pEnd);
        H.Chunkify("", 0, myConn);
        wantRequest = true;
        parseData = false;
        cmafLLStream = false;
        return;
      }
      std::string moof = CMAF::keyHeader(M, thisIdx, ps.snappedStart, pEnd, cmafLLSeq++, false, false);
      uint64_t mdatSize = 8 + ps.payloadSize;
      char mdatHeader[] = {0x00, 0x00, 0x00, 0x00, 'm', 'd', 'a', 't'};
      Bit::htobl(mdatHeader, mdatSize);
      H.Chunkify(moof.c_str(), moof.size(), myConn);
      H.Chunkify(mdatHeader, 8, myConn);
      cmafLLPartEnd = pEnd;
      cmafLLPartLeft = ps.payloadSize;
    }
    char *data;
    size_t dataLen;
    thisPacket.getString("data", data, dataLen);
    H.Chunkify(data, dataLen, myConn);
    if (dataLen >= cmafLLPartLeft) {
      cmafLLPartLeft = 0;
    } else {
      cmafLLPartLeft -= dataLen;
    }
    if (!cmafLLPartLeft && cmafLLPartEnd >= cmafLLFragEnd) {
      DEBUG_MSG(5, "Low-latency DASH completed fragment track=%zu start=%" PRIu64 " end=%" PRIu64, thisIdx,
                cmafLLFragStart, cmafLLFragEnd);
      H.Chunkify("", 0, myConn);
      wantRequest = true;
      parseData = false;
      cmafLLStream = false;
      return;
    }
  }

  void OutCMAF::sendNext(){
    if (isRecording()){
      pushNext();
      return;
    }
    if (cmafLLStream) {
      sendNextLL();
      return;
    }
    if (thisPacket.getTime() >= targetTime){
      HIGH_MSG("Finished playback to %" PRIu64, targetTime);
      wantRequest = true;
      parseData = false;
      H.Chunkify("", 0, myConn);
      return;
    }
    char *data;
    size_t dataLen;
    thisPacket.getString("data", data, dataLen);
    H.Chunkify(data, dataLen, myConn);
  }

  /***************************************************************************************************/
  /* Utility */
  /***************************************************************************************************/

  bool OutCMAF::tracksAligned(const std::set<size_t> &trackList){
    if (trackList.size() <= 1){return true;}

    size_t baseTrack = *trackList.begin();
    for (std::set<size_t>::iterator it = trackList.begin(); it != trackList.end(); ++it){
      if (*it == baseTrack){continue;}
      if (!M.tracksAlign(*it, baseTrack)){return false;}
    }
    return true;
  }

  OutCMAF::DashSegmentWindow
    OutCMAF::generateSegmentlist(size_t idx, std::stringstream & s,
                                 void dashSegmentCallBack(uint64_t, uint64_t, std::stringstream &, bool),
                                 uint64_t minStartTime, uint64_t maxEndTime, bool includeForming, size_t timingTrack) {
    DashSegmentWindow window;
    if (idx == INVALID_TRACK_ID || !M.getValidTracks().count(idx)) { return window; }
    if (timingTrack == INVALID_TRACK_ID) { timingTrack = idx; }
    if (!M.getValidTracks().count(timingTrack)) { return window; }
    DTSC::Fragments fragments(M.fragments(timingTrack));
    uint32_t firstFragment = fragments.getFirstValid();
    uint32_t lastFragment = fragments.getEndValid();
    // Do not use jitter/keep-away as a segment-list cutoff. It is playback
    // delay advice, not media availability. Complete fragments are filtered by
    // the servability predicate below; LL-DASH additionally lists the forming
    // fragment for chunked transfer once that fragment exists.
    DTSC::Keys keys(M.getKeys(timingTrack));
    if (M.getLive()) {
      DTSC::Parts parts(M.parts(idx));
      const size_t firstValidPart = parts.getFirstValid();
      const size_t endValidPart = parts.getEndValid();
      if (firstValidPart < endValidPart) {
        const uint64_t firstValidPartTime = M.getPartTime(firstValidPart, idx);
        while (firstFragment < lastFragment && keys.getTime(fragments.getFirstKey(firstFragment)) < firstValidPartTime) {
          ++firstFragment;
        }
      }
    }

    bool first = true;
    // skip the first two fragments if live
    if (M.getLive() && (lastFragment - firstFragment) > 6){firstFragment += 2;}

    for (; firstFragment < lastFragment; ++firstFragment){
      uint32_t duration = fragments.getDuration(firstFragment);
      uint64_t starttime = keys.getTime(fragments.getFirstKey(firstFragment));
      bool forming = false;
      if (!duration){
        if (M.getVod()) {
          duration = M.getLastms(idx) - starttime;
        } else if (includeForming) {
          // Still-forming fragment (LL-DASH only): its real duration isn't known yet, so use the
          // largest fragment as a nominal <S d> and let the next publishTime refresh reconcile it.
          // The caller only sets includeForming when the GOP is fixed (dashFixedGop), so this
          // nominal matches the actual duration; on variable GOP the forming segment is not listed.
          forming = true;
          duration = M.biggestFragment(timingTrack);
          if (!duration) { continue; }
        } else {
          continue; // skip last fragment when live
        }
      }
      // A complete fragment must be fully servable. A forming fragment only needs
      // to exist: the segment endpoint opens a chunked response and waits for the
      // first part before sending media bytes.
      const uint64_t checkEnd = forming ? starttime + CMAF::Live::partDurationMaxMs : starttime + duration;
      if (!forming && !CMAF::Live::isRangeServable(M, idx, starttime, checkEnd).ok) { continue; }
      if (M.getVod()) { starttime -= M.getFirstms(idx); }
      if (minStartTime && starttime + duration <= minStartTime) { continue; }
      if (maxEndTime && starttime >= maxEndTime) { continue; }
      if (!window.count) { window.start = starttime; }
      window.end = starttime + duration;
      ++window.count;
      dashSegmentCallBack(starttime, duration, s, first);
      first = false;
    }
    return window;

    /*LTS-START
    // remove lines to reduce size towards listlimit setting - but keep at least 4X target
    // duration available
    uint64_t listlimit = config->getInteger("listlimit");
    if (listlimit){
      while (lines.size() > listlimit &&
             (totalDuration - durations.front()) > (targetDuration * 4000)){
        lines.pop_front();
        totalDuration -= durations.front();
        durations.pop_front();
        ++skippedLines;
      }
    }
    LTS-END*/
  }

  std::string OutCMAF::buildNalUnit(size_t len, const char *data){
    char *res = (char *)malloc(len + 4);
    Bit::htobl(res, len);
    memcpy(res + 4, data, len);
    return std::string(res, len + 4);
  }

  std::string OutCMAF::h264init(const std::string &initData){
    char res[7];
    snprintf(res, 7, "%.2X%.2X%.2X", initData[1], initData[2], initData[3]);
    return res;
  }

  std::string OutCMAF::h265init(const std::string &initData){
    char res[17];
    snprintf(res, 17, "%.2X%.2X%.2X%.2X%.2X%.2X%.2X%.2X", initData[1], initData[6], initData[7],
             initData[8], initData[9], initData[10], initData[11], initData[12]);
    return res;
  }

  /*********************************/
  /* MPEG-DASH Manifest Generation */
  /*********************************/

  void OutCMAF::sendDashManifest(){
    std::string method = H.method;
    H.Clean();
    H.SetHeader("Content-Type", "application/dash+xml");
    H.SetHeader("Cache-Control", "no-store");
    H.setCORSHeaders();
    // The HTTP Date header backs up in-MPD UTCTiming for client clock sync
    // and keeps MPD responses aligned with normal HTTP cache semantics.
    {
      time_t nowSec = (time_t)Util::epoch();
      struct tm *gmt = gmtime(&nowSec);
      char dateBuf[40];
      if (gmt && strftime(dateBuf, sizeof(dateBuf), "%a, %d %b %Y %H:%M:%S GMT", gmt)) { H.SetHeader("Date", dateBuf); }
    }
    if (method == "OPTIONS" || method == "HEAD"){
      H.SendResponse("200", "OK", myConn);
      H.Clean();
      return;
    }
    const std::string manifest = dashManifest();
    if (!manifest.size()) {
      H.SetHeader("Content-Type", "text/plain; charset=utf-8");
      H.SetBody("DASH manifest not ready\n");
      H.SendResponse("503", "DASH manifest not ready", myConn);
      H.Clean();
      return;
    }
    H.SetBody(manifest);
    H.SendResponse("200", "OK", myConn);
    H.Clean();
  }

  void dashSegment(uint64_t start, uint64_t duration, std::stringstream &s, bool first){
    s << "<S ";
    if (first){s << "t=\"" << start << "\" ";}
    s << "d=\"" << duration << "\" />" << std::endl;
  }

  void dashSegmentNoop(uint64_t, uint64_t, std::stringstream &, bool) {}

  std::string OutCMAF::dashTime(uint64_t time){
    std::stringstream r;
    r << "PT";
    if (time >= 3600000){r << (time / 3600000) << "H";}
    if (time >= 60000){r << (time / 60000) % 60 << "M";}
    r << (time / 1000) % 60 << "." << std::setfill('0') << std::setw(3) << (time % 1000) << "S";
    return r.str();
  }

  static uint64_t selectedMaxFragmentDurationMs(const DTSC::Meta & M, const std::set<size_t> & vTracks,
                                                const std::set<size_t> & aTracks) {
    uint64_t targetDurationMs = 0;
    for (std::set<size_t>::const_iterator it = vTracks.begin(); it != vTracks.end(); ++it) {
      targetDurationMs = std::max<uint64_t>(targetDurationMs, M.biggestFragment(*it));
    }
    for (std::set<size_t>::const_iterator it = aTracks.begin(); it != aTracks.end(); ++it) {
      targetDurationMs = std::max<uint64_t>(targetDurationMs, M.biggestFragment(*it));
    }
    return std::max<uint64_t>(targetDurationMs, 2000);
  }

  static uint64_t selectedKeepAwayMs(const DTSC::Meta & M, const std::map<size_t, Comms::Users> & userSelect) {
    uint64_t keepAwayMs = 0;
    for (std::map<size_t, Comms::Users>::const_iterator it = userSelect.begin(); it != userSelect.end(); ++it) {
      if (it->first == INVALID_TRACK_ID) { continue; }
      keepAwayMs = std::max<uint64_t>(keepAwayMs, M.getMinKeepAway(it->first));
    }
    const uint64_t maxKeepAwayMs = M.getMaxKeepAway();
    if (maxKeepAwayMs && keepAwayMs > maxKeepAwayMs) { keepAwayMs = maxKeepAwayMs; }
    return keepAwayMs;
  }

  static double dashAvailabilityTimeOffset(const DTSC::Meta & M, size_t idx, bool includeForming) {
    if (!includeForming || idx == INVALID_TRACK_ID) { return 0.0; }
    const uint64_t segmentDurationMs = M.biggestFragment(idx);
    if (!segmentDurationMs) { return 0.0; }
    return segmentDurationMs / 1000.0;
  }

  /// True only if the recent COMPLETE fragments on every given track have near-equal duration
  /// (a fixed/stable GOP). LL-DASH advertises the still-forming segment with a nominal duration;
  /// that only stays honest when the GOP is fixed, so this gates that advertisement. Conservative:
  /// returns false unless there is enough evidence (>=3 complete fragments per track, within 10%).
  static bool dashFixedGop(const DTSC::Meta & M, const std::set<size_t> & vTracks, const std::set<size_t> & aTracks) {
    std::set<size_t> tracks = vTracks;
    tracks.insert(aTracks.begin(), aTracks.end());
    for (std::set<size_t>::const_iterator it = tracks.begin(); it != tracks.end(); ++it) {
      DTSC::Fragments fr(M.fragments(*it));
      const uint32_t first = fr.getFirstValid();
      const uint32_t end = fr.getEndValid();
      uint64_t ref = 0;
      uint32_t checked = 0;
      for (uint32_t i = end; i > first && checked < 6; --i) {
        const uint64_t d = fr.getDuration(i - 1);
        if (!d) { continue; } // skip the still-forming / zero-duration fragment
        if (!ref) {
          ref = d;
        } else if (d > ref + ref / 10 || d + ref / 10 < ref) {
          return false;
        } // >10% deviation
        ++checked;
      }
      if (checked < 3) { return false; } // not enough complete fragments to be confident
    }
    return true;
  }

  void OutCMAF::dashAdaptationSet(size_t id, size_t idx, std::stringstream &r){
    std::string type = M.getType(idx);
    r << "<AdaptationSet group=\"" << id << "\" mimeType=\"" << type << "/mp4\" ";
    if (type == "video"){
      r << "width=\"" << M.getWidth(idx) << "\" height=\"" << M.getHeight(idx) << "\" ";
      const uint64_t fpks = M.getFpks(idx);
      if (fpks) {
        r << "frameRate=\"";
        if (fpks % 1000) {
          r << fpks << "/1000";
        } else {
          r << fpks / 1000;
        }
        r << "\" ";
      }
    }
    r << "segmentAlignment=\"true\" id=\"" << idx
      << "\" startWithSAP=\"1\" subsegmentAlignment=\"true\" subsegmentStartsWithSAP=\"1\">"
      << std::endl;
  }

  void OutCMAF::dashRepresentation(size_t id, size_t idx, std::stringstream &r){
    std::string codec = M.getCodec(idx);
    std::string type = M.getType(idx);
    r << "<Representation id=\"" << idx << "\" bandwidth=\"" << M.getBps(idx) * 8 << "\" codecs=\"";
    r << Util::codecString(M.getCodec(idx), M.getInit(idx));
    r << "\" ";
    if (type == "audio"){
      r << "audioSamplingRate=\"" << M.getRate(idx)
        << "\"> <AudioChannelConfiguration "
           "schemeIdUri=\"urn:mpeg:dash:23003:3:audio_channel_configuration:2011\" value=\""
        << M.getChannels(idx) << "\" /></Representation>" << std::endl;
    }else{
      r << "/>";
    }
  }

  void OutCMAF::dashSegmentTemplate(std::stringstream & r, double availabilityTimeOffset, size_t timingTrack, size_t requestTrack) {
    r << "<SegmentTemplate timescale=\"1000\" ";
    // LL-DASH: signal that segments may be fetched before they are complete, and how
    // far ahead of nominal completion (in seconds) they first become available.
    if (availabilityTimeOffset > 0) {
      r << "availabilityTimeOffset=\"" << availabilityTimeOffset << "\" availabilityTimeComplete=\"false\" ";
    }
    r << "media=\"$RepresentationID$/chunk_$Time$.m4s";
    if (timingTrack != INVALID_TRACK_ID && requestTrack != INVALID_TRACK_ID && timingTrack != requestTrack) {
      r << "?mTrack=" << timingTrack;
    }
    r << "\" "
         "initialization=\"$RepresentationID$/init.m4s\"><SegmentTimeline>"
      << std::endl;
  }

  void OutCMAF::dashAdaptation(size_t id, std::set<size_t> tracks, bool aligned, std::stringstream & r,
                               uint64_t minStartTime, uint64_t maxEndTime, bool includeForming, size_t timingTrack) {
    if (!tracks.size()){return;}
    if (aligned){
      size_t firstTrack = *tracks.begin();
      dashAdaptationSet(id, *tracks.begin(), r);
      const size_t trackTiming = timingTrack == INVALID_TRACK_ID ? firstTrack : timingTrack;
      dashSegmentTemplate(r, dashAvailabilityTimeOffset(M, trackTiming, includeForming), trackTiming, firstTrack);
      generateSegmentlist(firstTrack, r, dashSegment, minStartTime, maxEndTime, includeForming, trackTiming);
      r << "</SegmentTimeline></SegmentTemplate>" << std::endl;
      for (std::set<size_t>::iterator it = tracks.begin(); it != tracks.end(); it++){
        dashRepresentation(id, *it, r);
      }
      r << "</AdaptationSet>" << std::endl;
      return;
    }
    for (std::set<size_t>::iterator it = tracks.begin(); it != tracks.end(); it++){
      std::string codec = M.getCodec(*it);
      std::string type = M.getType(*it);
      const size_t trackTiming = timingTrack == INVALID_TRACK_ID ? *it : timingTrack;
      dashAdaptationSet(id, *it, r);
      dashSegmentTemplate(r, dashAvailabilityTimeOffset(M, trackTiming, includeForming), trackTiming, *it);
      generateSegmentlist(*it, r, dashSegment, minStartTime, maxEndTime, includeForming, trackTiming);
      r << "</SegmentTimeline></SegmentTemplate>" << std::endl;
      dashRepresentation(id, *it, r);
      r << "</AdaptationSet>" << std::endl;
    }
  }

  /// Returns a string with the full XML DASH manifest MPD file.
  std::string OutCMAF::dashManifest(bool checkAlignment){
    initialize();
    selectDefaultTracks();
    std::set<size_t> vTracks;
    std::set<size_t> aTracks;
    std::set<size_t> sTracks;
    for (std::map<size_t, Comms::Users>::iterator it = userSelect.begin(); it != userSelect.end();
         it++){
      if (M.getType(it->first) == "video"){vTracks.insert(it->first);}
      if (M.getType(it->first) == "audio"){aTracks.insert(it->first);}
      if (M.getCodec(it->first) == "subtitle"){sTracks.insert(it->first);}
    }

    if (!vTracks.size() && !aTracks.size()){return "";}

    std::set<size_t> dashVTracks = vTracks;
    std::set<size_t> dashATracks = aTracks;

    std::stringstream r;
    r << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>" << std::endl;
    r << "<MPD ";
    size_t mainTrack = getMainSelectedTrack();
    size_t mainDuration = M.getDuration(mainTrack);
    uint64_t dashWindowStart = 0;
    uint64_t dashWindowEnd = 0;
    uint64_t dashLatencyTargetMs = 0; ///< live low-latency target, drives ServiceDescription (0 = none)
    bool dashLowLatency = false; ///< LL-DASH timing/signalling enabled
    bool dashForming = false; ///< advertise the still-forming segment (LL + fixed GOP only)
    if (M.getVod()){
      r << "type=\"static\" mediaPresentationDuration=\"" << dashTime(mainDuration)
        << "\" minBufferTime=\"PT1.5S\" ";
    }else{
      // Low-latency DASH is DASH-specific: it advertises and serves the still-forming
      // segment over chunked transfer. It is independent of --chunked-segments,
      // which only governs the framing of completed CMAF objects.
      const bool dashLowLatencyRequested = config->getBool("dashlowlatency");
      // Only advertise LL-DASH when the GOP is fixed, so the still-forming segment's nominal <S d>
      // matches what the endpoint will stream. Variable GOP falls back to standard DASH signalling.
      dashForming = dashLowLatencyRequested && dashFixedGop(M, vTracks, aTracks);
      dashLowLatency = dashForming;
      const size_t dashTimingTrack = vTracks.size() ? *vTracks.begin() : INVALID_TRACK_ID;
      bool hasDashWindow = false;
      std::stringstream ignoredSegments;
      dashVTracks.clear();
      dashATracks.clear();
      for (std::set<size_t>::iterator it = vTracks.begin(); it != vTracks.end(); ++it) {
        DashSegmentWindow trackWindow = generateSegmentlist(*it, ignoredSegments, dashSegmentNoop, 0, 0, dashForming, *it);
        if (trackWindow.count) {
          dashVTracks.insert(*it);
          if (!hasDashWindow || trackWindow.start > dashWindowStart) { dashWindowStart = trackWindow.start; }
          if (!hasDashWindow || trackWindow.end < dashWindowEnd) { dashWindowEnd = trackWindow.end; }
          hasDashWindow = true;
        }
      }
      for (std::set<size_t>::iterator it = aTracks.begin(); it != aTracks.end(); ++it) {
        const size_t trackTiming = dashTimingTrack == INVALID_TRACK_ID ? *it : dashTimingTrack;
        DashSegmentWindow trackWindow = generateSegmentlist(*it, ignoredSegments, dashSegmentNoop, 0, 0, dashForming, trackTiming);
        if (trackWindow.count) {
          dashATracks.insert(*it);
          if (!hasDashWindow || trackWindow.start > dashWindowStart) { dashWindowStart = trackWindow.start; }
          if (!hasDashWindow || trackWindow.end < dashWindowEnd) { dashWindowEnd = trackWindow.end; }
          hasDashWindow = true;
        }
      }
      if (!hasDashWindow || dashWindowEnd <= dashWindowStart) { return ""; }
      std::set<size_t> filteredVTracks;
      std::set<size_t> filteredATracks;
      for (std::set<size_t>::iterator it = dashVTracks.begin(); it != dashVTracks.end(); ++it) {
        if (generateSegmentlist(*it, ignoredSegments, dashSegmentNoop, dashWindowStart, dashWindowEnd, dashForming, *it).count) {
          filteredVTracks.insert(*it);
        }
      }
      for (std::set<size_t>::iterator it = dashATracks.begin(); it != dashATracks.end(); ++it) {
        const size_t trackTiming = dashTimingTrack == INVALID_TRACK_ID ? *it : dashTimingTrack;
        if (generateSegmentlist(*it, ignoredSegments, dashSegmentNoop, dashWindowStart, dashWindowEnd, dashForming, trackTiming)
              .count) {
          filteredATracks.insert(*it);
        }
      }
      dashVTracks.swap(filteredVTracks);
      dashATracks.swap(filteredATracks);
      if (!dashVTracks.size() && !dashATracks.size()) { return ""; }
      mainDuration = dashWindowEnd - dashWindowStart;
      const uint64_t streamStartMs =
        M.getUTCOffset() ? M.packetTimeToUnixMs(0) : M.getBootMsOffset() + (Util::unixMS() - Util::bootMS());
      // Keep availabilityStartTime in milliseconds; whole-second truncation shifts
      // segment-availability calculations by up to one second at the live edge.
      const uint64_t availabilityStartMs = streamStartMs ? streamStartMs : (Util::epoch() * 1000 - dashWindowEnd);
      const uint64_t targetDurationMs = selectedMaxFragmentDurationMs(M, dashVTracks, dashATracks);
      const uint64_t keepAwayMs = selectedKeepAwayMs(M, userSelect);
      uint64_t suggestedPresentationDelay;
      uint64_t minimumUpdatePeriodMs;
      uint64_t minBufferMs;
      if (dashLowLatency) {
        // LL-DASH: the forming segment is delivered over chunked transfer (parts as produced),
        // signalled with availabilityTimeOffset below. Keep the presentation delay above the
        // part cadence so the player can read the forming segment without draining its buffer.
        minBufferMs = CMAF::Live::partDurationMaxMs * 3; // ~1.5s
        suggestedPresentationDelay = CMAF::Live::partDurationMaxMs * 6 + keepAwayMs; // ~3s, >= minBuffer
        // Refresh near the part cadence (not targetDur/2) so the timeline / forming-segment info
        // stays fresh for the player.
        minimumUpdatePeriodMs = std::max<uint64_t>(CMAF::Live::partDurationMaxMs * 2, 1000); // ~1s
        dashLatencyTargetMs = suggestedPresentationDelay;
      } else {
        // Standard DASH: complete segments only. The newest listed segment is ~1 targetDuration
        // behind the live edge (the forming segment is never listed), so two durations plus the
        // stream's keep-away keeps the play point safely behind it without the extra-conservative
        // 3x delay. No low-latency timing/signalling.
        suggestedPresentationDelay = targetDurationMs * 2 + keepAwayMs;
        minimumUpdatePeriodMs = 2000;
        minBufferMs = 2000;
      }
      r << "type=\"dynamic\" minimumUpdatePeriod=\"" << dashTime(minimumUpdatePeriodMs) << "\" availabilityStartTime=\""
        << Util::getUTCStringMillis(availabilityStartMs) << "\" timeShiftBufferDepth=\"" << dashTime(mainDuration)
        << "\" suggestedPresentationDelay=\"" << dashTime(suggestedPresentationDelay) << "\" minBufferTime=\""
        << dashTime(minBufferMs) << "\" publishTime=\"" << Util::getUTCStringMillis(Util::unixMS()) << "\" ";
    }

    r << "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" ";
    r << "xmlns:xlink=\"http://www.w3.org/1999/xlink\" ";
    r << "xsi:schemaLocation=\"urn:mpeg:DASH:schema:MPD:2011 "
         "http://standards.iso.org/ittf/PubliclyAvailableStandards/MPEG-DASH_schema_files/"
         "DASH-MPD.xsd\" ";
    r << "profiles=\"urn:mpeg:dash:profile:isoff-live:2011\" "
         "xmlns=\"urn:mpeg:dash:schema:mpd:2011\" >"
      << std::endl;
    r << "<ProgramInformation><Title>" << streamName << "</Title></ProgramInformation>"
      << std::endl;
    // Low-latency DASH only: advertise a target latency backed by chunked transfer
    // of the forming segment. Standard DASH emits no ServiceDescription.
    if (dashLowLatency) {
      // target = our SPD; min/max bound how far dash.js may drift while catching up / recovering
      // after a stall (min ~3 parts, max ~2x target) so it doesn't chase an unachievable edge.
      const uint64_t latencyMinMs = CMAF::Live::partDurationMaxMs * 3;
      const uint64_t latencyMaxMs = dashLatencyTargetMs * 2;
      r << "<ServiceDescription id=\"0\"><Latency target=\"" << dashLatencyTargetMs << "\" min=\"" << latencyMinMs
        << "\" max=\"" << latencyMaxMs
        << "\" referenceId=\"0\" /><PlaybackRate min=\"0.96\" max=\"1.04\" /></ServiceDescription>" << std::endl;
    }
    r << "<Period " << (M.getLive() ? "start=\"PT0.0S\"" : "") << ">" << std::endl;

    bool videoAligned = checkAlignment && tracksAligned(dashVTracks);
    bool audioAligned = checkAlignment && tracksAligned(dashATracks);
    const size_t dashTimingTrack = dashVTracks.size() ? *dashVTracks.begin() : INVALID_TRACK_ID;
    dashAdaptation(1, dashVTracks, videoAligned, r, dashWindowStart, dashWindowEnd, dashForming);
    dashAdaptation(2, dashATracks, audioAligned, r, dashWindowStart, dashWindowEnd, dashForming, dashTimingTrack);

    if (sTracks.size()){
      for (std::set<size_t>::iterator it = sTracks.begin(); it != sTracks.end(); it++){
        std::string lang = (M.getLang(*it) == "" ? "unknown" : M.getLang(*it));
        r << "<AdaptationSet id=\"" << *it << "\" group=\"3\" mimeType=\"text/vtt\" lang=\"" << lang
          << "\"><Representation id=\"" << *it << "\" bandwidth=\"256\"><BaseURL>../../"
          << streamName << ".vtt?track=" << *it << "</BaseURL></Representation></AdaptationSet>"
          << std::endl;
      }
    }

    r << "</Period>" << std::endl;
    // Give the client a server clock to anchor segment-availability math to. The direct
    // scheme embeds the server's current UTC inline and refreshes on each MPD reload.
    if (M.getLive()) {
      r << "<UTCTiming schemeIdUri=\"urn:mpeg:dash:utc:direct:2014\" value=\""
        << Util::getUTCStringMillis(Util::unixMS()) << "\" />" << std::endl;
    }
    r << "</MPD>" << std::endl;

    return r.str();
  }

  /****************************************/
  /* Smooth Streaming Manifest Generation */
  /****************************************/

  std::string toUTF16(const std::string &original){
    std::string result;
    result.append("\377\376", 2);
    for (std::string::const_iterator it = original.begin(); it != original.end(); it++){
      result += (*it);
      result.append("\000", 1);
    }
    return result;
  }

  /// Converts bytes per second and track ID into a single bits per second value, where the last
  /// two digits are the track ID. Breaks for track IDs > 99. But really, this is MS-SS, so who
  /// cares..?
  uint64_t bpsAndIdToBitrate(uint32_t bps, uint64_t tid){
    return ((uint64_t)((bps * 8) / 100)) * 100 + tid;
  }

  void smoothSegment(uint64_t start, uint64_t duration, std::stringstream &s, bool first){
    s << "<c ";
    if (first){s << "t=\"" << start << "\" ";}
    s << "d=\"" << duration << "\" />" << std::endl;
  }

  void OutCMAF::sendSmoothManifest(){
    std::string method = H.method;
    H.Clean();
    H.SetHeader("Content-Type", "application/dash+xml");
    H.SetHeader("Cache-Control", "no-store");
    H.setCORSHeaders();
    if (method == "OPTIONS" || method == "HEAD"){
      H.SendResponse("200", "OK", myConn);
      H.Clean();
      return;
    }
    H.SetBody(smoothManifest());
    H.SendResponse("200", "OK", myConn);
    H.Clean();
  }

  void OutCMAF::smoothAdaptation(const std::string &type, std::set<size_t> tracks,
                                 std::stringstream &r){
    if (!tracks.size()){return;}
    DTSC::Keys keys(M.getKeys(*tracks.begin()));
    r << "<StreamIndex Type=\"" << type << "\" QualityLevels=\"" << tracks.size() << "\" Name=\""
      << type << "\" Chunks=\"" << keys.getValidCount() << "\" Url=\"Q({bitrate})/"
      << "chunk_{start_time}.m4s\" ";
    if (type == "video"){
      size_t maxWidth = 0;
      size_t maxHeight = 0;

      for (std::set<size_t>::iterator it = tracks.begin(); it != tracks.end(); it++){
        size_t width = M.getWidth(*it);
        size_t height = M.getHeight(*it);
        if (width > maxWidth){maxWidth = width;}
        if (height > maxHeight){maxHeight = height;}
      }
      r << "MaxWidth=\"" << maxWidth << "\" MaxHeight=\"" << maxHeight << "\" DisplayWidth=\""
        << maxWidth << "\" DisplayHeight=\"" << maxHeight << "\"";
    }
    r << ">\n";
    size_t index = 0;
    for (std::set<size_t>::iterator it = tracks.begin(); it != tracks.end(); it++){
      r << "<QualityLevel Index=\"" << index++ << "\" Bitrate=\""
        << bpsAndIdToBitrate(M.getBps(*it) * 8, *it) << "\" CodecPrivateData=\"" << std::hex;
      if (type == "audio"){
        std::string init = M.getInit(*it);
        for (unsigned int i = 0; i < init.size(); i++){
          r << std::setfill('0') << std::setw(2) << std::right << (int)init[i];
        }
        r << std::dec << "\" SamplingRate=\"" << M.getRate(*it)
          << "\" Channels=\"2\" BitsPerSample=\"16\" PacketSize=\"4\" AudioTag=\"255\" "
             "FourCC=\"AACL\" />\n";
      }
      if (type == "video"){
        MP4::AVCC avccbox;
        avccbox.setPayload(M.getInit(*it));
        std::string tmpString = avccbox.asAnnexB();
        for (size_t i = 0; i < tmpString.size(); i++){
          r << std::setfill('0') << std::setw(2) << std::right << (int)tmpString[i];
        }
        r << std::dec << "\" MaxWidth=\"" << M.getWidth(*it) << "\" MaxHeight=\""
          << M.getHeight(*it) << "\" FourCC=\"AVC1\" />\n";
      }
    }
    generateSegmentlist(*tracks.begin(), r, smoothSegment);
    r << "</StreamIndex>\n";
  }

  /// Returns a string with the full XML DASH manifest MPD file.
  std::string OutCMAF::smoothManifest(bool checkAlignment){
    initialize();

    std::stringstream r;
    r << "<?xml version=\"1.0\" encoding=\"utf-16\"?>\n"
         "<SmoothStreamingMedia MajorVersion=\"2\" MinorVersion=\"0\" TimeScale=\"1000\" ";

    selectDefaultTracks();
    std::set<size_t> vTracks;
    std::set<size_t> aTracks;
    for (std::map<size_t, Comms::Users>::iterator it = userSelect.begin(); it != userSelect.end();
         it++){
      if (M.getType(it->first) == "video"){vTracks.insert(it->first);}
      if (M.getType(it->first) == "audio"){aTracks.insert(it->first);}
    }

    if (!aTracks.size() && !vTracks.size()){
      FAIL_MSG("No valid tracks found");
      return "";
    }

    if (M.getVod()){
      r << "Duration=\"" << M.getLastms(vTracks.size() ? *vTracks.begin() : *aTracks.begin())
        << "\">\n";
    }else{
      r << "Duration=\"0\" IsLive=\"TRUE\" LookAheadFragmentCount=\"2\" DVRWindowLength=\""
        << M.getBufferWindow() << "\" CanSeek=\"TRUE\" CanPause=\"TRUE\">\n";
    }

    smoothAdaptation("audio", aTracks, r);
    smoothAdaptation("video", vTracks, r);
    r << "</SmoothStreamingMedia>\n";

    return toUTF16(r.str());
  }

  /**********************************/
  /* CMAF Push Output functionality */
  /**********************************/

  // When we disconnect a track, or when we're done pushing out, send an empty 'mfra' box to
  // indicate track end.
  void OutCMAF::onTrackEnd(size_t idx){
    if (!isRecording()){return;}
    if (!pushTracks.count(idx) || !pushTracks.at(idx).D.getSocket()){return;}
    INFO_MSG("Disconnecting track %zu", idx);
    pushTracks[idx].disconnect();
    pushTracks.erase(idx);
  }

  // Create the connections and post request needed to start pushing out a track.
  void OutCMAF::setupTrackObject(size_t idx){
    CMAFPushTrack &track = pushTracks[idx];
    track.url = pushUrl;
    if (targetParams.count("usp") && targetParams["usp"] == "1"){
      std::string usp_path = "Streams(" + M.getTrackIdentifier(idx) + ")";
      track.url = track.url.link(usp_path);
    }else{
      track.url.path += "/";
      track.url = track.url.link(M.getTrackIdentifier(idx));
    }

    track.connect(targetParams["debug"]);

    std::string header = CMAF::trackHeader(M, idx, true);
    track.send(header);
  }

  /// Function that waits at most `maxWait` ms (in steps of 100ms) for the next keyframe to become
  /// available. Uses thisIdx and thisPacket to determine track and current timestamp
  /// respectively.
  bool OutCMAF::waitForNextKey(uint64_t maxWait){
    uint64_t mTrk = getMainSelectedTrack();
    size_t currentKey = M.getKeyIndexForTime(mTrk, thisTime);
    uint64_t startTime = Util::bootMS();
    DTSC::Keys keys(M.getKeys(mTrk));
    while (startTime + maxWait > Util::bootMS() && keepGoing()){
      if (keys.getEndValid() > currentKey + 1 &&
          M.getLastms(thisIdx) >= M.getTimeForKeyIndex(mTrk, currentKey + 1)){
        return true;
      }
      Util::sleep(20);
      meta.reloadReplacedPagesIfNeeded();
    }
    INFO_MSG("Timed out waiting for next key (track %" PRIu64
             ", %zu+1, last is %zu, time is %" PRIu64 ")",
             mTrk, currentKey, keys.getEndValid() - 1,
             M.getTimeForKeyIndex(getMainSelectedTrack(), currentKey + 1));
    return (keys.getEndValid() > currentKey + 1 &&
            M.getLastms(thisIdx) >= M.getTimeForKeyIndex(mTrk, currentKey + 1));
  }

  // Set up an empty connection to the target to make sure we can push data towards it.
  void OutCMAF::startPushOut(){
    myConn.close();
    myConn.Received().clear();
    myConn.open(pushUrl.host, pushUrl.getPort(), true);
    wantRequest = false;
    parseData = true;
  }

  // CMAF Push output uses keyframe boundaries instead of fragment boundaries, to allow for lower
  // latency
  void OutCMAF::pushNext(){
    size_t mTrk = getMainSelectedTrack();
    // Set up a new connection if this is a new track, or if we have been disconnected.
    if (!pushTracks.count(thisIdx) || !pushTracks.at(thisIdx).D.getSocket()){
      if (pushTracks.count(thisIdx)){
        INFO_MSG("Reconnecting existing track: socket was disconnected");
      }
      CMAFPushTrack &track = pushTracks[thisIdx];
      size_t keyIndex = M.getKeyIndexForTime(mTrk, thisPacket.getTime());
      track.headerFrom = M.getTimeForKeyIndex(mTrk, keyIndex);
      if (track.headerFrom < thisPacket.getTime()){
        track.headerFrom = M.getTimeForKeyIndex(mTrk, keyIndex + 1);
      }

      INFO_MSG("Starting track %zu at %" PRIu64 "ms into the stream, current packet at %" PRIu64
               "ms",
               thisIdx, track.headerFrom, thisPacket.getTime());

      setupTrackObject(thisIdx);
      track.headerUntil = 0;
    }
    CMAFPushTrack &track = pushTracks[thisIdx];
    if (thisPacket.getTime() < track.headerFrom){return;}
    if (thisPacket.getTime() >= track.headerUntil){
      size_t keyIndex = M.getKeyIndexForTime(mTrk, thisTime);
      uint64_t keyTime = M.getTimeForKeyIndex(mTrk, keyIndex);
      if (keyTime > thisTime){
        realTime = 1000;
        if (!liveSeek()){
          WARN_MSG("Corruption probably occurred, initiating reconnect. Key %zu is time %" PRIu64
                   ", but packet is time %" PRIu64,
                   keyIndex, keyTime, thisTime);
          onTrackEnd(thisIdx);
          track.headerFrom = M.getTimeForKeyIndex(mTrk, keyIndex + 1);
          track.headerUntil = 0;
          pushNext();
        }
        realTime = 0;
        return;
      }
      track.headerFrom = keyTime;
      if (!waitForNextKey()){
        onTrackEnd(thisIdx);
        dropTrack(thisIdx, "No next keyframe available");
        return;
      }
      track.headerUntil = M.getTimeForKeyIndex(mTrk, keyIndex + 1);
      std::string keyHeader = CMAF::keyHeader(M, thisIdx, track.headerFrom, track.headerUntil,
                                              keyIndex + 1, true, true);
      uint64_t mdatSize = 8 + CMAF::payloadSize(M, thisIdx, track.headerFrom, track.headerUntil);
      char mdatHeader[] ={0x00, 0x00, 0x00, 0x00, 'm', 'd', 'a', 't'};
      Bit::htobl(mdatHeader, mdatSize);

      track.send(keyHeader);
      track.send(mdatHeader, 8);
    }
    char *data;
    size_t dataLen;
    thisPacket.getString("data", data, dataLen);

    track.send(data, dataLen);
  }

}// namespace Mist

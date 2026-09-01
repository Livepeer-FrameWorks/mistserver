#include "output_cmaf.h"

#include <mist/bitfields.h>
#include <mist/checksum.h>
#include <mist/cmaf.h>
// #include <mist/defines.h>
// #include <mist/encode.h>
#include <mist/hls_support.h>
#include <iomanip>
#include <limits>
#include <sstream>
#include <vector>
// #include <mist/mp4.h>
// #include <mist/mp4_dash.h>
// #include <mist/mp4_encryption.h>
// #include <mist/mp4_generic.h>
// #include <mist/timing.h>

const std::string hlsMediaFormat = ".m4s";

namespace{
  const uint32_t defaultPartTargetMs = 500;
  const uint32_t minPartTargetMs = 100;
  const uint32_t maxPartTargetMs = 60000;
}

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
    uaDelay = 0;
    realTime = 0;
    cmafLLStream = false;
    cmafLLRequestTrack = INVALID_TRACK_ID;
    cmafSegmentEnd = 0;
    cmafMuxedStream = false;
    muxedSample = 0;
    muxedByDefault = config->getString("packaging") == "muxed";
    const int64_t configuredPartTarget = config->getInteger("partduration");
    partTargetMs = configuredPartTarget >= minPartTargetMs && configuredPartTarget <= maxPartTargetMs
                       ? (uint32_t)configuredPartTarget
                       : defaultPartTargetMs;
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
    capa["codecs"][0u][6u].append("+opus");
    capa["codecs"][0u][7u].append("+AV1");
    capa["codecs"][0u][8u].append("+VP8");
    capa["codecs"][0u][9u].append("+VP9");
    capa["encryption"].append("CTR128");

    { // DASH playback method
      JSON::Value & M = capa["methods"].append();
      M["hrn"] = "DASH";
      M["handler"] = "http";
      M["type"] = "dash/video/mp4";
      M["ttff"] = "segs";
      M["ttff_segs"] = 3;
      M["latency"] = "frag*1.5";
      M["bw"].fromString("[0, 0, 12, false, 100]");
      M["control"] = 10;
      M["stability"] = 4;
      M["cpu_server"] = 4;
      M["permissibility"] = 10;
      M["url_rel"] = "/cmaf/$/index.mpd";
      M["abr"] = true;
    }
    { // HLS CMAF playback method
      JSON::Value & M = capa["methods"].append();
      M["hrn"] = "HLS (CMAF)";
      M["handler"] = "http";
      M["type"] = "html5/application/vnd.apple.mpegurl;version=7";
      M["ttff"] = "segs";
      M["ttff_segs"] = 3;
      M["latency"] = "frag*1.5";
      M["bw"].fromString("[0, 0, 12, false, 100]");
      M["control"] = 10;
      M["stability"] = 8;
      M["cpu_server"] = 4;
      M["permissibility"] = 10;
      M["url_rel"] = "/cmaf/$/index.m3u8";
      M["abr"] = true;
    }
    { // MSS playback method
      JSON::Value & M = capa["methods"].append();
      M["hrn"] = "MS Smooth Streaming";
      M["handler"] = "http";
      M["type"] = "html5/application/vnd.ms-sstr+xml";
      M["ttff"] = "segs";
      M["ttff_segs"] = 3;
      M["latency"] = "frag*1.5";
      M["bw"].fromString("[0, 0, 12, false, 100]");
      M["control"] = 1;
      M["stability"] = 3;
      M["cpu_server"] = 4;
      M["permissibility"] = 10;
      M["url_rel"] = "/cmaf/$/Manifest";
      M["abr"] = true;
    }

    // MP3 does not work in browsers
    capa["exceptions"]["codec:MP3"] = JSON::fromString("[[\"blacklist\",[\"Mozilla/\"]]]");

    cfg->addOption(
        "listlimit",
        JSON::fromString(
            "{\"arg\":\"integer\",\"default\":0,\"short\":\"y\",\"long\":\"list-limit\","
            "\"help\":\"Maximum number of segments in live playlists (0 = infinite).\"}"));
    capa["optional"]["listlimit"]["name"] = "Live playlist limit";
    capa["optional"]["listlimit"]["help"] =
        "Maximum number of complete segments in live playlists (minimum 6 when available; 0 = infinite).";
    capa["optional"]["listlimit"]["default"] = 0;
    capa["optional"]["listlimit"]["type"] = "uint";
    capa["optional"]["listlimit"]["option"] = "--list-limit";
    capa["optional"]["listlimit"]["display"] = "advanced";

    cfg->addOption(
        "partduration",
        JSON::fromString(
            "{\"arg\":\"integer\",\"default\":500,\"long\":\"part-duration\","
            "\"help\":\"Target duration in milliseconds for CMAF low-latency parts.\"}"));
    capa["optional"]["partduration"]["name"] = "Low-latency part target";
    capa["optional"]["partduration"]["help"] =
        "Lifetime-stable target duration in milliseconds for LL-HLS and LL-DASH parts "
        "(100-60000).";
    capa["optional"]["partduration"]["default"] = defaultPartTargetMs;
    capa["optional"]["partduration"]["type"] = "uint";
    capa["optional"]["partduration"]["option"] = "--part-duration";
    capa["optional"]["partduration"]["display"] = "advanced";

    cfg->addOption(
        "packaging",
        JSON::fromString(
            "{\"arg\":\"string\",\"default\":\"separate\",\"long\":\"packaging\","
            "\"help\":\"CMAF packaging mode: separate (default) or muxed.\"}"));
    capa["optional"]["packaging"]["name"] = "CMAF packaging mode";
    capa["optional"]["packaging"]["help"] =
        "Use separate single-track CMAF representations (default), or a supported multiplexed "
        "audio/video fragmented-MP4 representation. Muxed packaging may also be requested for "
        "one HLS or DASH manifest with ?packaging=muxed. Muxed DASH is not DASH-IF IOP compliant.";
    capa["optional"]["packaging"]["default"] = "separate";
    capa["optional"]["packaging"]["type"] = "select";
    capa["optional"]["packaging"]["option"] = "--packaging";
    capa["optional"]["packaging"]["select"][0u][0u] = "separate";
    capa["optional"]["packaging"]["select"][0u][1u] = "Separate tracks (CMAF/DASH-IF)";
    capa["optional"]["packaging"]["select"][1u][0u] = "muxed";
    capa["optional"]["packaging"]["select"][1u][1u] = "Muxed audio/video fMP4";
    capa["optional"]["packaging"]["display"] = "advanced";

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
    capa["optional"]["chunkedsegments"]["display"] = "advanced";

    cfg->addOption("mergesessions",
                   JSON::fromString("{\"short\":\"M\",\"long\":\"mergesessions\",\"help\":\"Merge "
                                    "together sessions from one user into a single session.\"}"));
    capa["optional"]["mergesessions"]["name"] = "Merge sessions";
    capa["optional"]["mergesessions"]["help"] =
        "If enabled, merges together all views from a single user into a single combined session. "
        "If disabled, each view (main playlist request) is a separate session.";
    capa["optional"]["mergesessions"]["option"] = "--mergesessions";
    capa["optional"]["mergesessions"]["display"] = "advanced";

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
    capa["optional"]["chunkpath"]["display"] = "advanced";

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

  static std::string muxedTrackName(const std::set<size_t> &tracks) {
    std::stringstream name;
    name << "muxed";
    for (std::set<size_t>::const_iterator track = tracks.begin(); track != tracks.end(); ++track) {
      name << "_" << *track;
    }
    return name.str();
  }

  static std::string hlsMuxedTrackPath(const DTSC::Meta &M, const std::set<size_t> &tracks) {
    if (tracks.size() == 1) { return muxedTrackName(tracks); }
    std::stringstream path;
    for (std::set<size_t>::const_iterator track = tracks.begin(); track != tracks.end(); ++track) {
      if (M.getType(*track) == "video") { path << "v" << *track; }
    }
    for (std::set<size_t>::const_iterator track = tracks.begin(); track != tracks.end(); ++track) {
      if (M.getType(*track) == "audio") {
        if (path.tellp()) { path << "/"; }
        path << "a" << *track;
      }
    }
    return path.str();
  }

  static bool parseTrackId(const std::string &token, size_t prefixLength, size_t &track) {
    if (token.size() <= prefixLength) { return false; }
    char *parsedEnd = 0;
    const unsigned long long value = strtoull(token.c_str() + prefixLength, &parsedEnd, 10);
    if (!parsedEnd || *parsedEnd || value > std::numeric_limits<size_t>::max()) { return false; }
    track = (size_t)value;
    return true;
  }

  static bool parseTypedTrackName(const std::string &path, size_t &track,
                                  size_t *mediaSlash = 0) {
    const size_t slash = path.find('/');
    const std::string name = path.substr(0, slash);
    if (name.size() < 2 || (name[0] != 'v' && name[0] != 'a')) { return false; }
    if (!parseTrackId(name, 1, track)) { return false; }
    if (mediaSlash) { *mediaSlash = slash; }
    return true;
  }

  static bool parseMuxedTrackName(const std::string &path, std::set<size_t> &tracks,
                                  size_t *mediaSlash = 0) {
    tracks.clear();
    const size_t slash = path.find('/');
    if (mediaSlash) { *mediaSlash = slash; }
    const std::string name = path.substr(0, slash);
    if (!name.compare(0, 6, "muxed_") && name.size() > 6) {
      size_t pos = 6;
      while (pos < name.size()) {
        size_t end = name.find('_', pos);
        if (end == std::string::npos) { end = name.size(); }
        size_t track;
        if (!parseTrackId(name.substr(pos, end - pos), 0, track)) { return false; }
        tracks.insert(track);
        pos = end + 1;
      }
      return !tracks.empty();
    }

    // Muxed HLS paths contain two type-explicit components: v<video>/a<audio>/...
    size_t track;
    if (!parseTypedTrackName(path, track) || slash == std::string::npos) { return false; }
    tracks.insert(track);
    const size_t secondEnd = path.find('/', slash + 1);
    const std::string second = path.substr(slash + 1, secondEnd - slash - 1);
    if (second.size() < 2 || (second[0] != 'v' && second[0] != 'a') ||
        !parseTrackId(second, 1, track)) { return false; }
    tracks.insert(track);
    if (mediaSlash) { *mediaSlash = secondEnd; }
    return true;
  }

  std::set<size_t> OutCMAF::defaultMuxedTracks() {
    selectDefaultTracks();
    std::set<size_t> result;
    size_t video = INVALID_TRACK_ID;
    size_t audio = INVALID_TRACK_ID;
    const size_t main = getMainSelectedTrack();
    if (M.getValidTracks().count(main)) {
      if (M.getType(main) == "video") { video = main; }
      if (M.getType(main) == "audio") { audio = main; }
    }
    for (std::map<size_t, Comms::Users>::const_iterator track = userSelect.begin();
         track != userSelect.end(); ++track) {
      if (video == INVALID_TRACK_ID && M.getType(track->first) == "video") { video = track->first; }
      if (audio == INVALID_TRACK_ID && M.getType(track->first) == "audio") { audio = track->first; }
    }
    if (video != INVALID_TRACK_ID) { result.insert(video); }
    if (audio != INVALID_TRACK_ID) { result.insert(audio); }
    return result;
  }

  bool OutCMAF::selectMuxedTracks(const std::set<size_t> &tracks) {
    size_t videoCount = 0;
    size_t audioCount = 0;
    for (std::set<size_t>::const_iterator track = tracks.begin(); track != tracks.end(); ++track) {
      if (!M.getValidTracks().count(*track)) { return false; }
      const std::string type = M.getType(*track);
      if (type == "video") { ++videoCount; }
      else if (type == "audio") { ++audioCount; }
      else { return false; }
    }
    // One multiplexed Representation contains at most one video and one audio component. In
    // particular, Apple forbids multiple audio streams in a single HLS media segment.
    if ((!videoCount && !audioCount) || videoCount > 1 || audioCount > 1) { return false; }
    userSelect.clear();
    for (std::set<size_t>::const_iterator track = tracks.begin(); track != tracks.end(); ++track) {
      userSelect[*track].reload(streamName, *track);
      if (!userSelect[*track]) {
        userSelect.clear();
        return false;
      }
    }
    trackSelectionChanged();
    return true;
  }

  bool OutCMAF::useMuxedPackaging(const HTTP::Parser &req) const {
    return muxedByDefault || req.GetVar("packaging") == "muxed";
  }

  /// \brief Builds master playlist for (LL)HLS.
  ///\return The master playlist file for (LL)HLS.
  void OutCMAF::sendHlsMasterManifest(const HTTP::Parser & req) {
    HLS::Generator generator;
    generator.setExt(hlsMediaFormat);
    if ((Comms::tknMode & 0x04) && tkn.size()){generator.setParam("tkn", tkn);}
    if (req.GetVar("llhls") == "0"){generator.setParam("llhls", "0");}

    if (useMuxedPackaging(req)) {
      const std::set<size_t> tracks = defaultMuxedTracks();
      if (!selectMuxedTracks(tracks)) {
        H.SendResponse("503", "No muxable tracks", myConn);
        return;
      }
      generator.setMuxed(true);
      generator.setMediaPath(hlsMuxedTrackPath(M, tracks));
    } else {
      selectDefaultTracks();
    }

    H.SetBody(generator.masterPlaylist(M, userSelect, getMainSelectedTrack()));
    H.SendResponse("200", "OK", myConn);
  }

  /// \brief Builds media playlist to (LL)HLS
  ///\return The media playlist file to (LL)HLS
  void OutCMAF::sendHlsMediaManifest(const HTTP::Parser & req, const size_t requestTid) {
    if (!M.getValidTracks().count(requestTid)) {
      H.SendResponse("404", "Track not found", myConn);
      return;
    }

    HLS::Generator generator;
    generator.setExt(hlsMediaFormat);
    generator.setPartTarget(partTargetMs);
    generator.setListLimit(config->getInteger("listlimit"));
    generator.setMuxed(cmafMuxedStream);
    if (config->getString("chunkpath").size()){
      generator.setUrlPrefix(
          HTTP::URL(config->getString("chunkpath")).link("./" + req.url).link("./").getUrl());
    }
    if ((Comms::tknMode & 0x04) && tkn.size()){generator.setParam("tkn", tkn);}
    generator.setParam("mTrack", req.GetVar("mTrack"));
    generator.setParam("iMsn", req.GetVar("iMsn"));
    generator.setParam("_HLS_skip", req.GetVar("_HLS_skip"));
    generator.setParam("_HLS_msn", req.GetVar("_HLS_msn"));
    generator.setParam("_HLS_part", req.GetVar("_HLS_part"));
    generator.setParam("llhls", req.GetVar("llhls"));
    // Legacy, non-standard override for reverse proxies that cannot modify the query string.
    // Any non-empty value overrides the llhls query parameter; only the exact value "0" disables LL-HLS.
    if (req.GetHeader("X-Mist-LLHLS").size()){
      generator.setParam("llhls", req.GetHeader("X-Mist-LLHLS"));
    }

    const HLS::Playlist playlist =
        generator.mediaPlaylist(M, userSelect, requestTid, getMainSelectedTrack());
    if (playlist.code == 400){
      H.SendResponse("400", "Bad Request: Invalid LLHLS parameter", myConn);
      return;
    }
    if (playlist.code == 503){
      H.SendResponse("503", "Service Unavailable", myConn);
      return;
    }
    if (playlist.code != 200){
      H.SendResponse(JSON::Value(playlist.code).asString(), "Playlist unavailable", myConn);
      return;
    }
    H.SetBody(playlist.data);
    H.SendResponse("200", "OK", myConn);
  } // namespace Mist

  void OutCMAF::sendHlsManifest(const HTTP::Parser & req, const std::string url, bool headersOnly) {
    H.SetHeader("Content-Type", "application/vnd.apple.mpegurl"); // for .m3u8
    H.SetHeader("Cache-Control", "no-store");
    if (headersOnly) {
      H.SetBody("");
      H.SendResponse("200", "OK", myConn);
      return;
    }

    if (url.find("/") == std::string::npos){
      cmafMuxedStream = false;
      sendHlsMasterManifest(req);
    }else{
      std::set<size_t> muxedTracks;
      if (parseMuxedTrackName(url, muxedTracks)) {
        if (!selectMuxedTracks(muxedTracks)) {
          H.SendResponse("404", "Muxed track selection not found", myConn);
          return;
        }
        cmafMuxedStream = true;
        sendHlsMediaManifest(req, getMainSelectedTrack());
      } else {
        cmafMuxedStream = false;
        size_t track;
        sendHlsMediaManifest(req, parseTypedTrackName(url, track) ? track : atoll(url.c_str()));
      }
    }
  }

  void OutCMAF::sendCmafError(const std::string & code, const std::string & message) {
    H.SetHeader("Content-Type", "text/plain; charset=utf-8");
    H.SetHeader("Cache-Control", "no-store");
    H.SetBody(message + "\n");
    H.SendResponse(code, message, myConn);
  }

  uint64_t OutCMAF::getPartTargetTime(size_t requestTrack, size_t timingTrack,
                                      uint64_t fragmentStart, uint64_t fragmentIndex,
                                      uint32_t part) const {
    DTSC::Fragments fragments(M.fragments(timingTrack));
    if (fragmentIndex < fragments.getFirstValid() || fragmentIndex >= fragments.getEndValid()) {
      return 0;
    }

    const uint64_t partStartOffset = uint64_t(part) * partTargetMs;
    const uint64_t partEndOffset = partStartOffset + partTargetMs;
    if (fragmentStart > UINT64_MAX - partEndOffset) { return 0; }

    const uint64_t fragmentDuration = fragments.getDuration(fragmentIndex);
    if (fragmentDuration) {
      if (partStartOffset >= fragmentDuration) { return 0; }
      return fragmentStart + std::min(fragmentDuration, partEndOffset);
    }

    const uint64_t partEnd = fragmentStart + partEndOffset;
    if (partEnd > UINT64_MAX - 50) { return 0; }
    const uint64_t readyAt = partEnd + 50;

    // A forming part is available once both the requested and timing tracks have reached it.
    // This is media-clock state from DTSC metadata; wall-clock jitter does not hide stored parts.
    uint64_t lastMs = std::min(M.getLastms(requestTrack), M.getLastms(timingTrack));
    uint16_t attempts = 0;
    while (readyAt > lastMs && attempts++ < 50) {
      Util::wait(readyAt - lastMs);
      lastMs = std::min(M.getLastms(requestTrack), M.getLastms(timingTrack));
    }
    return partEnd;
  }

  bool OutCMAF::onFinish() {
    if (wantRequest) { return true; }

    H.Chunkify("", 0, myConn);
    H.Clean();

    wantRequest = true;
    parseData = false;
    cmafLLStream = false;
    cmafMuxedStream = false;
    cmafSegmentEnd = 0;
    cmafLLPartLeft = 0;
    muxedSamples.clear();
    muxedSample = 0;
    return true;
  }

  void OutCMAF::respondHTTP(const HTTP::Parser & req, bool headersOnly) {
    HTTPOutput::respondHTTP(req, headersOnly);
    initialize();
    if (req.url.find('/', 6) == std::string::npos) {
      H.SendResponse("404", "Stream not found", myConn);
      return;
    }

    // Strip /cmaf/<streamname>/ from url
    std::string url = req.url.substr(req.url.find('/', 6) + 1);
    HTTP::URL rUrl(reqUrl);

    // Send a dash manifest for any URL with .mpd in the path
    if (rUrl.getExt() == "mpd") {
      sendDashManifest(req, headersOnly);
      return;
    }

    // DASH-IF clock synchronization endpoint. The MPD references this relative
    // resource through the http-xsdate timing scheme.
    if (url == "time.txt") {
      H.SetHeader("Content-Type", "text/plain; charset=utf-8");
      H.SetHeader("Cache-Control", "no-store");
      H.SetBody(headersOnly ? "" : Util::getUTCStringMillis(Util::unixMS()) + "\n");
      H.SendResponse("200", "OK", myConn);
      return;
    }

    // Send a hls manifest for any URL with index.m3u8 in the path
    if (rUrl.getExt() == "m3u8") {
      sendHlsManifest(req, url, headersOnly);
      return;
    }

    // Send a smooth manifest for any URL with .mpd in the path
    if (url.find("Manifest") != std::string::npos){
      sendSmoothManifest(headersOnly);
      return;
    }

    const uint64_t msn = atoll(req.GetVar("msn").c_str());
    const uint64_t dur = atoll(req.GetVar("dur").c_str());
    const std::string mTrackParam = req.GetVar("mTrack");
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
    if (headersOnly) {
      H.SendResponse("200", "OK", myConn);
      return;
    }

    std::set<size_t> requestedMuxedTracks;
    size_t mediaSlash = url.find('/');
    const bool requestedMuxed = parseMuxedTrackName(url, requestedMuxedTracks, &mediaSlash);
    // Both muxed_<tracks> and v<video>/a<audio> paths explicitly request muxed packaging.
    // Accept them independently of the configured default so child URLs of a muxed manifest do
    // not need to repeat the packaging query parameter.
    size_t idx = INVALID_TRACK_ID;
    if (requestedMuxed) {
      if (!selectMuxedTracks(requestedMuxedTracks)) {
        sendCmafError("404", "Muxed track selection not found");
        return;
      }
      cmafMuxedStream = true;
      idx = getMainSelectedTrack();
    } else {
      cmafMuxedStream = false;
      if (!parseTypedTrackName(url, idx, &mediaSlash)) { idx = atoll(url.c_str()); }
    }
    std::string mediaObject = mediaSlash == std::string::npos ? url : url.substr(mediaSlash + 1);
    if (!requestedMuxed && url.find("Q(") != std::string::npos){
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

    if (mediaObject.find(hlsMediaFormat) == std::string::npos){
      sendCmafError("404", "File not found");
      return;
    }

    if (mediaObject.find("init" + hlsMediaFormat) != std::string::npos){
      std::string headerData;
      if (requestedMuxed) {
        Util::ResizeablePointer init;
        if (!CMAF::header(init, M, userSelect)) {
          sendCmafError("404", "Muxed initialization is unavailable");
          return;
        }
        headerData.assign((char *)init, init.size());
      } else {
        headerData = CMAF::trackHeader(M, idx);
      }
      H.StartResponse("200", "OK", req, myConn, !config->getBool("chunkedsegments"));
      H.Chunkify(headerData.c_str(), headerData.size(), myConn);
      H.Chunkify("", 0, myConn);
      return;
    }

    // Select the right track
    if (!requestedMuxed) {
      userSelect.clear();
      userSelect[idx].reload(streamName, idx);
    }

    uint64_t fragmentIndex;
    uint64_t startTime;
    uint64_t targetTime;
    uint32_t part;

    // set targetTime
    if (sscanf(mediaObject.c_str(), "chunk_%" PRIu64 ".%" PRIu32 ".*", &startTime, &part) == 2){
      // Logic: calculate targetTime for partial segments
      targetTime = getPartTargetTime(idx, mTrack, startTime, msn, part);
      if (!targetTime){
        sendCmafError("404", "Partial fragment does not exist");
        return;
      }
      const uint64_t partOffset = uint64_t(part) * uint64_t(partTargetMs);
      if (partOffset > UINT64_MAX - startTime) {
        sendCmafError("404", "Partial fragment does not exist");
        return;
      }
      startTime += partOffset;
      fragmentIndex = M.getFragmentIndexForTime(mTrack, startTime);
      DEBUG_MSG(5, "partial segment requested: %s st %" PRIu64 " et %" PRIu64, url.c_str(),
                startTime, targetTime);
    }else if (sscanf(mediaObject.c_str(), "chunk_%" PRIu64 ".*", &startTime) == 1){
      // Logic: calculate targetTime for full segments
      if (M.getVod()){startTime += M.getFirstms(idx);}
      DTSC::Fragments fragments(M.fragments(mTrack));
      fragmentIndex = M.getFragmentIndexForTime(mTrack, startTime);
      // A request for the still-forming fragment (duration not yet known) is served
      // over chunked transfer: the fragment is streamed as a sequence of per-part
      // [moof][mdat] CMAF chunks into one open response (see sendNextLL). Only the
      // low-latency DASH manifest advertises such requests, and HLS never asks for
      // an incomplete fragment as a full segment, so complete-segment and HLS serving
      // are left untouched.
      if (!dur && M.getLive()) {
        const uint64_t waitUntil = Util::bootMS() + (uint64_t(partTargetMs) * 4);
        while (true) {
          const bool exactFragment = fragmentIndex >= fragments.getFirstValid() &&
            fragmentIndex < fragments.getEndValid() && M.getTimeForFragmentIndex(mTrack, fragmentIndex) == startTime;
          if (exactFragment && !fragments.getDuration(fragmentIndex)) {
            const uint64_t firstPartEnd = getPartTargetTime(idx, mTrack, startTime, fragmentIndex, 0);
            if (firstPartEnd && CMAF::payloadSize(M, idx, startTime, firstPartEnd)) {
              const uint64_t segmentDuration = M.biggestFragment(mTrack);
              if (!segmentDuration) { break; }
              DEBUG_MSG(5, "Low-latency DASH chunked transfer for forming fragment %s track=%zu start=%" PRIu64,
                        url.c_str(), idx, startTime);
              cmafLLStream = true;
              cmafLLFragStart = startTime;
              cmafLLFragEnd = startTime + segmentDuration;
              cmafLLMsn = fragmentIndex;
              cmafLLmTrack = mTrack;
              cmafLLRequestTrack = idx;
              cmafLLPartEnd = 0;
              cmafLLPartLeft = 0;
              cmafLLSeq = fragmentIndex;
              // Always chunked, independent of --chunked-segments: the final
              // Content-Length is unknowable while the segment is still being produced.
              H.StartResponse("200", "OK", req, myConn, false);
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
    const uint64_t payloadSizeForTrack = CMAF::payloadSize(M, idx, startTime, targetTime);
    if (!payloadSizeForTrack) {
      sendCmafError("404", "Segment has no media data");
      return;
    }
    const uint64_t mediaStart = M.getPartTime(M.getPartIndex(startTime, idx), idx);
    // Per-track objects snap to that track's first packet. A muxed object keeps the requested
    // shared interval so an audio packet preceding the primary track's first packet is not lost.
    startTime = requestedMuxed ? requestedStartTime : mediaStart;
    uint64_t payloadSize = payloadSizeForTrack;
    std::string headerData;
    if (requestedMuxed) {
      CMAF::MuxedFragment fragment;
      if (!CMAF::muxedFragment(fragment, M, userSelect, startTime, targetTime, fragmentIndex)) {
        sendCmafError("404", "Muxed segment is unavailable");
        return;
      }
      headerData.swap(fragment.header);
      payloadSize = fragment.payloadSize;
      muxedSamples.swap(fragment.samples);
      muxedSample = 0;
    } else {
      headerData = CMAF::keyHeader(M, idx, startTime, targetTime, fragmentIndex, false, false);
      muxedSamples.clear();
      muxedSample = 0;
    }

    DEBUG_MSG(5, "CMAF segment payload track=%zu start=%" PRIu64 " target=%" PRIu64 " header=%zu payload=%" PRIu64, idx,
              startTime, targetTime, headerData.size(), payloadSize);

    H.StartResponse("200", "OK", req, myConn, !config->getBool("chunkedsegments"));
    H.Chunkify(headerData.c_str(), headerData.size(), myConn);
    if (!requestedMuxed) {
      const uint64_t mdatSize = 8 + payloadSize;
      char mdatHeader[] ={0x00, 0x00, 0x00, 0x00, 'm', 'd', 'a', 't'};
      Bit::htobl(mdatHeader, mdatSize);
      H.Chunkify(mdatHeader, 8, myConn);
    }

    cmafSegmentEnd = targetTime;
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
      onFinish();
      return;
    }
    // Index of the part-grid cell this packet falls in, relative to the fragment.
    const uint32_t chunkIdx = (uint32_t)((t - cmafLLFragStart) / partTargetMs);
    // Open a new chunk only when this packet belongs to a part we have not opened yet.
    // Skipped (empty) cells get no chunk; a packet always lands in the chunk that
    // covers its own timestamp.
    if (!cmafLLPartEnd || t >= cmafLLPartEnd) {
      const uint64_t pStart = cmafLLFragStart + (uint64_t)chunkIdx * partTargetMs;
      // Blocks until the part is complete; returns 0 / a clamped end at fragment end.
      const uint64_t pEnd =
          getPartTargetTime(cmafLLRequestTrack, cmafLLmTrack, cmafLLFragStart, cmafLLMsn, chunkIdx);
      const uint64_t payloadSize = pEnd > pStart
                                       ? CMAF::payloadSize(M, cmafLLRequestTrack, pStart, pEnd)
                                       : 0;
      if (!payloadSize) {
        DEBUG_MSG(5,
                  "Low-latency DASH closing fragment on unavailable part track=%zu start=%" PRIu64 " end=%" PRIu64
                  " part=%u partStart=%" PRIu64 " partEnd=%" PRIu64,
                  thisIdx, cmafLLFragStart, cmafLLFragEnd, chunkIdx, pStart, pEnd);
        onFinish();
        return;
      }
      if (cmafMuxedStream) {
        CMAF::MuxedFragment fragment;
        if (!CMAF::muxedFragment(fragment, M, userSelect, pStart, pEnd, cmafLLSeq++)) {
          onFinish();
          return;
        }
        H.Chunkify(fragment.header.c_str(), fragment.header.size(), myConn);
        cmafLLPartLeft = fragment.payloadSize;
        muxedSamples.swap(fragment.samples);
        muxedSample = 0;
      } else {
        const uint64_t mediaStart =
            M.getPartTime(M.getPartIndex(pStart, cmafLLRequestTrack), cmafLLRequestTrack);
        std::string moof = CMAF::keyHeader(M, cmafLLRequestTrack, mediaStart, pEnd,
                                           cmafLLSeq++, false, false);
        uint64_t mdatSize = 8 + payloadSize;
        char mdatHeader[] = {0x00, 0x00, 0x00, 0x00, 'm', 'd', 'a', 't'};
        Bit::htobl(mdatHeader, mdatSize);
        H.Chunkify(moof.c_str(), moof.size(), myConn);
        H.Chunkify(mdatHeader, 8, myConn);
        cmafLLPartLeft = payloadSize;
      }
      cmafLLPartEnd = pEnd;
    }
    char *data;
    size_t dataLen;
    thisPacket.getString("data", data, dataLen);
    if (cmafMuxedStream) {
      if (muxedSample >= muxedSamples.size()) { return; }
      const CMAF::MuxedSample &expected = muxedSamples[muxedSample];
      if (thisIdx != expected.track || t != expected.time || dataLen != expected.size) {
        FAIL_MSG("Muxed LL-DASH packet order mismatch: expected track=%zu time=%" PRIu64
                 " size=%u, got track=%zu time=%" PRIu64 " size=%zu",
                 expected.track, expected.time, expected.size, thisIdx, t, dataLen);
        onFinish();
        return;
      }
      ++muxedSample;
    }
    H.Chunkify(data, dataLen, myConn);
    if (dataLen >= cmafLLPartLeft) {
      cmafLLPartLeft = 0;
    } else {
      cmafLLPartLeft -= dataLen;
    }
    if (!cmafLLPartLeft && cmafLLPartEnd >= cmafLLFragEnd) {
      DEBUG_MSG(5, "Low-latency DASH completed fragment track=%zu start=%" PRIu64 " end=%" PRIu64, thisIdx,
                cmafLLFragStart, cmafLLFragEnd);
      onFinish();
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
    if (cmafMuxedStream) {
      if (muxedSample >= muxedSamples.size()) {
        onFinish();
        return;
      }
      char *data;
      size_t dataLen;
      thisPacket.getString("data", data, dataLen);
      const CMAF::MuxedSample &expected = muxedSamples[muxedSample];
      if (thisIdx != expected.track || thisPacket.getTime() != expected.time ||
          dataLen != expected.size) {
        FAIL_MSG("Muxed CMAF packet order mismatch: expected track=%zu time=%" PRIu64
                 " size=%u, got track=%zu time=%" PRIu64 " size=%zu",
                 expected.track, expected.time, expected.size, thisIdx, thisPacket.getTime(),
                 dataLen);
        onFinish();
        return;
      }
      H.Chunkify(data, dataLen, myConn);
      ++muxedSample;
      if (muxedSample == muxedSamples.size()) { onFinish(); }
      return;
    }
    if (cmafSegmentEnd && thisPacket.getTime() >= cmafSegmentEnd) {
      onFinish();
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
    DTSC::Keys keys(M.getKeys(timingTrack));
    // Fragments may outlive their packet metadata. Start at the first fragment whose payload is
    // still retained; jitter and keep-away are playback hints and do not affect this window.
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
      if (!duration){
        if (M.getVod()) {
          duration = M.getLastms(idx) - starttime;
        } else if (includeForming) {
          // Still-forming fragment (LL-DASH only): its real duration isn't known yet, so use the
          // largest fragment as a nominal <S d> and let the next publishTime refresh reconcile it.
          // The caller only sets includeForming when the GOP is fixed (dashFixedGop), so this
          // nominal matches the actual duration; on variable GOP the forming segment is not listed.
          duration = M.biggestFragment(timingTrack);
          if (!duration) { continue; }
        } else {
          continue; // skip last fragment when live
        }
      }
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

  /*********************************/
  /* MPEG-DASH Manifest Generation */
  /*********************************/

  void OutCMAF::sendDashManifest(const HTTP::Parser &req, bool headersOnly) {
    H.SetHeader("Content-Type", "application/dash+xml");
    H.SetHeader("Cache-Control", "no-store");
    // The HTTP Date header backs up in-MPD UTCTiming for client clock sync
    // and keeps MPD responses aligned with normal HTTP cache semantics.
    {
      time_t nowSec = (time_t)Util::epoch();
      struct tm *gmt = gmtime(&nowSec);
      char dateBuf[40];
      if (gmt && strftime(dateBuf, sizeof(dateBuf), "%a, %d %b %Y %H:%M:%S GMT", gmt)) { H.SetHeader("Date", dateBuf); }
    }
    if (headersOnly) {
      H.SendResponse("200", "OK", myConn);
      H.Clean();
      return;
    }
    const std::string manifest = dashManifest(true, useMuxedPackaging(req));
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

  /// Maximum proven interval between representation resynchronization points.
  /// Video fragment boundaries are keyframe/SAP boundaries; audio samples are
  /// independently decodable and each emitted CMAF chunk is a resync point.
  static uint64_t dashResyncIntervalMs(const DTSC::Meta &M, size_t track,
                                       uint32_t partGridMs) {
    if (!M.getValidTracks().count(track)) { return 0; }
    if (M.getType(track) == "video") {
      DTSC::Fragments fragments(M.fragments(track));
      uint64_t maximum = 0;
      uint32_t checked = 0;
      for (uint32_t i = fragments.getEndValid();
           i > fragments.getFirstValid() && checked < 6; --i) {
        const uint64_t duration = fragments.getDuration(i - 1);
        if (!duration) { continue; }
        maximum = std::max(maximum, duration);
        ++checked;
      }
      return checked >= 3 ? maximum : 0;
    }

    DTSC::Parts parts(M.parts(track));
    uint64_t maxSampleDuration = 0;
    for (size_t i = parts.getFirstValid(); i < parts.getEndValid(); ++i) {
      maxSampleDuration = std::max<uint64_t>(maxSampleDuration, parts.getDuration(i));
    }
    return maxSampleDuration ? uint64_t(partGridMs) + maxSampleDuration : 0;
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

  void OutCMAF::dashRepresentation(size_t id, size_t idx, std::stringstream &r,
                                   bool strictLowLatency, uint64_t availabilityStartMs){
    std::string codec = M.getCodec(idx);
    std::string type = M.getType(idx);
    r << "<Representation id=\"" << idx << "\" bandwidth=\"" << M.getBps(idx) * 8 << "\" codecs=\"";
    r << Util::codecString(M.getCodec(idx), M.getInit(idx));
    r << "\" ";
    if (type == "audio"){
      r << "audioSamplingRate=\"" << M.getRate(idx) << "\"";
    }
    if (!strictLowLatency) {
      if (type == "audio") {
        r << "> <AudioChannelConfiguration "
             "schemeIdUri=\"urn:mpeg:dash:23003:3:audio_channel_configuration:2011\" value=\""
          << M.getChannels(idx) << "\" /></Representation>" << std::endl;
      } else {
        r << "/>";
      }
      return;
    }

    r << ">" << std::endl;
    // RepresentationBaseType requires AudioChannelConfiguration before
    // ProducerReferenceTime and Resync.
    if (type == "audio") {
      r << "<AudioChannelConfiguration "
           "schemeIdUri=\"urn:mpeg:dash:23003:3:audio_channel_configuration:2011\" value=\""
        << M.getChannels(idx) << "\" />" << std::endl;
    }
    r << "<ProducerReferenceTime id=\"0\" inband=\"false\" type=\"encoder\" wallClockTime=\""
      << Util::getUTCStringMillis(availabilityStartMs)
      << "\" presentationTime=\"0\"><UTCTiming "
         "schemeIdUri=\"urn:mpeg:dash:utc:http-xsdate:2014\" value=\"time.txt\" />"
         "</ProducerReferenceTime>" << std::endl;
    const uint64_t resyncInterval = dashResyncIntervalMs(M, idx, partTargetMs);
    if (resyncInterval) {
      r << "<Resync type=\"1\" dT=\"" << resyncInterval << "\" marker=\"true\" />" << std::endl;
    }
    r << "</Representation>" << std::endl;
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

  void OutCMAF::dashAdaptation(size_t id, std::set<size_t> tracks, bool aligned, std::stringstream & r, uint64_t minStartTime,
                               uint64_t maxEndTime, bool includeForming, size_t timingTrack, double availabilityTimeOffset,
                               bool strictLowLatency, uint64_t availabilityStartMs) {
    if (!tracks.size()){return;}
    if (aligned){
      size_t firstTrack = *tracks.begin();
      dashAdaptationSet(id, *tracks.begin(), r);
      const size_t trackTiming = timingTrack == INVALID_TRACK_ID ? firstTrack : timingTrack;
      dashSegmentTemplate(r, availabilityTimeOffset, trackTiming, firstTrack);
      generateSegmentlist(firstTrack, r, dashSegment, minStartTime, maxEndTime, includeForming, trackTiming);
      r << "</SegmentTimeline></SegmentTemplate>" << std::endl;
      for (std::set<size_t>::iterator it = tracks.begin(); it != tracks.end(); it++){
        dashRepresentation(id, *it, r, strictLowLatency, availabilityStartMs);
      }
      r << "</AdaptationSet>" << std::endl;
      return;
    }
    for (std::set<size_t>::iterator it = tracks.begin(); it != tracks.end(); it++){
      std::string codec = M.getCodec(*it);
      std::string type = M.getType(*it);
      const size_t trackTiming = timingTrack == INVALID_TRACK_ID ? *it : timingTrack;
      dashAdaptationSet(id, *it, r);
      dashSegmentTemplate(r, availabilityTimeOffset, trackTiming, *it);
      generateSegmentlist(*it, r, dashSegment, minStartTime, maxEndTime, includeForming, trackTiming);
      r << "</SegmentTimeline></SegmentTemplate>" << std::endl;
      dashRepresentation(id, *it, r, strictLowLatency, availabilityStartMs);
      r << "</AdaptationSet>" << std::endl;
    }
  }

  void OutCMAF::dashMuxedAdaptation(const std::set<size_t> &videoTracks,
                                    const std::set<size_t> &audioTracks, std::stringstream &r,
                                    uint64_t minStartTime, uint64_t maxEndTime, bool includeForming,
                                    double availabilityTimeOffset) {
    std::set<size_t> tracks = videoTracks;
    tracks.insert(audioTracks.begin(), audioTracks.end());
    if (tracks.empty()) { return; }
    const size_t timingTrack = videoTracks.empty() ? *audioTracks.begin() : *videoTracks.begin();
    const std::string representation = muxedTrackName(tracks);
    r << "<AdaptationSet id=\"1\" mimeType=\""
      << (videoTracks.empty() ? "audio/mp4" : "video/mp4")
      << "\" segmentAlignment=\"true\" startWithSAP=\"1\">" << std::endl;
    if (!videoTracks.empty()) { r << "<ContentComponent id=\"1\" contentType=\"video\" />" << std::endl; }
    if (!audioTracks.empty()) { r << "<ContentComponent id=\"2\" contentType=\"audio\" />" << std::endl; }
    dashSegmentTemplate(r, availabilityTimeOffset, timingTrack, timingTrack);
    generateSegmentlist(timingTrack, r, dashSegment, minStartTime, maxEndTime, includeForming,
                        timingTrack);
    r << "</SegmentTimeline></SegmentTemplate>" << std::endl;

    uint64_t bandwidth = 0;
    std::string codecs;
    for (std::set<size_t>::const_iterator track = tracks.begin(); track != tracks.end(); ++track) {
      bandwidth += M.getBps(*track) * 8;
      if (codecs.size()) { codecs += ","; }
      codecs += Util::codecString(M.getCodec(*track), M.getInit(*track));
    }
    r << "<Representation id=\"" << representation << "\" bandwidth=\"" << bandwidth
      << "\" codecs=\"" << codecs << "\"";
    if (!videoTracks.empty()) {
      const size_t video = *videoTracks.begin();
      r << " width=\"" << M.getWidth(video) << "\" height=\"" << M.getHeight(video) << "\"";
      if (M.getFpks(video)) { r << " frameRate=\"" << M.getFpks(video) << "/1000\""; }
    }
    if (audioTracks.empty()) {
      r << " />" << std::endl;
    } else {
      const size_t audio = *audioTracks.begin();
      r << " audioSamplingRate=\"" << M.getRate(audio) << "\">"
        << "<AudioChannelConfiguration "
           "schemeIdUri=\"urn:mpeg:dash:23003:3:audio_channel_configuration:2011\" value=\""
        << M.getChannels(audio) << "\" /></Representation>" << std::endl;
    }
    r << "</AdaptationSet>" << std::endl;
  }

  /// Returns a string with the full XML DASH manifest MPD file.
  std::string OutCMAF::dashManifest(bool checkAlignment, bool muxedPackaging){
    initialize();
    selectDefaultTracks();
    if (muxedPackaging) {
      const std::set<size_t> tracks = defaultMuxedTracks();
      if (!selectMuxedTracks(tracks)) { return ""; }
    }
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
    bool dashStrictLowLatency = false; ///< DASH-IF LL profile requirements are proven
    bool dashForming = false; ///< advertise the still-forming segment (LL + fixed GOP only)
    double dashAtoSecs = 0.0; ///< honest availabilityTimeOffset in seconds (0 = not advertised)
    uint64_t availabilityStartMs = 0; ///< wall-clock anchor for presentation time zero
    if (M.getVod()){
      r << "type=\"static\" mediaPresentationDuration=\"" << dashTime(mainDuration)
        << "\" minBufferTime=\"PT1.5S\" ";
    }else{
      // Low-latency DASH is DASH-specific: it advertises and serves the still-forming
      // segment over chunked transfer. It is independent of --chunked-segments,
      // which only governs the framing of completed CMAF objects.
      // Only advertise LL-DASH when the GOP is fixed, so the still-forming segment's nominal <S d>
      // matches what the endpoint will stream. Variable GOP falls back to standard DASH signalling.
      // Every DASH segment is cut on its timing track's fragment grid (video when present; audio
      // only falls back to its own grid without video), so gate on those grids only: audio tracks'
      // own fragment boundaries are arbitrary and would wrongly disable LL.
      const size_t dashTimingTrack = vTracks.size() ? *vTracks.begin() : INVALID_TRACK_ID;
      std::set<size_t> dashTimingTracks;
      if (dashTimingTrack != INVALID_TRACK_ID) {
        dashTimingTracks.insert(dashTimingTrack);
      } else {
        dashTimingTracks = aTracks;
      }
      dashForming = dashFixedGop(M, dashTimingTracks, std::set<size_t>());
      dashLowLatency = dashForming;
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
      const uint64_t streamStartMs = M.packetTimeToUnixMs(0);
      // Keep availabilityStartTime in milliseconds; whole-second truncation shifts
      // segment-availability calculations by up to one second at the live edge.
      availabilityStartMs = streamStartMs ? streamStartMs : (Util::epoch() * 1000 - dashWindowEnd);
      const uint64_t targetDurationMs = selectedMaxFragmentDurationMs(M, dashVTracks, dashATracks);
      const uint64_t keepAwayMs = getMinKeepAway();
      if (dashForming) {
        // The first part is available one part duration after the segment starts. The MPD therefore
        // advertises the remainder of the segment as availabilityTimeOffset.
        uint64_t segDurMs = 0;
        for (std::set<size_t>::iterator it = dashTimingTracks.begin(); it != dashTimingTracks.end(); ++it) {
          const uint64_t frag = M.biggestFragment(*it);
          if (frag && (!segDurMs || frag < segDurMs)) { segDurMs = frag; }
        }
        const uint64_t atoMs = segDurMs > partTargetMs ? segDurMs - partTargetMs : 0;
        if (atoMs) {
          dashAtoSecs = atoMs / 1000.0;
        } else {
          dashForming = false;
          dashLowLatency = false;
        }
      }
      // Muxed CMAF remains a functional legacy LL-DASH mode, but it does not claim
      // DASH-IF CMAF profile conformance. For separate representations, prove a
      // resynchronization interval for every selected track before using the profile.
      dashStrictLowLatency = dashLowLatency && !muxedPackaging &&
                             dashFixedGop(M, dashVTracks, std::set<size_t>()) &&
                             tracksAligned(dashVTracks);
      uint64_t maxAnyResyncMs = 0;
      if (dashStrictLowLatency) {
        for (std::set<size_t>::const_iterator it = dashVTracks.begin(); it != dashVTracks.end(); ++it) {
          const uint64_t interval = dashResyncIntervalMs(M, *it, partTargetMs);
          if (!interval) { dashStrictLowLatency = false; break; }
          maxAnyResyncMs = std::max(maxAnyResyncMs, interval);
        }
        for (std::set<size_t>::const_iterator it = dashATracks.begin();
             dashStrictLowLatency && it != dashATracks.end(); ++it) {
          const uint64_t interval = dashResyncIntervalMs(M, *it, partTargetMs);
          if (!interval) { dashStrictLowLatency = false; break; }
          maxAnyResyncMs = std::max(maxAnyResyncMs, interval);
        }
      }
      uint64_t suggestedPresentationDelay;
      uint64_t minimumUpdatePeriodMs;
      uint64_t minBufferMs;
      if (dashLowLatency) {
        // LL-DASH: the forming segment is delivered over chunked transfer (parts as produced),
        // signalled with availabilityTimeOffset below.
        minBufferMs = uint64_t(partTargetMs) * 3;
        suggestedPresentationDelay = uint64_t(partTargetMs) * 4 + keepAwayMs;
        // The DASH-IF Resync alternative requires dT to be no greater than the target
        // latency. Keep the normal latency calculation whenever it already satisfies
        // that rule, otherwise move it only one millisecond beyond the proven SAP gap.
        if (dashStrictLowLatency && maxAnyResyncMs >= suggestedPresentationDelay) {
          suggestedPresentationDelay = maxAnyResyncMs + 1;
        }
        // DASH-IF defines its low-latency offering in the typical 2-10 second range.
        // Keep serving functional chunked DASH outside that range, but do not attach
        // the strict profile identifier to it.
        if (suggestedPresentationDelay > 10000) { dashStrictLowLatency = false; }
        // Refresh near the part cadence (not targetDur/2) so the timeline / forming-segment info
        // stays fresh for the player.
        minimumUpdatePeriodMs = std::max<uint64_t>(uint64_t(partTargetMs) * 2, 1000);
        dashLatencyTargetMs = suggestedPresentationDelay;
      } else {
        // Standard DASH lists complete segments only; keep two target durations of playback room.
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
    r << "profiles=\"urn:mpeg:dash:profile:isoff-live:2011";
    if (dashStrictLowLatency) {
      r << ",urn:mpeg:dash:profile:cmaf:2019"
           ",http://www.dashif.org/guidelines/low-latency-live-v5";
    }
    r << "\" "
         "xmlns=\"urn:mpeg:dash:schema:mpd:2011\" >"
      << std::endl;
    r << "<ProgramInformation><Title>" << streamName << "</Title></ProgramInformation>"
      << std::endl;
    // Low-latency DASH only: advertise a target latency backed by chunked transfer
    // of the forming segment. Standard DASH emits no ServiceDescription.
    if (dashLowLatency) {
      const uint64_t latencyMinMs = uint64_t(partTargetMs) * 3;
      const uint64_t latencyMaxMs = dashLatencyTargetMs * 2;
      r << "<ServiceDescription id=\"0\"><Latency target=\"" << dashLatencyTargetMs << "\" min=\"" << latencyMinMs
        << "\" max=\"" << latencyMaxMs << "\"";
      if (dashStrictLowLatency) { r << " referenceId=\"0\""; }
      r << " /><PlaybackRate min=\"0.96\" max=\"1.04\" /></ServiceDescription>" << std::endl;
    }
    r << "<Period " << (M.getLive() ? "start=\"PT0.0S\"" : "") << ">" << std::endl;

    if (muxedPackaging) {
      dashMuxedAdaptation(dashVTracks, dashATracks, r, dashWindowStart, dashWindowEnd,
                          dashForming, dashAtoSecs);
    } else {
      bool videoAligned = checkAlignment && tracksAligned(dashVTracks);
      bool audioAligned = checkAlignment && tracksAligned(dashATracks);
      const size_t dashTimingTrack = dashVTracks.size() ? *dashVTracks.begin() : INVALID_TRACK_ID;
      dashAdaptation(1, dashVTracks, videoAligned, r, dashWindowStart, dashWindowEnd, dashForming,
                     INVALID_TRACK_ID, dashAtoSecs, dashStrictLowLatency, availabilityStartMs);
      dashAdaptation(2, dashATracks, audioAligned, r, dashWindowStart, dashWindowEnd, dashForming,
                     dashTimingTrack, dashAtoSecs, dashStrictLowLatency, availabilityStartMs);
    }

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
    // Give the client a server clock to anchor segment-availability math to. Strict
    // DASH-IF LL uses a fetchable millisecond-precision source; legacy modes retain
    // the inline direct clock for compatibility.
    if (M.getLive()) {
      if (dashStrictLowLatency) {
        r << "<UTCTiming schemeIdUri=\"urn:mpeg:dash:utc:http-xsdate:2014\" value=\"time.txt\" />"
          << std::endl;
      } else {
        r << "<UTCTiming schemeIdUri=\"urn:mpeg:dash:utc:direct:2014\" value=\""
          << Util::getUTCStringMillis(Util::unixMS()) << "\" />" << std::endl;
      }
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

  void OutCMAF::sendSmoothManifest(bool headersOnly) {
    H.SetHeader("Content-Type", "application/dash+xml");
    H.SetHeader("Cache-Control", "no-store");
    if (headersOnly) {
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

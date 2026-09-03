#include "input_buffer.h"

#include "../process_supervisor.h"
#include "../processing_lifecycle.h"
#include "processing_rate.h"

#include <mist/bitfields.h>
#include <mist/defines.h>
#include <mist/langcodes.h>
#include <mist/procs.h>
#include <mist/stream.h>
#include <mist/triggers.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#ifndef TIMEOUTMULTIPLIER
#define TIMEOUTMULTIPLIER 2
#endif

/*LTS-START*/
// We consider a stream playable when this many fragments are available.
#define FRAG_BOOT 3
/*LTS-END*/

// For USR2 signal handling
Mist::InputBuffer *myBuf = 0;
void usr2sig_handler(int signum) {
  myBuf->onDebug();
}

namespace Mist{
  InputBuffer::InputBuffer(Util::Config *cfg) : Input(cfg){
    lastBPS = 0;
    firstProcTime = 0;
    lastProcTime = 0;
    allProcsRunning = false;
    processOverrideResolved = false;
    effectiveSpeed = 0;
    startupSeedApplied = false;
    lastRateUpdateMs = 0;
    rateJitterMs = (uint32_t)(((uint64_t)getpid() * 1103515245u + 12345u) % 251u);
    rampLockoutTicks = 0;

    capa["optional"].removeMember("realtime");

    lastReTime = 0; /*LTS*/
    finalMillis = 0;
    capa["name"] = "Buffer";
    JSON::Value option;
    option["arg"] = "integer";
    option["long"] = "buffer";
    option["short"] = "b";
    option["help"] = "DVR buffer time in ms";
    option["value"].append(50000);
    config->addOption("bufferTime", option);
    option["long"] = "idleTime";
    option["short"] = "I";
    option["help"] = "Max track idle time in ms";
    config->addOption("idleTime", option);
    capa["optional"]["DVR"]["name"] = "Buffer time (ms)";
    capa["optional"]["DVR"]["help"] =
        "The target available buffer time for this live stream, in milliseconds. This is the time "
        "available to seek around in, and will automatically be extended to fit whole keyframes as "
        "well as the minimum duration needed for stable playback.";
    capa["optional"]["DVR"]["option"] = "--buffer";
    capa["optional"]["DVR"]["type"] = "uint";
    capa["optional"]["DVR"]["default"] = 50000;

    capa["optional"]["idleTime"]["name"] = "Track idle time (ms)";
    capa["optional"]["idleTime"]["help"] = "Maximum tolerated duration a track will be allowed to be idle for before being deleted. If a track is idle for more than 5 seconds and its last timestamp differs from still-active tracks by at least this much, it will also be deleted.";
    capa["optional"]["idleTime"]["option"] = "--idleTime";
    capa["optional"]["idleTime"]["type"] = "uint";
    capa["optional"]["idleTime"]["default"] = 50000;
    /*LTS-start*/
    option.null();
    option["arg"] = "integer";
    option["long"] = "cut";
    option["short"] = "c";
    option["help"] = "Any timestamps before this will be cut from the live buffer";
    option["value"].append(0);
    config->addOption("cut", option);
    capa["optional"]["cut"]["name"] = "Cut time (ms)";
    capa["optional"]["cut"]["help"] =
        "Any timestamps before this will be cut from the live buffer.";
    capa["optional"]["cut"]["option"] = "--cut";
    capa["optional"]["cut"]["type"] = "uint";
    capa["optional"]["cut"]["default"] = 0;
    option.null();

    option["arg"] = "integer";
    option["long"] = "resume";
    option["short"] = "R";
    option["help"] = "Enable resuming support (1) or disable resuming support (0, default)";
    option["value"].append(0);
    config->addOption("resume", option);
    capa["optional"]["resume"]["name"] = "Resume support";
    capa["optional"]["resume"]["help"] =
        "If enabled, the buffer will linger after source disconnect to allow resuming the stream "
        "later. If disabled, the buffer will instantly close on source disconnect.";
    capa["optional"]["resume"]["option"] = "--resume";
    capa["optional"]["resume"]["type"] = "select";
    capa["optional"]["resume"]["select"][0u][0u] = "0";
    capa["optional"]["resume"]["select"][0u][1u] = "Disabled";
    capa["optional"]["resume"]["select"][1u][0u] = "1";
    capa["optional"]["resume"]["select"][1u][1u] = "Enabled";
    capa["optional"]["resume"]["default"] = 0;
    option.null();

    option["arg"] = "integer";
    option["long"] = "maxkeepaway";
    option["short"] = "M";
    option["help"] = "Maximum distance in milliseconds to fall behind the live point for stable playback.";
    option["value"].append(45000);
    config->addOption("maxkeepaway", option);
    capa["optional"]["maxkeepaway"]["name"] = "Maximum live keep-away distance";
    capa["optional"]["maxkeepaway"]["help"] = "Maximum distance in milliseconds to fall behind the live point for stable playback.";
    capa["optional"]["maxkeepaway"]["option"] = "--maxkeepaway";
    capa["optional"]["maxkeepaway"]["type"] = "uint";
    capa["optional"]["maxkeepaway"]["default"] = 45000;
    maxKeepAway = 45000;
    option.null();

    option["arg"] = "integer";
    option["long"] = "segment-size";
    option["short"] = "S";
    option["help"] = "Target time duration in milliseconds for segments";
    option["value"].append(DEFAULT_FRAGMENT_DURATION);
    config->addOption("segmentsize", option);
    capa["optional"]["segmentsize"]["name"] = "Segment size (ms)";
    capa["optional"]["segmentsize"]["help"] = "Target time duration in milliseconds for segments.";
    capa["optional"]["segmentsize"]["option"] = "--segment-size";
    capa["optional"]["segmentsize"]["type"] = "uint";
    capa["optional"]["segmentsize"]["default"] = DEFAULT_FRAGMENT_DURATION;

    capa["optional"]["fallback_stream"]["name"] = "Fallback stream";
    capa["optional"]["fallback_stream"]["help"] =
        "Alternative stream to load for playback when there is no active broadcast";
    capa["optional"]["fallback_stream"]["type"] = "str";
    capa["optional"]["fallback_stream"]["default"] = "";
    option.null();
    capa["optional"]["DVR"]["display"] = "always";
    capa["optional"]["resume"]["display"] = "always";
    /*LTS-end*/

    capa["source_name"] = "Receiving a push"; //shown instead of "Buffer" in the settings page help text
    capa["source_match"] = "push://*";
    capa["source_prefill"] = "push://";
    capa["source_syntax"] = "push://[host][@password]";
    capa["source_help"] = "Set up another application/server to push a stream into MistServer. [Host] and [@password] are optional, but enforced if set up. Host only allows the matching address to push into MistServer, a matching password allows non-matching addresses to push into MistServer as well. For a list of matching methods see below. Methods such as stream keys, the USER_NEW trigger and JWTs will bypass this matching.";
    capa["non-provider"] = true; // Indicates we don't provide data, only collect it
    capa["priority"] = 9;
    capa["desc"] =
        "This input type is both used for push- and pull-based streams. It provides a buffer for "
        "live media data. The push://[host][@password] style source allows all enabled protocols "
        "that support push input to accept a push into MistServer, where you can accept incoming "
        "streams from everyone, based on a set password, and/or use hostname/IP whitelisting.";
    capa["source_desc"] = "This input type is used for push based streams. It provides a buffer for "
        "live media data. The push://[host][@password] style source allows all enabled protocols "
        "that support push input to accept a push into MistServer, where you can accept incoming "
        "streams from everyone, based on a set password, and/or use hostname/IP whitelisting."; //shown in the settings page input description instead of capa["desc"]
    bufferTime = 50000;
    idleTime = 50000;
    cutTime = 0;
    segmentSize = DEFAULT_FRAGMENT_DURATION;
    hasPush = false;
    everHadPush = false;
    resumeMode = false;
    processControlledRealtime = false;
    drainConsumerUsers = 0;
  }

  InputBuffer::~InputBuffer(){
    config->is_active = false;
  }

  /// Cleans up any left-over data for the current stream
  void InputBuffer::onCrash(){
    WARN_MSG("Buffer crashed. Cleaning.");
    streamName = config->getString("streamname");

    // Scoping to clear up users page
    {
      Comms::Users cleanUsers;
      cleanUsers.reload(streamName);
      cleanUsers.finishAll();
      cleanUsers.setMaster(true);
    }
    // Scoping to clear up metadata pages
    {
      DTSC::Meta cleanMeta(streamName, false);
      cleanMeta.setMaster(true);
    }
  }

  /// Intended to be triggered by USR2 signals, this function prints internal state information as log messages.
  void InputBuffer::onDebug() {
    // Temporarily reset debug level to INFO so the messages are visible no matter what
    int32_t dbgOld = Util::printDebugLevel;
    Util::printDebugLevel = DLVL_INFO;
    // Print some helpful debugging messages

    // Process info
    std::set<size_t> gPids;
    INFO_MSG("There are %zu running processes:", runningProcs.size());
    for (std::map<std::string, pid_t>::iterator it = runningProcs.begin(); it != runningProcs.end(); it++) {
      INFO_MSG("Process PID %d: %s", it->second, it->first.c_str());
      gPids.insert(it->second);
    }

    size_t cUsers = 0;
    bool hPush = false;
    size_t lastUser = users.recordCount();
    for (size_t i = 0; i < lastUser; ++i) {
      if (users.getStatus(i) == COMM_STATUS_INVALID) { continue; }

      if (!(users.getStatus(i) & COMM_STATUS_DISCONNECT) && (users.getStatus(i) & COMM_STATUS_SOURCE)) {
        INFO_MSG("Connection %zu (PID %" PRIu32 ") is a source for track %" PRIu32, i, users.getPid(i), users.getTrack(i));
        if (M && M.trackValid(users.getTrack(i)) && !gPids.count(users.getPid(i))) { hPush = true; }
      }

      if (!(users.getStatus(i) & COMM_STATUS_DONOTTRACK) && !gPids.count(users.getPid(i))) { ++cUsers; }
    }
    INFO_MSG("%zu active connections to tracks", cUsers);
    INFO_MSG("Push active (non-process): %s", hPush ? "Yes" : "No");
    if (M) {
      uint64_t time = Util::bootSecs();
      std::set<size_t> aTrks = M.getValidTracks();
      INFO_MSG("There are %zu active tracks:", aTrks.size());
      for (std::set<size_t>::iterator it = aTrks.begin(); it != aTrks.end(); it++) {
        INFO_MSG("Track %zu: %s", *it, M.getTrackIdentifier(*it).c_str());
        INFO_MSG("  Contains timestamps %" PRIu64 " - %" PRIu64, M.getFirstms(*it), M.getLastms(*it));
        INFO_MSG("  Last updated %" PRId64 "s ago", (int64_t)(time - M.getLastUpdated(*it)));
      }
      JSON::Value stream_details;
      M.getHealthJSON(stream_details);
      INFO_MSG("Health: %s", stream_details.toString().c_str());
    } else {
      INFO_MSG("The buffer's metadata is disconnected or uninitialized!");
    }

    // Change debug level back to what it was
    Util::printDebugLevel = dbgOld;
  }

  /// \triggers
  /// The `"STREAM_BUFFER"` trigger is stream-specific, and is ran whenever the buffer changes state
  /// between playable (FULL) or not (EMPTY). It cannot be cancelled. It is possible to receive
  /// multiple EMPTY calls without FULL calls in between, as EMPTY is always generated when a stream
  /// is unloaded from memory, even if this stream never reached playable state in the first place
  /// (e.g. a broadcast was cancelled before filling enough buffer to be playable). Its payload is:
  /// ~~~~~~~~~~~~~~~
  /// streamname
  /// FULL, EMPTY, DRY or RECOVER (depending on current state)
  /// Detected issues in string format, or empty string if no issues
  /// ~~~~~~~~~~~~~~~
  void InputBuffer::updateMeta(){
    if (!M){
      Util::logExitReason(ER_SHM_LOST, "Lost connection to metadata");
      return;
    }
    static bool wentDry = false;
    static uint64_t lastFragCount = 0xFFFFull;
    size_t currBPS = 0;
    uint64_t firstms = 0xFFFFFFFFFFFFFFFFull;
    uint64_t lastms = 0;
    uint64_t fragCount = 0xFFFFull;
    std::set<size_t> validTracks = M.getValidTracks();
    for (std::set<size_t>::iterator it = validTracks.begin(); it != validTracks.end(); it++){
      size_t i = *it;
      currBPS += M.getBps(i); /*LTS*/
      if (M.getType(i) == "meta" || !M.getType(i).size()){continue;}
      std::string init = M.getInit(i);
      // Prevent init data from being thrown away
      if (init.size()){
        if (!initData.count(i) || initData[i] != init){initData[i] = init;}
      }else{
        if (initData.count(i)){meta.setInit(i, initData[i]);}
      }
      if (M.hasEmbeddedFrames(i)){
        fragCount = FRAG_BOOT;
      }else{
        DTSC::Fragments fragments(M.fragments(i));
        if (fragments.getEndValid() < fragCount){fragCount = fragments.getEndValid();}
      }
      if (M.getFirstms(i) < firstms){firstms = M.getFirstms(i);}
      if (M.getLastms(i) > lastms){lastms = M.getLastms(i);}
    }
    if (currBPS != lastBPS){
      lastBPS = currBPS;
      if (Triggers::shouldTrigger("LIVE_BANDWIDTH", streamName, [this](const char *param) {
        if (!param) { return false; }
        DONTEVEN_MSG("Comparing %s to %zu", param, lastBPS);
        return JSON::Value(param).asInt() <= lastBPS;
      })) {
        std::stringstream pl;
        pl << streamName << "\n" << lastBPS;
        std::string payload = pl.str();
        if (!Triggers::doTrigger("LIVE_BANDWIDTH", payload, streamName)){
          WARN_MSG("Shutting down buffer because bandwidth limit reached!");
          config->is_active = false;
          userSelect.clear();
        }
      }
    }
    if (fragCount >= FRAG_BOOT && fragCount != 0xFFFFull){
      JSON::Value stream_details;
      M.getHealthJSON(stream_details);
      if ((lastFragCount == 0xFFFFull || stream_details.isMember("issues") != wentDry) && Triggers::shouldTrigger("STREAM_BUFFER", streamName)){
        if (lastFragCount == 0xFFFFull){
          std::string payload = streamName + "\nFULL\n" + stream_details.toString();
          Triggers::doTrigger("STREAM_BUFFER", payload, streamName);
        }else{
          if (stream_details.isMember("issues")){
            std::string payload = streamName + "\nDRY\n" + stream_details.toString();
            Triggers::doTrigger("STREAM_BUFFER", payload, streamName);
          }else{
            std::string payload = streamName + "\nRECOVER\n" + stream_details.toString();
            Triggers::doTrigger("STREAM_BUFFER", payload, streamName);
          }
        }
      }
      wentDry = stream_details.isMember("issues");
      lastFragCount = fragCount;
    }
    finalMillis = lastms;
    meta.setBufferWindow(lastms - firstms);
    meta.setLive(true);
  }

  bool InputBuffer::keepRunning(bool updateActCtr) {
    if (M.getLive() && updateActCtr) {
      uint64_t currLastUpdate = M.getLastUpdated();
      if (currLastUpdate > activityCounter) {
        if ((connectedUsers || isAlwaysOn()) && M.getValidTracks().size()) { activityCounter = currLastUpdate; }
      }
    }
    return Input::keepRunning(false);
  }

  /// Checks if removing a key from this track is allowed/safe, and if so, removes it.
  /// Returns true if a key was actually removed, false otherwise
  /// Aborts if any of the following conditions are true (while active):
  /// * no keys present
  /// * not at least 4 whole fragments present
  /// * first fragment hasn't been at least lastms-firstms ms in buffer
  /// * less than 8 times the biggest fragment duration is buffered
  /// If a key was deleted and the first buffered data page is no longer used, it is deleted also.
  bool InputBuffer::removeKey(size_t tid){
    DTSC::Keys keys(M.keys(tid));
    // If this track is empty, abort
    if (!keys.getValidCount()){return false;}
    // the following checks only run if we're not shutting down
    if (config->is_active){
      // Make sure we have at least 4 whole fragments at all times,
      DTSC::Fragments fragments(M.fragments(tid));
      if (fragments.getValidCount() < 5){return false;}
      // ensure we have each fragment buffered for at least the whole bufferTime
      if ((M.getLastms(tid) - M.getFirstms(tid)) < bufferTime){return false;}
      uint32_t firstFragment = fragments.getFirstValid();
      uint32_t endFragment = fragments.getEndValid();
      if (endFragment - firstFragment > 2){
        /// Make sure we have at least 8X the target duration.
        // The target duration is the biggest fragment, rounded up to whole seconds.
        uint64_t targetDuration = (M.biggestFragment(tid) / 1000 + 1) * 1000;
        // The start is the third fragment's begin
        uint64_t fragStart = keys.getTime(fragments.getFirstKey(firstFragment));
        // The end is the last fragment's begin
        uint64_t fragEnd = keys.getTime(fragments.getFirstKey(endFragment - 1));
        if ((fragEnd - fragStart) < (targetDuration * 8)){return false;}
      }
    }
    // Alright, everything looks good, let's delete the key and possibly also fragment
    return meta.removeFirstKey(tid);
  }

  void InputBuffer::finish(){
    if (M.getValidTracks().size()){
      /*LTS-START*/
      if (M.getBufferWindow()){
        if (Triggers::shouldTrigger("STREAM_BUFFER")){
          std::string payload =
              config->getString("streamname") + "\nEMPTY\n" + JSON::Value(finalMillis).asString();
          Triggers::doTrigger("STREAM_BUFFER", payload, config->getString("streamname"));
        }
      }
      /*LTS-END*/
    }
    Input::finish();
    updateMeta();
  }

  void InputBuffer::removeTrack(size_t tid){
    size_t lastUser = users.recordCount();
    for (size_t i = 0; i < lastUser; ++i){
      if (users.getStatus(i) == COMM_STATUS_INVALID){continue;}
      if (!(users.getStatus(i) & COMM_STATUS_SOURCE)){continue;}
      if (users.getTrack(i) != tid){continue;}
      // We have found the right track here (pid matches, and COMM_STATUS_SOURCE set)
      users.setStatus(COMM_STATUS_REQDISCONNECT | users.getStatus(i), i);
      break;
    }

    INFO_MSG("Should remove track %zu", tid);
    meta.reloadReplacedPagesIfNeeded();
    meta.removeTrack(tid);
    /*LTS-START*/
    if (!M.getValidTracks().size()){
      if (Triggers::shouldTrigger("STREAM_BUFFER")){
        std::string payload = config->getString("streamname") + "\nEMPTY";
        Triggers::doTrigger("STREAM_BUFFER", payload, config->getString("streamname"));
      }
    }
    /*LTS-END*/
  }

  void InputBuffer::removeUnused(){
    meta.reloadReplacedPagesIfNeeded();
    if (!meta){
      return;
    }
    // first remove all tracks that have not been updated for too long
    bool changed = true;
    while (changed){
      changed = false;
      uint64_t time = Util::bootSecs();
      uint64_t compareFirst = 0xFFFFFFFFFFFFFFFFull;
      uint64_t compareLast = 0;
      std::set<std::string> activeTypes;

      std::set<size_t> tracks = M.getValidTracks();
      std::set<size_t> tracksWithData = M.getValidTracks(true);
      // for tracks that were updated in the last 5 seconds, get the first and last ms edges.
      for (std::set<size_t>::iterator idx = tracks.begin(); idx != tracks.end(); idx++){
        size_t i = *idx;
        if ((time - M.getLastUpdated(i)) > 5){continue;}
        if (!tracksWithData.count(i)) { continue; }
        activeTypes.insert(M.getType(i));
        if (M.getLastms(i) > compareLast){compareLast = M.getLastms(i);}
        if (M.getFirstms(i) < compareFirst){compareFirst = M.getFirstms(i);}
      }
      for (std::set<size_t>::iterator idx = tracks.begin(); idx != tracks.end(); idx++){
        size_t i = *idx;
        //Don't delete idle metadata tracks
        if (M.getType(i) == "meta") {
          if (!M.isClaimed(i)) {
            // Not claimed? Update NowMs to ~50ms ago.
            meta.upNowms(i, Util::bootMS() - 50 - M.getBootMsOffset());
          }
          continue;
        }
        uint64_t lastUp = M.getLastUpdated(i);
        //Prevent issues when getLastUpdated > current time. This can happen if the second rolls over exactly during this loop.
        if (lastUp >= time){continue;}
        std::string codec = M.getCodec(i);
        std::string type = M.getType(i);
        uint64_t firstms = M.getFirstms(i);
        uint64_t lastms = M.getLastms(i);
        bool hasData = tracksWithData.count(i);
        // if not updated for an entire buffer duration, or last updated track and this track differ
        // by an entire buffer duration, erase the track.
        if (time - lastUp > (idleTime / 1000) ||
            (hasData && compareLast && activeTypes.count(type) && (time - lastUp) > 5 &&
             ((compareLast < firstms && (firstms - compareLast) > idleTime) ||
              (compareFirst > lastms && (compareFirst - lastms) > idleTime)))) {
          // erase this track
          if ((time - lastUp) > (idleTime / 1000)){
            WARN_MSG("Erasing %s track %zu (%s/%s) because not updated for %" PRIu64 "s (> %" PRIu64 "s)",
                     streamName.c_str(), i, type.c_str(), codec.c_str(), time - lastUp,
                     idleTime / 1000);
          }else{
            WARN_MSG("Erasing %s inactive track %zu (%s/%s) because it was inactive for 5+ seconds "
                     "and contains data (%" PRIu64 "s - %" PRIu64
                     "s), while active tracks are (%" PRIu64 "s - %" PRIu64
                     "s), which is more than %" PRIu64 "s seconds apart.",
                     streamName.c_str(), i, type.c_str(), codec.c_str(), firstms / 1000,
                     lastms / 1000, compareFirst / 1000, compareLast / 1000, idleTime / 1000);
          }
          meta.reloadReplacedPagesIfNeeded();
          removeTrack(i);
          changed = true;
          break;
        }
      }
    }

    std::set<size_t> tracks = M.getValidTracks();

    // find the earliest video keyframe stored
    uint64_t videoFirstms = 0xFFFFFFFFFFFFFFFFull;

    for (std::set<size_t>::iterator idx = tracks.begin(); idx != tracks.end(); idx++){
      size_t i = *idx;
      if (!M.trackLoaded(i)){continue;}
      if (M.getType(i) == "video"){
        if (M.getFirstms(i) < videoFirstms){videoFirstms = M.getFirstms(i);}
      }
    }
    for (std::set<size_t>::iterator idx = tracks.begin(); idx != tracks.end(); idx++){
      size_t i = *idx;
      if (!M.trackLoaded(i)){continue;}
      if (M.hasEmbeddedFrames(i)){continue;}
      std::string type = M.getType(i);
      DTSC::Keys keys(M.keys(i));
      // non-video tracks need to have a second keyframe that is <= firstVideo
      // firstVideo = 1 happens when there are no tracks, in which case we don't care any more
      uint32_t firstKey = keys.getFirstValid();
      uint32_t endKey = keys.getEndValid();
      if (type != "video" && videoFirstms != 0xFFFFFFFFFFFFFFFFull){
        if ((endKey - firstKey) < 2 || keys.getTime(firstKey + 1) > videoFirstms){continue;}
      }
      // Buffer cutting
      while (keys.getValidCount() > 1 && keys.getTime(keys.getFirstValid()) < cutTime){
        if (!removeKey(i)){break;}
      }
      // Buffer size management
      /// \TODO Make sure data has been in the buffer for at least bufferTime after it goes in
      while (keys.getValidCount() > 1 && (M.getLastms(i) - keys.getTime(keys.getFirstValid() + 1)) > bufferTime){
        if (!removeKey(i)){break;}
      }
      Util::RelAccX &tPages = meta.pages(i);
      Util::RelAccXFieldData firstKeyEnt = tPages.getFieldData("firstkey");
      Util::RelAccXFieldData keyCount = tPages.getFieldData("keycount");
      for (uint32_t j = tPages.getDeleted(); j < tPages.getEndPos(); j++){
        if (tPages.getInt(firstKeyEnt, j) + tPages.getInt(keyCount, j) > firstKey){break;}
        bufferRemove(i, tPages.getInt(firstKeyEnt, j), j);
      }
    }
    updateMeta();
  }

  void InputBuffer::updateProcessingRate() {
    if (runningProcs.empty()) { return; }
    uint64_t now = Util::bootMS();
    uint64_t interval = effectiveSpeed ? 1000 + rateJitterMs : 100;
    if (lastRateUpdateMs && now - lastRateUpdateMs < interval) { return; }
    lastRateUpdateMs = now;

    uint64_t operatorCap = 0;
    {
      std::string strName = config->getString("streamname");
      Util::sanitizeName(strName);
      strName = strName.substr(0, strName.find_first_of("+ "));
      char confName[NAME_BUFFER_SIZE];
      snprintf(confName, sizeof(confName), SHM_STREAM_CONF, strName.c_str());
      Util::DTSCShmReader configReader(confName);
      DTSC::Scan cfg = configReader.getScan();
      if (cfg && cfg.getMember("realtime_speed")) { operatorCap = cfg.getMember("realtime_speed").asInt(); }
    }

    bool allContractsReady = true;
    bool anyHardSlow = false, hardLockout = false, anyRegularSlow = false;
    bool anyStaleHold = false, anyCpuPrimary = false, sawFresh = false;
    bool tickSourceLimited = false, tickProcessorLimited = false, tickWarmup = false;
    uint64_t targetSpeed = 0;
    uint32_t tickInputQ = 0, tickOutputQ = 0, tickCapacityQ = 0;
    size_t requiredCount = 0;

    for (auto it = lastConsumedUpdateMs.begin(); it != lastConsumedUpdateMs.end();) {
      bool alive = false;
      for (auto & rp : runningProcs) {
        if (rp.second == it->first) {
          alive = true;
          break;
        }
      }
      if (!alive) {
        procsReadyForSpeedUp.erase(it->first);
        it = lastConsumedUpdateMs.erase(it);
      } else {
        ++it;
      }
    }

    for (auto & rp : runningProcs) {
      pid_t pid = rp.second;
      if (!pid) { continue; }
      JSON::Value args = JSON::fromString(rp.first);
      bool inconsequential = args.isMember("inconsequential") && args["inconsequential"].asBool();
      if (!inconsequential) { ++requiredCount; }

      char pageName[NAME_BUFFER_SIZE];
      snprintf(pageName, sizeof(pageName), SHM_PROC_STATE, pid);
      IPC::sharedPage page(pageName, 0, false, false);
      ProcState cur;
      if (!ProcState::readSnapshot(page, cur)) {
        if (!inconsequential) {
          allContractsReady = false;
          anyStaleHold = true;
          procsReadyForSpeedUp.erase(pid);
        }
        if (page) { page.master = false; }
        continue;
      }
      page.master = false;

      bool stale = !cur.lastUpdateMs || now < cur.lastUpdateMs || now - cur.lastUpdateMs > 5000;
      bool contractReady = cur.phase >= PRC_PHASE_STARTUP && cur.recommendedFeedQ16_16;
      if (stale || !contractReady) {
        if (!inconsequential) {
          allContractsReady = false;
          anyStaleHold = true;
          procsReadyForSpeedUp.erase(pid);
        }
        continue;
      }

      uint64_t recommended = std::max((uint64_t)1, (uint64_t)(cur.recommendedFeedQ16_16 / 65536));
      if (cur.recommendedFeedQ16_16 & 0xFFFF) { recommended += 1; }
      if (!inconsequential && (!targetSpeed || recommended < targetSpeed)) { targetSpeed = recommended; }
      if (!inconsequential && cur.primaryResource == PRC_RESOURCE_CPU) { anyCpuPrimary = true; }
      if (!inconsequential && cur.phase < PRC_PHASE_READY) { tickWarmup = true; }

      bool fresh = !lastConsumedUpdateMs.count(pid) || cur.lastUpdateMs > lastConsumedUpdateMs[pid];
      lastConsumedUpdateMs[pid] = cur.lastUpdateMs;
      if (!fresh) { continue; }
      sawFresh = true;

      if (inconsequential) { continue; }
      if (cur.flags & PRC_FLAG_SOURCE_LIMITED) { tickSourceLimited = true; }
      if (cur.flags & PRC_FLAG_PROCESSOR_LIMITED) { tickProcessorLimited = true; }
      if (cur.inputSpeedQ16_16 && (!tickInputQ || cur.inputSpeedQ16_16 < tickInputQ)) {
        tickInputQ = cur.inputSpeedQ16_16;
      }
      if (cur.outputSpeedQ16_16 && (!tickOutputQ || cur.outputSpeedQ16_16 < tickOutputQ)) {
        tickOutputQ = cur.outputSpeedQ16_16;
      }
      if ((cur.flags & PRC_FLAG_CAPACITY_VALID) && !(cur.flags & PRC_FLAG_SOURCE_LIMITED) && cur.confidenceQ0_16 &&
          cur.capacitySpeedQ16_16 && (!tickCapacityQ || cur.capacitySpeedQ16_16 < tickCapacityQ)) {
        tickCapacityQ = cur.capacitySpeedQ16_16;
      }

      ProcFeedVote feedVote = classifyProcFeedVote(cur.flags, cur.reasonCode, cur.canAcceptMore, cur.pressureQ0_16);
      if (feedVote == PROC_FEED_HARD_LOCKOUT) {
        anyHardSlow = true;
        hardLockout = true;
        procsReadyForSpeedUp.erase(pid);
      } else if (feedVote == PROC_FEED_HARD) {
        anyHardSlow = true;
        procsReadyForSpeedUp.erase(pid);
      } else if (feedVote == PROC_FEED_SLOW) {
        anyRegularSlow = true;
        procsReadyForSpeedUp.erase(pid);
      } else {
        // SOURCE_LIMITED is intentionally eligible: requested and achieved
        // rates are separate, and source starvation must not score the proc.
        procsReadyForSpeedUp.insert(pid);
      }
    }

    if (!targetSpeed) { targetSpeed = 1; }
    if (operatorCap) { targetSpeed = std::min(targetSpeed, operatorCap); }
    if (!allContractsReady) { targetSpeed = 1; }

    bool nodeHold = false, nodeSlow = false;
    if (anyCpuPrimary) {
      IPC::sharedPage nodePage(SHM_NODE_PRESSURE, 0, false, false);
      NodePressureState node;
      if (NodePressureState::readSnapshot(nodePage, node) && node.lastUpdateMs && now >= node.lastUpdateMs &&
          now - node.lastUpdateMs <= 3000) {
        uint8_t verdict = node.cpuVerdict();
        nodeHold = verdict >= 1;
        nodeSlow = verdict >= 2;
      }
      if (nodePage) { nodePage.master = false; }
    }

    uint64_t previous = effectiveSpeed;
    if (rampLockoutTicks) { --rampLockoutTicks; }
    size_t readyVoteCount = procsReadyForSpeedUp.size();
    bool allReady = requiredCount && readyVoteCount >= requiredCount;
    ProcessingRateInput rateInput;
    // The first complete, unpressured contract set is the proc-authored
    // bootstrap seed, not a ramp destination. Before it arrives the feeder
    // runs at its normal 1x fallback. This avoids losing several seconds to
    // 1->2->3->... merely because InputBuffer observed the BOOTING page first.
    bool applyStartupSeed = !startupSeedApplied && allContractsReady && !anyHardSlow && !anyRegularSlow && !nodeHold && !nodeSlow;
    rateInput.current = applyStartupSeed ? 0 : effectiveSpeed;
    rateInput.target = targetSpeed;
    rateInput.hardSlow = anyHardSlow;
    rateInput.regularSlow = anyRegularSlow;
    rateInput.nodeSlow = nodeSlow;
    rateInput.nodeHold = nodeHold;
    rateInput.freshVoteRound = sawFresh && allReady;
    rateInput.contractsReady = allContractsReady;
    rateInput.rampLocked = rampLockoutTicks;
    ProcessingRateResult rateResult = decideProcessingRate(rateInput);
    effectiveSpeed = rateResult.speed;
    if (allContractsReady) { startupSeedApplied = true; }
    bool countHardSlow = false, countRegularSlow = false, countRamp = false, countNodeLimited = false;
    if (anyHardSlow) {
      procsReadyForSpeedUp.clear();
      countHardSlow = true;
      if (hardLockout) { rampLockoutTicks = 10; }
    } else if (anyRegularSlow || nodeSlow) {
      procsReadyForSpeedUp.clear();
      countRegularSlow = true;
      countNodeLimited = nodeSlow;
    } else if (previous && targetSpeed < previous) {
      procsReadyForSpeedUp.clear();
    } else if (rateResult.ramped) {
      procsReadyForSpeedUp.clear();
      countRamp = true;
    } else if (nodeHold) {
      countNodeLimited = true;
    }
    if (operatorCap && effectiveSpeed > operatorCap) { effectiveSpeed = operatorCap; }

    if (previous != effectiveSpeed) {
      INFO_MSG("Processing rate changed: %" PRIu64 "x -> %" PRIu64 "x (target=%" PRIu64
               "x, ready=%zu/%zu, hard=%d, slow=%d, nodeHold=%d, lockout=%u)",
               previous, effectiveSpeed, targetSpeed, readyVoteCount, requiredCount, anyHardSlow,
               anyRegularSlow || nodeSlow, nodeHold, rampLockoutTicks);
    }

    ProcessStreamStateTick diagnosticTick;
    diagnosticTick.effectiveSpeed = effectiveSpeed;
    diagnosticTick.hardSlow = countHardSlow;
    diagnosticTick.regularSlow = countRegularSlow;
    diagnosticTick.ramped = countRamp;
    diagnosticTick.lockout = rampLockoutTicks;
    diagnosticTick.staleHold = anyStaleHold;
    diagnosticTick.warmup = tickWarmup;
    diagnosticTick.sourceLimited = tickSourceLimited;
    diagnosticTick.processorLimited = tickProcessorLimited;
    diagnosticTick.nodeLimited = countNodeLimited;
    diagnosticTick.inputSpeedQ16 = tickInputQ;
    diagnosticTick.outputSpeedQ16 = tickOutputQ;
    diagnosticTick.capacitySpeedQ16 = tickCapacityQ;
    speedStats.recordTick(diagnosticTick);
    if (streamStatus && streamStatus.len >= 16) {
      memcpy(streamStatus.mapped + STRMSTATE_EFFECTIVE_SPEED_OFFSET, &effectiveSpeed, sizeof(uint64_t));
    }
    if (streamStatus && streamStatus.len >= STRMSTATE_PAGE_LEN) {
      speedStats.writeStatistics(streamStatus.mapped, streamStatus.len);
    }
  }

  void InputBuffer::userLeadIn(){
    meta.reloadReplacedPagesIfNeeded();
    /*LTS-START*/
    // Reload the configuration to make sure we stay up to date with changes through the api
    if (Util::epoch() - lastReTime > 4){preRun();}
    size_t procInterval = 5000;
    if (!firstProcTime || Util::bootMS() - firstProcTime < 30000){
      if (!firstProcTime){firstProcTime = Util::bootMS();}
      if (Util::bootMS() - firstProcTime < 10000){
        procInterval = 200;
      }else{
        procInterval = 1000;
      }
    }
    if (Util::bootMS() - lastProcTime > procInterval){
      lastProcTime = Util::bootMS();
      std::string fullName = config->getString("streamname");
      Util::sanitizeName(fullName);
      std::string strName = fullName.substr(0, fullName.find_first_of("+ "));
      char tmpBuf[NAME_BUFFER_SIZE];
      snprintf(tmpBuf, NAME_BUFFER_SIZE, SHM_STREAM_CONF, strName.c_str());
      Util::DTSCShmReader rStrmConf(tmpBuf);
      DTSC::Scan streamCfg = rStrmConf.getScan();
      if (streamCfg){
        JSON::Value configuredProcesses;
        /*LTS-START*/
        if (!processOverrideResolved) {
          processOverrideResolved = true;
          std::string fullStreamName = config->getString("streamname");
          if (Triggers::shouldTrigger("STREAM_PROCESS", fullStreamName)) {
            Triggers::Result triggerResult;
            Triggers::doTrigger("STREAM_PROCESS", fullStreamName, fullStreamName, false, triggerResult);
            if ((triggerResult.action == Triggers::ACT_VALUE || triggerResult.action == Triggers::ACT_KEEP) &&
                triggerResult.response.size()) {
              processOverride = JSON::fromString(triggerResult.response);
              if (!processOverride.isArray()) { processOverride.null(); }
            }
          }
        }
        if (processOverride.isArray() && processOverride.size()) {
          configuredProcesses = processOverride;
        } else {
          configuredProcesses = streamCfg.getMember("processes").asJSON();
        }
        /*LTS-END*/
        processControlledRealtime = streamCfg.getMember("process_controlled_realtime").asBool();
        checkProcesses(configuredProcesses);
        // Published after checkProcesses so retired procs (hard-failed or
        // restart-disabled) drop out of the expectation on the same tick.
        if (processControlledRealtime) { publishProcessingOutputExpectation(configuredProcesses); }
      }else{
        processControlledRealtime = false;
        //If there is no config, we assume all processes are running, since, well, there can't be any
        allProcsRunning = true;
      }
    }
    updateProcessingRate();
    /*LTS-END*/
    connectedUsers = 0;
    drainConsumerUsers = 0;

    //Store child process PIDs in generatePids.
    //These are controlled by the buffer (usually processes) and should not count towards incoming pushes
    generatePids.clear();
    for (std::map<std::string, pid_t>::iterator it = runningProcs.begin(); it != runningProcs.end(); it++){
      generatePids.insert(it->second);
    }
    hasPush = false;
  }
  void InputBuffer::userOnActive(size_t id){
    ///\todo Add tracing of earliest watched keys, to prevent data going out of memory for
    /// still-watching viewers
    if (users.getStatus(id) & COMM_STATUS_SOURCE) {
      bool isProcess = generatePids.count(users.getPid(id));
      if (isProcess) {
        processUsers[id] = users.getTrack(id);
        processPidsWithUsers.insert(users.getPid(id));
      } else {
        sourceUsers[id] = users.getTrack(id);
      }
      // GeneratePids holds the pids of the process that generate data, so ignore those for determining if a push is ingested.
      if (!isProcess && M.trackValid(users.getTrack(id))) { hasPush = true; }
    }

    if (!(users.getStatus(id) & COMM_STATUS_DONOTTRACK)) {
      ++connectedUsers;
      if (!(users.getStatus(id) & COMM_STATUS_SOURCE)) { ++drainConsumerUsers; }
    }
  }
  void InputBuffer::userOnDisconnect(size_t id){
    if (processUsers.count(id)) {
      pid_t procPid = users.getPid(id);
      if (meta.isClaimed(processUsers[id])) {
        INFO_MSG("Track %zu lost its process, but is still claimed! Reclaiming for resume...", processUsers[id]);
        meta.breakClaim(processUsers[id]);
      } else {
        INFO_MSG("Track %zu lost its process and is now unclaimed, keeping it around for resume", processUsers[id]);
      }
      processUsers.erase(id);
      processPidsWithUsers.erase(procPid);
      return;
    }
    if (sourceUsers.count(id)) {
      if (!retainDisconnectedSourceTrack(resumeMode, processControlledRealtime, hasProcessDrainConsumers(),
                                         M.getCodec(sourceUsers[id]) == "rawhls")) {
        INFO_MSG("Disconnected track %zu", sourceUsers[id]);
        meta.reloadReplacedPagesIfNeeded();
        removeTrack(sourceUsers[id]);
      } else {
        if (meta.isClaimed(sourceUsers[id])) {
          INFO_MSG("Track %zu lost its source, but is still claimed! Reclaiming for resume...", sourceUsers[id]);
          meta.breakClaim(sourceUsers[id]);
        } else {
          if (M.getType(sourceUsers[id]) == "meta") {
            HIGH_MSG("Track %zu lost its source and is now unclaimed, keeping it around for resume", sourceUsers[id]);
          } else {
            INFO_MSG("Track %zu lost its source and is now unclaimed, keeping it around for resume", sourceUsers[id]);
          }
        }
        activityCounter = Util::bootSecs();
      }
      sourceUsers.erase(id);
    }
  }
  bool InputBuffer::hasProcessDrainConsumers() const {
    if (processUsers.size()) { return true; }
    for (std::map<std::string, pid_t>::const_iterator it = runningProcs.begin(); it != runningProcs.end(); ++it) {
      if (processPidsWithUsers.count(it->second)) { continue; }
      if (Util::Procs::isActive(it->second)) { return true; }
    }
    return drainConsumerUsers != 0;
  }

  bool InputBuffer::hasActiveProcessProducers() const {
    // Keep treating a just-spawned or just-exited child as pending until
    // checkProcesses has collected it and applied its restart policy. A raw
    // isActive() probe has a startup race before the child is observable.
    return processUsers.size() || runningProcs.size();
  }

  bool InputBuffer::processingProcessMatchesSource(const JSON::Value & proc) const {
    if (!proc.isObject()) { return false; }
    if (proc.isMember("sink") && proc["sink"].isString() && proc["sink"].asStringRef().size()) {
      std::string sink = proc["sink"].asStringRef();
      Util::streamVariables(sink, streamName);
      if (sink != streamName) { return false; }
    }
    auto hasOriginalTrack = [this](const std::set<size_t> & tracks) {
      for (std::set<size_t>::const_iterator it = tracks.begin(); it != tracks.end(); ++it) {
        if (M.getSourceTrack(*it) == INVALID_TRACK_ID) { return true; }
      }
      return false;
    };

    if (proc.isMember("tags_inhibit")) {
      std::set<std::string> tags = Util::streamTags(streamName);
      auto matchesTag = [&tags](const JSON::Value & J) {
        if (!J.isString()) { return false; }
        const std::string & tag = J.asStringRef();
        if (tag.size() && tag[0] == '#') { return tags.count(tag.substr(1)) != 0; }
        return tags.count(tag) != 0;
      };
      const JSON::Value & inhib = proc["tags_inhibit"];
      if (inhib.isString() && matchesTag(inhib)) { return false; }
      if (inhib.isArray()) {
        jsonForEachConst (inhib, it) {
          if (matchesTag(*it)) { return false; }
        }
      }
    }

    if (proc.isMember("source_track")) {
      std::set<size_t> tracks = Util::findTracks(M, JSON::Value(), "", proc["source_track"].asStringRef());
      if (!tracks.size()) { return false; }
    }
    if (proc.isMember("track_select")) {
      std::set<size_t> tracks = Util::wouldSelect(M, proc["track_select"].asStringRef());
      if (!tracks.size()) { return false; }
    }
    if (proc.isMember("track_inhibit")) {
      std::set<size_t> tracks = Util::wouldSelect(
        M, std::string("audio=none&video=none&subtitle=none&meta=none&") + proc["track_inhibit"].asStringRef());
      if (hasOriginalTrack(tracks)) { return false; }
    }
    return true;
  }

  bool InputBuffer::processingProcessMayMatchTranscodeOutput(const JSON::Value & proc, const JSON::Value & procs) const {
    if (!proc.isObject() || !proc.isMember("track_select") || !proc["track_select"].isString() ||
        proc["track_select"].asStringRef().empty()) {
      return false;
    }
    if (proc.isMember("source_track")) { return false; }
    if (proc.isMember("sink") && proc["sink"].isString() && proc["sink"].asStringRef().size()) {
      std::string sink = proc["sink"].asStringRef();
      Util::streamVariables(sink, streamName);
      if (sink != streamName) { return false; }
    }
    if (proc.isMember("tags_inhibit")) {
      std::set<std::string> tags = Util::streamTags(streamName);
      auto inhibited = [&tags](const JSON::Value & value) {
        if (!value.isString()) { return false; }
        const std::string & tag = value.asStringRef();
        return tags.count(tag.size() && tag[0] == '#' ? tag.substr(1) : tag) != 0;
      };
      if (proc["tags_inhibit"].isString() && inhibited(proc["tags_inhibit"])) { return false; }
      if (proc["tags_inhibit"].isArray()) {
        jsonForEachConst (proc["tags_inhibit"], tagIt) {
          if (inhibited(*tagIt)) { return false; }
        }
      }
    }

    DTSC::Meta potential;
    potential.reInit("", true);
    if (!procs.isArray()) { return false; }
    jsonForEachConst (procs, candidate) {
      if (!candidate->isObject() || !candidate->isMember("process")) { continue; }
      const std::string producer = (*candidate)["process"].asString();
      if (producer != "AV" && producer != "FFmpeg") { continue; }
      if (!candidate->isMember("codec") || !(*candidate)["codec"].isString() || (*candidate)["codec"].asStringRef().empty()) {
        continue;
      }
      if (candidate->isMember("target_mask") && !(*candidate)["target_mask"].isNull() &&
          (*candidate)["target_mask"].asString() != "" && !((*candidate)["target_mask"].asInt() & TRACK_VALID_INT_PROCESS)) {
        continue;
      }
      if (!processingProcessMatchesSource(*candidate) || processingProcessRetired(*candidate)) { continue; }

      std::string type = (*candidate)["x-LSP-kind"].asString();
      const std::string codec = (*candidate)["codec"].asString();
      if (type != "audio" && type != "video") {
        static const std::set<std::string> audioCodecs = {"AAC",  "AC3", "ALAW", "FLAC",  "MP3",
                                                          "opus", "PCM", "ULAW", "vorbis"};
        type = audioCodecs.count(codec) ? "audio" : "video";
      }
      const size_t track = potential.addTrack();
      potential.setID(track, track + 1);
      potential.setType(track, type);
      potential.setCodec(track, codec);
      potential.validateTrack(track, TRACK_VALID_ALL);
      potential.update(0, 0, track, 1, 0, true, 1);
    }
    return Util::wouldSelect(potential, proc["track_select"].asStringRef()).size() != 0;
  }

  // A retired process will never (re)produce output tracks: it exited
  // unrecoverably (checkProcesses disabled its restart) or has restarts
  // disabled and its only boot already ended. Retired processes drop out of
  // the published output expectation, so recordings waiting on that
  // expectation unblock instead of waiting for tracks that will never come
  // (e.g. a fallback to source passthrough). Uses the same config-key
  // construction as checkProcesses so the lookups match.
  bool InputBuffer::processingProcessRetired(const JSON::Value & proc) const {
    JSON::Value tmp = proc;
    tmp["source"] = streamName;
    const std::string key = tmp.toString();
    if (procHardFailed.count(key)) { return true; }
    std::string restartType = "fixed";
    if (proc.isMember("restart_type")) { restartType = proc["restart_type"].asString(); }
    if (restartType == "disabled") {
      std::map<std::string, uint32_t>::const_iterator boots = procBoots.find(key);
      if (boots != procBoots.end() && boots->second) {
        std::map<std::string, pid_t>::const_iterator running = runningProcs.find(key);
        if (running == runningProcs.end() || !Util::Procs::isActive(running->second)) { return true; }
      }
    }
    return false;
  }

  size_t InputBuffer::expectedProcessingOutputTracks(const JSON::Value & procs, bool & resolved) const {
    resolved = true;
    if (!procs.isArray() || !procs.size()) { return 0; }
    size_t expectedOutputTracks = 0;
    jsonForEachConst (procs, it) {
      if (!it->isObject() || !it->isMember("process")) { continue; }
      const std::string procName = (*it)["process"].asString();
      // Thumbs is intentionally absent: its JPEG track is enrichment that no
      // consumer gates on, it does not mark its output as a derived track
      // (no setSourceTrack), and a flaky thumbnailer must not hold up the
      // recording of the actual media outputs.
      if (procName != "AV" && procName != "Livepeer" && procName != "FFmpeg" && procName != "ONNX") { continue; }
      const bool matchesCurrentSource = processingProcessMatchesSource(*it);
      const bool mayMatchConfiguredOutput =
        !matchesCurrentSource && procName == "ONNX" && processingProcessMayMatchTranscodeOutput(*it, procs);
      if (!matchesCurrentSource && !mayMatchConfiguredOutput) { continue; }
      if (processingProcessRetired(*it)) { continue; }
      // File outputs select TRACK_VALID_EXT_PUSH. Do not make them wait for
      // derived tracks their own validity mask prevents them from selecting.
      // This is especially important for raw AV/ONNX intermediate tracks in
      // multi-stage processing chains.
      bool recordingVisible = true;
      if (it->isMember("target_mask") && !(*it)["target_mask"].isNull() && (*it)["target_mask"].asString() != "") {
        recordingVisible = ((*it)["target_mask"].asInt() & TRACK_VALID_EXT_PUSH) != 0;
      } else if (procName == "AV") {
        const std::string codec = (*it)["codec"].asString();
        if (codec == "UYVY" || codec == "YUYV" || codec == "I420" || codec == "PCM" || codec == "NV12") {
          recordingVisible = false;
        }
      }
      if (!recordingVisible) { continue; }
      if (procName == "Livepeer" && it->isMember("target_profiles") && (*it)["target_profiles"].isArray()) {
        jsonForEachConst ((*it)["target_profiles"], prof) {
          if (!prof->isObject()) { continue; }
          if (prof->isMember("track_inhibit")) {
            std::set<size_t> tracks = Util::wouldSelect(
              M, std::string("audio=none&video=none&subtitle=none&meta=none&") + (*prof)["track_inhibit"].asStringRef());
            bool hasOriginalTrack = false;
            for (std::set<size_t>::const_iterator trackIt = tracks.begin(); trackIt != tracks.end(); ++trackIt) {
              if (M.getSourceTrack(*trackIt) == INVALID_TRACK_ID) {
                hasOriginalTrack = true;
                break;
              }
            }
            if (hasOriginalTrack) { continue; }
          }
          ++expectedOutputTracks;
        }
      } else if (procName == "ONNX") {
        JSON::Value keyed = *it;
        keyed["source"] = streamName;
        std::map<std::string, pid_t>::const_iterator running = runningProcs.find(keyed.toString());
        if (running == runningProcs.end() || !running->second) {
          resolved = false;
          continue;
        }
        char pageName[NAME_BUFFER_SIZE];
        snprintf(pageName, sizeof(pageName), SHM_PROC_STATE, running->second);
        IPC::sharedPage page(pageName, 0, false, false);
        ProcState state;
        const bool hasContract = ProcState::readSnapshot(page, state) && (state.flags & PRC_FLAG_OUTPUT_CONTRACT_VALID);
        if (page) { page.master = false; }
        if (!hasContract) {
          resolved = false;
          continue;
        }
        expectedOutputTracks += state.expectedOutputTracks;
      } else {
        ++expectedOutputTracks;
      }
    }
    return expectedOutputTracks;
  }

  void InputBuffer::publishProcessingOutputExpectation(const JSON::Value & procs) {
    if (!streamStatus || streamStatus.len < 16) { return; }
    auto publish = [this](bool resolved, uint16_t expected) {
      streamStatus.mapped[STRMSTATE_PROCESS_OUTPUTS_RESOLVED_OFFSET] = resolved;
      streamStatus.mapped[STRMSTATE_PROCESS_FEED_PAUSED_OFFSET] = processControlledRealtime && !resolved;
      memcpy(streamStatus.mapped + STRMSTATE_PROCESS_OUTPUTS_EXPECTED_OFFSET, &expected, sizeof(uint16_t));
    };
    if (!M.getValidTracks().size()) {
      publish(false, 0);
      return;
    }
    bool resolved = false;
    size_t expected = expectedProcessingOutputTracks(procs, resolved);
    if (!resolved) {
      publish(false, 0);
      return;
    }
    if (expected > 0xFFFF) { expected = 0xFFFF; }
    publish(true, (uint16_t)expected);
  }

  void InputBuffer::userLeadOut() {
    static std::set<size_t> prevValidTracks;
    std::set<size_t> validTracks = M.getValidTracks();
    if (validTracks != prevValidTracks){
      MEDIUM_MSG("Valid tracks count changed from %zu to %zu", prevValidTracks.size(), validTracks.size());
      prevValidTracks = validTracks;
      if (Triggers::shouldTrigger("LIVE_TRACK_LIST")){
        JSON::Value triggerPayload;
        M.toJSON(triggerPayload, true, true);
        std::string payload = config->getString("streamname") + "\n" + triggerPayload.toString() + "\n";
        Triggers::doTrigger("LIVE_TRACK_LIST", payload, config->getString("streamname"));
      }
    }

    // rawhls tracks always count as having an active push
    if (config->is_active) {
      for (const size_t & T : validTracks) {
        if (M.getCodec(T) == "rawhls") { hasPush = true; }
      }
    }

    if (config->is_active && streamStatus) {
      if (!processControlledRealtime || streamStatus.mapped[0] != STRMSTAT_SHUTDOWN) {
        streamStatus.mapped[0] = (hasPush && allProcsRunning) ? STRMSTAT_READY : STRMSTAT_WAIT;
      }
      if (processControlledRealtime && streamStatus.len > STRMSTATE_PROCESS_SOURCE_EOF_OFFSET) {
        streamStatus.mapped[STRMSTATE_PROCESS_SOURCE_EOF_OFFSET] = everHadPush && !hasPush;
      }
      if (processControlledRealtime && streamStatus.len > STRMSTATE_PROCESS_PRODUCERS_FINISHED_OFFSET) {
        const bool producersFinished = everHadPush && !hasPush && !hasActiveProcessProducers();
        streamStatus.mapped[STRMSTATE_PROCESS_PRODUCERS_FINISHED_OFFSET] = producersFinished;
      }
    }
    if (hasPush) { everHadPush = true; }
    ProcessingSourceEofAction eofAction = processingSourceEofAction(config->is_active, hasPush, everHadPush, resumeMode,
                                                                    processControlledRealtime, hasProcessDrainConsumers());
    if (eofAction != PROCESSING_EOF_NONE) {
      if (eofAction == PROCESSING_EOF_WAIT) {
        if (streamStatus) { streamStatus.mapped[0] = STRMSTAT_WAIT; }
      } else if (eofAction == PROCESSING_EOF_DRAIN) {
        if (streamStatus) {
          if (streamStatus.mapped[0] != STRMSTAT_SHUTDOWN) {
            INFO_MSG("Process-controlled realtime producers finished; signalling output drain");
          }
          streamStatus.mapped[0] = STRMSTAT_SHUTDOWN;
        }
      } else if (eofAction == PROCESSING_EOF_STOP) {
        Util::logExitReason(ER_CLEAN_EOF, "source disconnected for non-resumable stream");
        if (streamStatus) { streamStatus.mapped[0] = STRMSTAT_SHUTDOWN; }
        config->is_active = false;
        canCancelUnload = false;
        userSelect.clear();
      }
    }
  }

  bool InputBuffer::preRun(){
    // This function gets run periodically to make sure runtime updates of the config get parsed.
    Util::Procs::kill_timeout = 5;
    static bool firstRun = true;
    if (firstRun) {
      firstRun = false;
      // Setup USR2 signal handler for debugging purposes
      myBuf = this;
      struct sigaction new_action;
      new_action.sa_handler = usr2sig_handler;
      sigemptyset(&new_action.sa_mask);
      new_action.sa_flags = 0;
      sigaction(SIGUSR2, &new_action, NULL);
    }
    std::string strName = config->getString("streamname");
    Util::sanitizeName(strName);
    strName = strName.substr(0, (strName.find_first_of("+ ")));
    char tmpBuf[NAME_BUFFER_SIZE];
    snprintf(tmpBuf, NAME_BUFFER_SIZE, SHM_STREAM_CONF, strName.c_str());
    Util::DTSCShmReader rStrmConf(tmpBuf);
    DTSC::Scan streamCfg = rStrmConf.getScan();

    //Check if bufferTime setting is correct
    uint64_t tmpNum = getSettingUInt64(streamCfg, "DVR", "bufferTime");
    if (tmpNum < 1000){tmpNum = 1000;}
    if (bufferTime != tmpNum){
      DEVEL_MSG("Setting bufferTime from %" PRIu64 " to new value of %" PRIu64, bufferTime, tmpNum);
      bufferTime = tmpNum;
    }

    //Check if idleTime setting is correct
    tmpNum = getSettingUInt64(streamCfg, "idleTime", "idleTime");
    if (tmpNum < 1000){tmpNum = 1000;}
    if (idleTime != tmpNum){
      DEVEL_MSG("Setting idleTime from %" PRIu64 " to new value of %" PRIu64, idleTime, tmpNum);
      idleTime = tmpNum;
    }

    //Check if input timeout setting is correct
    tmpNum = getSettingUInt64(streamCfg, "inputtimeout");
    if (inputTimeout != tmpNum){
      DEVEL_MSG("Setting input timeout from %" PRIu64 " to new value of %" PRIu64, inputTimeout, tmpNum);
      inputTimeout = tmpNum;
    }

    //Check if cutTime setting is correct
    tmpNum = getSettingUInt64(streamCfg, "cut");
    // if the new value is different, print a message and apply it
    if (cutTime != tmpNum){
      INFO_MSG("Setting cutTime from %" PRIu64 " to new value of %" PRIu64, cutTime, tmpNum);
      cutTime = tmpNum;
    }

    //Check if resume setting is correct
    tmpNum = getSettingUInt64(streamCfg, "resume");
    if (resumeMode != (bool)tmpNum){
      INFO_MSG("Setting resume mode from %s to new value of %s",
               resumeMode ? "enabled" : "disabled", tmpNum ? "enabled" : "disabled");
      resumeMode = tmpNum;
    }

    if (!meta){return true;}//abort the rest if we can't write metadata
    lastReTime = Util::epoch(); /*LTS*/

    //Check if segmentsize setting is correct
    tmpNum = getSettingUInt64(streamCfg, "segmentsize");
    if (tmpNum < meta.biggestFragment() / 2){tmpNum = meta.biggestFragment() / 2;}
    segmentSize = meta.getMinimumFragmentDuration();
    if (segmentSize != tmpNum){
      INFO_MSG("Setting segmentSize from %zu to new value of %" PRIu64, segmentSize, tmpNum);
      segmentSize = tmpNum;
      meta.setMinimumFragmentDuration(segmentSize);
    }

    //Check if segmentsize setting is correct
    tmpNum = getSettingUInt64(streamCfg, "maxkeepaway");
    if (M.getMaxKeepAway() != tmpNum){
      INFO_MSG("Setting maxKeepAway from %" PRIu64 " to new value of %" PRIu64, M.getMaxKeepAway(), tmpNum);
      meta.setMaxKeepAway(tmpNum);
    }

    return true;
  }

  uint64_t InputBuffer::findTrack(const std::string &trackVal){
    std::set<size_t> validTracks = M.getValidTracks();
    if (!validTracks.size()){
      return INVALID_TRACK_ID;
    }// No tracks == we don't have a valid
                                                           // track
    if (!trackVal.size() || trackVal == "0"){return 0;}// don't select anything in particular
    if (trackVal.find(',') != std::string::npos){
      // Comma-separated list, recurse.
      std::stringstream ss(trackVal);
      std::string item;
      while (std::getline(ss, item, ',')){
        uint64_t r = findTrack(item);
        if (r){return r;}// return first match
      }
      return INVALID_TRACK_ID; // nothing found
    }
    uint64_t trackNo = JSON::Value(trackVal).asInt();
    if (trackVal == JSON::Value(trackNo).asString()){
      // It's an integer number
      if (!validTracks.count(trackNo)){
        return INVALID_TRACK_ID; // nothing found
      }
      return trackNo;
    }
    std::string trackLow = trackVal;
    Util::stringToLower(trackLow);
    if (trackLow == "all" || trackLow == "*"){
      // select all tracks of this type
      return *validTracks.begin();
    }
    // attempt to do language/codec matching
    // convert 2-character language codes into 3-character language codes
    if (trackLow.size() == 2){trackLow = Encodings::ISO639::twoToThree(trackLow);}
    for (std::set<size_t>::iterator it = validTracks.begin(); it != validTracks.end(); it++){
      std::string codecLow = M.getCodec(*it);
      Util::stringToLower(codecLow);
      if (M.getLang(*it) == trackLow || trackLow == codecLow){return *it;}
    }
    return INVALID_TRACK_ID; // nothing found
  }

  /*LTS-START*/
  /// Checks if all processes are running, starts them if needed, stops them if needed
  void InputBuffer::checkProcesses(const JSON::Value &procs){
    allProcsRunning = true;
    if (!M.getValidTracks().size()){return;}
    std::set<std::string> newProcs;
    uint64_t now = Util::bootMS(); //< Used for delayed starts

    // used for building args
    int err = fileno(stderr);

    // Why each configured process is excluded from newProcs this tick.
    // Consulted when stopping a still-running process so the stop log and
    // PROCESS_EXIT trigger name the guard that retired it.
    std::map<std::string, std::string> skipReasons;

    // Convert to strings
    jsonForEachConst(procs, it){
      JSON::Value tmp = *it;
      tmp["source"] = streamName;
      std::string key = tmp.toString();
      if (tmp.isMember("source_track")){
        std::set<size_t> wouldSelect = Util::findTracks(M, JSON::Value(), "", tmp["source_track"].asStringRef());
        // No match - skip this process
        if (!wouldSelect.size()) {
          skipReasons[key] = "source_track '" + tmp["source_track"].asString() + "' matches no tracks";
          continue;
        }
      }
      if (tmp.isMember("track_select")){
        std::set<size_t> wouldSelect = Util::wouldSelect(M, tmp["track_select"].asStringRef());
        // No match - skip this process
        if (!wouldSelect.size()) {
          skipReasons[key] = "track_select '" + tmp["track_select"].asString() + "' matches no tracks";
          continue;
        }
      }
      // If tags_inhibit is set, prevent the process from starting
      if (tmp.isMember("tags_inhibit")) {
        std::set<std::string> T = Util::streamTags(streamName);
        auto matchesTag = [&T](const JSON::Value & J) {
          if (!J.isString()) { return false; }
          const std::string & tag = J.asStringRef();
          if (tag.size() && tag[0] == '#') { return T.count(tag.substr(1)) > 0; }
          return T.count(tag) > 0;
        };
        JSON::Value & inhib = tmp["tags_inhibit"];
        if (inhib.isString()) {
          if (matchesTag(inhib)) {
            skipReasons[key] = "inhibited by stream tag '" + inhib.asString() + "'";
            continue;
          }
        } else if (inhib.isArray()) {
          bool hasMatch = false;
          jsonForEachConst (inhib, tagIt) {
            if (matchesTag(*tagIt)) {
              hasMatch = true;
              break;
            }
          }
          if (hasMatch) {
            skipReasons[key] = "inhibited by stream tags";
            continue;
          }
        }
      }
      if (tmp.isMember("track_inhibit")){
        std::set<size_t> wouldSelect = Util::wouldSelect(
            M, std::string("audio=none&video=none&subtitle=none&meta=none&") + tmp["track_inhibit"].asStringRef());
        if (wouldSelect.size()){
          // Inhibit if there is a match and we're not already running.
          if (!runningProcs.count(key)) {
            skipReasons[key] = "track_inhibit '" + tmp["track_inhibit"].asString() + "' matches existing tracks";
            continue;
          }
          bool inhibited = false;
          std::set<size_t> myTracks = M.getMySourceTracks(runningProcs[key]);
          // Also inhibit if there is a match with not-the-currently-running-process
          for (std::set<size_t>::iterator it = wouldSelect.begin(); it != wouldSelect.end(); ++it){
            if (!myTracks.count(*it)){inhibited = true;}
          }
          if (inhibited) {
            skipReasons[key] = "track_inhibit '" + tmp["track_inhibit"].asString() + "' matches tracks from another producer";
            continue;
          }
        }
      }
      // Mark process as should-be-active
      newProcs.insert(key);
    }

    // shut down deleted/changed processes
    if (runningProcs.size()){
      for (std::map<std::string, pid_t>::iterator it = runningProcs.begin(); it != runningProcs.end();) {
        if (!newProcs.count(it->first)) {
          const std::string processConfig = it->first;
          const pid_t processPid = it->second;
          std::string stopReason = "no longer in process configuration";
          {
            std::map<std::string, std::string>::iterator sr = skipReasons.find(processConfig);
            if (sr != skipReasons.end()) { stopReason = sr->second; }
          }
          std::string procType = JSON::fromString(processConfig)["process"].asString();
          processPidsWithUsers.erase(processPid);
          if (Util::Procs::isActive(processPid)) {
            INFO_MSG("Stopping process %s (PID %d): %s", procType.c_str(), processPid, stopReason.c_str());
            Util::Procs::ignoreExitCode(processPid);
            Util::Procs::Stop(processPid);
          } else {
            int exitCode = 0;
            Util::Procs::getExitCode(processPid, exitCode);
          }
          // Tell trigger consumers this was a deliberate supervisor stop, not a
          // process failure. Without it a sidecar only sees missing output and
          // misattributes the stop to the process itself.
          if (Triggers::shouldTrigger("PROCESS_EXIT", streamName)) {
            std::string payload = processExitTriggerPayload(streamName, procType, processConfig, processPid, 0,
                                                            procBoots[processConfig], "stopped", stopReason, stopReason);
            Triggers::doTrigger("PROCESS_EXIT", payload, streamName);
          }
          // Clean up SHM state page for this process
          {
            char shmName[NAME_BUFFER_SIZE];
            snprintf(shmName, NAME_BUFFER_SIZE, SHM_PROC_STATE, processPid);
            IPC::sharedPage sp;
            sp.init(shmName, 0, false, false);
            if (sp) { sp.master = true; }
          }
          // If we stop a process this way, reset it's counter and delayed start time
          procBoots.erase(processConfig);
          procNextBoot.erase(processConfig);
          procHardFailed.erase(processConfig);
          it = runningProcs.erase(it);
        } else {
          ++it;
        }
      }
    }

    // Clean up procHardFailed entries for configs no longer in the process list
    // (prevents sticky suppression when a config is removed and re-added)
    for (auto hfIt = procHardFailed.begin(); hfIt != procHardFailed.end();) {
      if (!newProcs.count(*hfIt)) {
        hfIt = procHardFailed.erase(hfIt);
      } else {
        ++hfIt;
      }
    }

    std::string debugLvl;
    // start up new/changed connectors
    for (const std::string & config : newProcs) {
      if (runningProcs.count(config) && Util::Procs::isActive(runningProcs[config])) { continue; }
      JSON::Value args(PARSEJSON, config);

      // Skip if this process previously hard-failed (config change clears this).
      if (procHardFailed.count(config)) { continue; }

      // If the process was running but is now dead, collect its result before restarting it.
      if (runningProcs.count(config)) {
        pid_t deadPid = runningProcs[config];
        int exitCode = 0;
        if (!Util::Procs::getExitCode(deadPid, exitCode)) { continue; }

        std::string shortReason;
        std::string longReason;
        {
          char shmName[NAME_BUFFER_SIZE];
          snprintf(shmName, NAME_BUFFER_SIZE, SHM_PROC_STATE, deadPid);
          IPC::sharedPage sp;
          sp.init(shmName, 0, false, false);
          ProcState state;
          if (ProcState::readSnapshot(sp, state)) {
            if (state.shortReason[0]) { shortReason = state.shortReason; }
            if (state.longReason[0]) { longReason = state.longReason; }
          }
          if (sp) { sp.master = true; }
        }

        std::string configuredRestartType = "fixed";
        if (args.isMember("restart_type")) { configuredRestartType = args["restart_type"].asString(); }
        const std::string status = processExitStatus(exitCode, configuredRestartType, procBoots[config]);

        std::string procType = args["process"].asString();
        if (Triggers::shouldTrigger("PROCESS_EXIT", streamName)) {
          std::string payload = processExitTriggerPayload(streamName, procType, config, deadPid, exitCode,
                                                          procBoots[config], status, shortReason, longReason);
          Triggers::doTrigger("PROCESS_EXIT", payload, streamName);
        }

        if (status == "unrecoverable") {
          WARN_MSG("Process `%s` (PID %d) exited with unrecoverable error (code %d: %s), disabling restart",
                   procType.c_str(), deadPid, exitCode, longReason.c_str());
          procHardFailed.insert(config);
          processPidsWithUsers.erase(deadPid);
          runningProcs.erase(config);
          continue;
        }
        processPidsWithUsers.erase(deadPid);
        runningProcs.erase(config);
      }

      // Check restart behaviour - default to instant (re)starts
      std::string restartType = "fixed";
      uint64_t restartDelay = 0;
      if (args.isMember("restart_type")) { restartType = args["restart_type"].asString(); }
      if (args.isMember("restart_delay")) { restartDelay = args["restart_delay"].asInt(); }

      // Skip if restarts are disabled and this buffer has already booted an instance of this process
      if (restartType == "disabled" && procBoots[config]) {
        VERYHIGH_MSG("Skipping process `%s`, as restarts are disabled", args["process"].asString().c_str());
        continue;
      }
      // Apply any delayed start time if we've booted before
      if (restartDelay && procBoots[config] && !procNextBoot[config]) {
        if (restartType == "fixed") {
          procNextBoot[config] = now + restartDelay;
        } else if (restartType == "backoff") {
          uint64_t thisTries = procBoots[config];
          if (thisTries > 10) { thisTries = 10; }
          procNextBoot[config] = now + Util::expBackoffMs(thisTries, 10, restartDelay);
        }
      }
      // Skip if we have a delayed start time
      if (procNextBoot[config] > now) {
        VERYHIGH_MSG("Delaying start of process `%s`, %" PRIu64 " ms remaining", args["process"].asString().c_str(),
                     procNextBoot[config] - now);
        continue;
      }

      // Do not restart processors into a stream that is already draining.
      const uint8_t procStreamState = Util::getStreamStatus(streamName);
      const bool sourceEof = processControlledRealtime && streamStatus &&
        streamStatus.len > STRMSTATE_PROCESS_SOURCE_EOF_OFFSET && streamStatus.mapped[STRMSTATE_PROCESS_SOURCE_EOF_OFFSET];
      if (!processSupervisorMayStart(Util::Config::is_active, procStreamState, sourceEof)) {
        VERYHIGH_MSG("Not starting process `%s`: stream is shutting down", args["process"].asString().c_str());
        continue;
      }

      std::string procname = Util::getMyPath() + "MistProc" + args["process"].asString();
      std::deque<std::string> argarr;
      argarr.push_back(procname);
      argarr.push_back(config);
      if (Util::printDebugLevel != DEBUG || args.isMember("debug")) {
        argarr.push_back("--debug");
        if (args.isMember("debug")) {
          argarr.push_back(args["debug"].asString());
        } else {
          argarr.push_back(std::to_string(Util::printDebugLevel));
        }
      }
      // Only count process as not-running if it's not inconsequential
      if (!args.isMember("inconsequential") || !args["inconsequential"].asBool()) { allProcsRunning = false; }
      runningProcs[config] = Util::Procs::StartPiped(argarr, 0, 0, &err);
      processPidsWithUsers.erase(runningProcs[config]);
      INFO_MSG("Started process %zu: %s %s", (size_t)runningProcs[config], argarr[0].c_str(), argarr[1].c_str());
      // Increment per-process boot counter
      procBoots[config]++;
      // Remove the delayed start counter
      procNextBoot.erase(config);
    }
  }
  /*LTS-END*/

}// namespace Mist

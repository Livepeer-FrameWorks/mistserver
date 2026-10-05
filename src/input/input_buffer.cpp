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
    publisherSessionEnded = false;
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
    uint64_t sourceFragCount = 0xFFFFull;
    bool hasSourceMedia = false;
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
      const uint64_t trackFrags = M.hasEmbeddedFrames(i) ? FRAG_BOOT : DTSC::Fragments(M.fragments(i)).getEndValid();
      if (trackFrags < fragCount) { fragCount = trackFrags; }
      if (!bufferTrackIsDerived(M.getSourceTrack(i))) {
        hasSourceMedia = true;
        if (trackFrags < sourceFragCount) { sourceFragCount = trackFrags; }
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
    fragCount = bufferReadinessFragments(sourceFragCount, fragCount, hasSourceMedia);
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
      // A process-controlled recorder still needs this key: the feed waits for it instead.
      uint64_t minHeldKey = 0;
      bool held = processControlledRealtime && bufferHolds.held(tid, minHeldKey);
      if (!keyRemovalAllowed(keys.getFirstValid(), held, minHeldKey)) { return false; }
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
    // A removed track's index can be reused by the next session's tracks.
    retainedSourceTracks.erase(tid);
    processTrackProducers.erase(tid);
    producerHoldUntil.erase(tid);
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

  /// Removes process outputs that were replaced explicitly: a producer whose output changed
  /// (another init or video size after a restart) registered a new track under the output key the
  /// old track carries, and left the old one unclaimed. The old track goes at once rather than
  /// lingering until it idles out.
  void InputBuffer::retireReplacedOutputs() {
    const std::set<size_t> validTracks = M.getValidTracks();
    std::map<std::string, size_t> newest;
    for (const size_t track : validTracks) {
      const std::string key = M.getOutputKey(track);
      if (key.empty()) { continue; }
      if (!newest.count(key) || track > newest[key]) { newest[key] = track; }
    }
    for (const size_t track : validTracks) {
      const std::string key = M.getOutputKey(track);
      if (key.empty() || newest[key] == track) { continue; }
      if (!bufferRetiresReplacedOutput(M.isClaimed(track), M.isClaimed(newest[key]))) { continue; }
      WARN_MSG("Removing track %zu: replaced by track %zu (output %s)", track, newest[key], key.c_str());
      meta.reloadReplacedPagesIfNeeded();
      removeTrack(track);
    }
  }

  /// Decides which tracks without a producer the buffer keeps for a returning producer, and
  /// publishes until when (boot ms) per track, so bounded reads wait for the producer instead of
  /// ending the track. A process output is kept while its configured process is restarted, up to
  /// its resume deadline (producerResumeDeadline); once that process is retired it is not kept and
  /// the idle timeout applies. A publisher's track kept for resume is held until the idle timeout
  /// would remove it.
  void InputBuffer::updateResumeHolds() {
    const uint64_t now = Util::bootMS();
    const std::set<size_t> validTracks = M.getValidTracks();
    for (std::map<size_t, uint64_t>::iterator it = producerHoldUntil.begin(); it != producerHoldUntil.end();) {
      if (validTracks.count(it->first)) {
        ++it;
      } else {
        it = producerHoldUntil.erase(it);
      }
    }
    for (const size_t track : validTracks) {
      uint64_t resumeUntil = 0;
      const bool producerAlive = M.isClaimed(track) && Util::Procs::isActive((pid_t)M.isClaimedBy(track));
      const std::string key = M.getOutputKey(track);
      if (key.size()) {
        std::map<std::string, std::string>::const_iterator producer =
          configuredProcessIdentities.find(DTSC::outputKeyIdentity(key));
        // Only a process that will be started again comes back for its outputs: not a retired
        // one, not one configured without restarts, and none in a stream whose source ended.
        bool restarts = false;
        uint64_t nextStart = 0;
        if (producer != configuredProcessIdentities.end()) {
          const JSON::Value config = JSON::fromString(producer->second);
          restarts = !processingProcessRetired(config) && config["restart_type"].asString() != "disabled" &&
            !(everHadPush && !hasPush);
          std::map<std::string, uint64_t>::const_iterator nextBoot = procNextBoot.find(producer->second);
          if (nextBoot != procNextBoot.end()) { nextStart = nextBoot->second; }
        }
        if (!restarts) {
          if (producerHoldUntil.erase(track)) {
            INFO_MSG("Track %zu (output %s) is no longer kept: its process will not be restarted", track, key.c_str());
          }
        } else if (producerAlive) {
          // Published while the producer runs, so a reader that sees it go also sees the hold.
          producerHoldUntil.erase(track);
          resumeUntil = producerResumeDeadline(now, nextStart);
        } else {
          if (!producerHoldUntil.count(track)) {
            producerHoldUntil[track] = producerResumeDeadline(now, nextStart);
            INFO_MSG("Keeping track %zu (output %s) for its restarting process for up to %" PRIu64 " ms", track,
                     key.c_str(), producerHoldUntil[track] - now);
          }
          if (trackHeldForProducer(true, producerHoldUntil[track], now)) { resumeUntil = producerHoldUntil[track]; }
        }
      } else if (resumeMode && !processControlledRealtime && !bufferTrackIsDerived(M.getSourceTrack(track))) {
        // Published while the publisher is connected too, so a reader that sees it leave also
        // sees the hold: until the idle timeout would remove the track.
        resumeUntil = (producerAlive ? now : M.getLastUpdated(track) * 1000) + idleTime;
      }
      if (M.getResumeUntil(track) != resumeUntil) { meta.setResumeUntil(track, resumeUntil); }
    }
  }

  void InputBuffer::removeUnused(){
    meta.reloadReplacedPagesIfNeeded();
    if (!meta){
      return;
    }
    retireReplacedOutputs();
    updateResumeHolds();
    // first remove all tracks that have not been updated for too long
    const bool idleEraseAllowed = bufferIdleTrackEraseAllowed(processControlledRealtime, hasProcessDrainConsumers());
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
        if (!idleEraseAllowed) { continue; }
        // Kept for its restarting producer: the restarted process continues it.
        std::map<size_t, uint64_t>::const_iterator hold = producerHoldUntil.find(i);
        if (hold != producerHoldUntil.end() && trackHeldForProducer(true, hold->second, Util::bootMS())) { continue; }
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
        const uint64_t pageFirstKey = tPages.getInt(firstKeyEnt, j);
        // Publishers expose a page before its first packet increments keycount.
        if (pageFirstKey >= firstKey || pageFirstKey + tPages.getInt(keyCount, j) > firstKey) { break; }
        bufferRemove(i, pageFirstKey, j);
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

    // Without a consequential process nothing votes on the feed rate, and
    // nothing needs the source paced: ramp toward the operator cap (or the
    // unconstrained ceiling) under node pressure instead of pinning 1x.
    bool unconstrained = !requiredCount;
    if (unconstrained) { targetSpeed = operatorCap ? operatorCap : PROCESSING_UNCONSTRAINED_SPEED; }
    if (!targetSpeed) { targetSpeed = 1; }
    if (operatorCap) { targetSpeed = std::min(targetSpeed, operatorCap); }
    if (!allContractsReady) { targetSpeed = 1; }

    bool nodeHold = false, nodeSlow = false;
    if (anyCpuPrimary || unconstrained) {
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
    bool allReady = unconstrained || (requiredCount && readyVoteCount >= requiredCount);
    ProcessingRateInput rateInput;
    // The first complete, unpressured contract set is the proc-authored
    // bootstrap seed, not a ramp destination. Before it arrives the feeder
    // runs at its normal 1x fallback. This avoids losing several seconds to
    // 1->2->3->... merely because InputBuffer observed the BOOTING page first.
    // An unconstrained stream has no proc-authored seed; it ramps up from 1x.
    bool applyStartupSeed = !startupSeedApplied && !unconstrained && allContractsReady && !anyHardSlow &&
      !anyRegularSlow && !nodeHold && !nodeSlow;
    rateInput.current = applyStartupSeed ? 0 : (unconstrained && !effectiveSpeed ? 1 : effectiveSpeed);
    rateInput.target = targetSpeed;
    rateInput.hardSlow = anyHardSlow;
    rateInput.regularSlow = anyRegularSlow;
    rateInput.nodeSlow = nodeSlow;
    rateInput.nodeHold = nodeHold;
    rateInput.consumerHold = consumerLag.held();
    rateInput.freshVoteRound = (sawFresh || unconstrained) && allReady;
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
               "x, ready=%zu/%zu, hard=%d, slow=%d, nodeHold=%d, consumerHold=%d, lockout=%u)",
               previous, effectiveSpeed, targetSpeed, readyVoteCount, requiredCount, anyHardSlow,
               anyRegularSlow || nodeSlow, nodeHold, consumerLag.held(), rampLockoutTicks);
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
    bool processExited = false;
    for (std::map<std::string, pid_t>::iterator it = runningProcs.begin(); it != runningProcs.end(); ++it) {
      if (it->second && !Util::Procs::isActive(it->second)) {
        processExited = true;
        break;
      }
    }
    bool restartDue = false;
    for (std::map<std::string, uint64_t>::iterator it = procNextBoot.begin(); it != procNextBoot.end(); ++it) {
      if (it->second && it->second <= Util::bootMS() && it->second > lastProcTime) {
        restartDue = true;
        break;
      }
    }
    if (processSupervisorCheckDue(Util::bootMS(), lastProcTime, procInterval, processExited, restartDue)) {
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
        if (processReplacements.size()) { configuredProcesses = applyProcessReplacements(configuredProcesses); }
        /*LTS-END*/
        processControlledRealtime = streamCfg.getMember("process_controlled_realtime").asBool();
        checkProcesses(configuredProcesses);
        releaseRetiredReservations();
        // Published after checkProcesses so retired processes (hard-failed or restart-disabled) drop
        // out on the same tick, and replacements it scheduled are expected before they start.
        if (processControlledRealtime) {
          publishProcessGraph(resolveProcessGraph(applyProcessReplacements(configuredProcesses)));
        }
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
    bufferHolds.beginScan();

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
      // A record first seen as a process's stays one: an exited process is collected (and
      // leaves generatePids) a tick before its records are disconnected.
      bool isProcess = generatePids.count(users.getPid(id)) || processUsers.count(id);
      if (isProcess) {
        processUsers[id] = users.getTrack(id);
        processPidsWithUsers.insert(users.getPid(id));
        processTrackProducers[users.getTrack(id)] = users.getPid(id);
      } else {
        const size_t newTrack = users.getTrack(id);
        if (!sourceUsers.count(id) && publisherSessionEnded) {
          // The override answered for the previous session; a failed trigger
          // keeps it, since only an answer replaces processOverride.
          INFO_MSG("New publisher session; re-resolving stream processes");
          publisherSessionEnded = false;
          processOverrideResolved = false;
          procStopped.clear();
        }
        if (!sourceUsers.count(id) && retainedSourceTracks.size()) {
          // A new publisher session: a retained track it did not resume is stale.
          retainedSourceTracks.erase(newTrack);
          const std::string newType = M.getType(newTrack);
          const std::set<size_t> retainedTracks = retainedSourceTracks;
          for (const size_t retained : retainedTracks) {
            if (!M.trackValid(retained)) {
              retainedSourceTracks.erase(retained);
              continue;
            }
            if (!dropRetainedSourceTrack(retained, M.getType(retained), newTrack, newType)) { continue; }
            INFO_MSG("Removing track %zu retained from the previous publisher session", retained);
            meta.reloadReplacedPagesIfNeeded();
            removeTrack(retained);
          }
        }
        sourceUsers[id] = newTrack;
      }
      // A disconnecting publisher is gone: this scan handles its disconnect right after.
      const bool disconnecting = users.getStatus(id) & COMM_STATUS_DISCONNECT;
      // GeneratePids holds the pids of the process that generate data, so ignore those for determining if a push is ingested.
      if (!isProcess && !disconnecting && M.trackValid(users.getTrack(id))) { hasPush = true; }
    }

    if (!(users.getStatus(id) & COMM_STATUS_DONOTTRACK)) {
      ++connectedUsers;
      if (!(users.getStatus(id) & COMM_STATUS_SOURCE)) { ++drainConsumerUsers; }
    }
    if (processControlledRealtime && (users.getStatus(id) & COMM_STATUS_HOLDBUFFER) && !(users.getStatus(id) & COMM_STATUS_SOURCE)) {
      bufferHolds.observe(id, users.getPid(id), users.getTrack(id), users.getKeyNum(id), Util::bootMS());
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
        if (retainedSourceTrackGoesStale(processControlledRealtime, M.getCodec(sourceUsers[id]) == "rawhls")) {
          retainedSourceTracks.insert(sourceUsers[id]);
        }
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
      if (publisherLeftEndsProcessSession(processControlledRealtime, sourceUsers.size())) {
        publisherSessionEnded = true;
      }
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

  // A retired process will never (re)produce output tracks: it exited
  // unrecoverably (checkProcesses disabled its restart) or has restarts
  // disabled and its only boot already ended. Retired processes drop out of
  // the outputs the processing graph expects, so recordings waiting on them
  // unblock instead of waiting for tracks that will never come
  // (e.g. a fallback to source passthrough). Uses the same config-key
  // construction as checkProcesses so the lookups match.
  bool InputBuffer::processingProcessRetired(const JSON::Value & proc) const {
    JSON::Value tmp = proc;
    tmp["source"] = streamName;
    const std::string key = tmp.toString();
    if (procHardFailed.count(key) || procStopped.count(key)) { return true; }
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

  /// Whether a configured process will (re)produce its outputs: it is not retired, and it did not
  /// already finish after the source ended (Thumbs completing its VOD sheet), since what it made
  /// is in the stream and it has nothing left to produce.
  bool InputBuffer::processWillProduce(const JSON::Value & proc) const {
    if (processingProcessRetired(proc)) { return false; }
    JSON::Value keyed = proc;
    keyed["source"] = streamName;
    const std::string key = keyed.toString();
    const auto running = runningProcs.find(key);
    const bool producerRunning = running != runningProcs.end() && running->second && Util::Procs::isActive(running->second);
    const auto boots = procBoots.find(key);
    return producerRunning || !everHadPush || hasPush || boots == procBoots.end() || !boots->second;
  }

  /// What a process declares for its configuration (see src/process/process_outputs.h), asked once
  /// per configuration. A process that gives no declaration is supervised on its configured track
  /// selection alone and adds no expected outputs.
  JSON::Value InputBuffer::processDeclaration(const std::string & config, const JSON::Value & args) {
    std::map<std::string, JSON::Value>::const_iterator known = processDeclarations.find(config);
    if (known != processDeclarations.end()) { return known->second; }
    std::deque<std::string> argarr;
    argarr.push_back(Util::getMyPath() + "MistProc" + args["process"].asString());
    argarr.push_back("--describe-outputs");
    argarr.push_back(config);
    JSON::Value declaration = JSON::fromString(Util::Procs::getOutputOf(argarr, 5000));
    if (!declaration.isObject()) {
      WARN_MSG("Process `%s` does not describe its outputs; it starts on its configured track selection and no "
               "recording waits for its tracks",
               args["process"].asString().c_str());
      declaration.null();
    }
    processDeclarations[config] = declaration;
    return declaration;
  }

  /// The processing graph of the given process list (see src/process_graph.h), rebuilt only when
  /// what it is built from changed.
  const ProcessGraph & InputBuffer::resolveProcessGraph(const JSON::Value & procs) {
    std::vector<ProcessGraphNode> nodes;
    const std::set<std::string> tags = Util::streamTags(streamName);
    std::string inputs = procs.toString() + "\n";
    for (const std::string & tag : tags) { inputs += tag + ","; }
    inputs += "\n";
    jsonForEachConst (procs, it) {
      if (!it->isObject() || !(*it)["process"].isString()) { continue; }
      ProcessGraphNode node;
      node.proc = *it;
      node.proc["source"] = streamName;
      node.config = node.proc.toString();
      node.declaration = processDeclaration(node.config, node.proc);
      node.inhibited = processInhibitReason(node.proc, M, tags);
      if (node.proc["sink"].isString() && node.proc["sink"].asStringRef().size()) {
        std::string sink = node.proc["sink"].asStringRef();
        Util::streamVariables(sink, streamName);
        node.producesHere = sink == streamName;
      }
      node.producing = processWillProduce(*it);
      inputs += node.producing ? "1" : "0";
      nodes.push_back(node);
    }
    inputs += "\n";
    const uint8_t oldMask = DTSC::trackValidMask;
    DTSC::trackValidMask = TRACK_VALID_ALL;
    const std::set<size_t> withData = M.getValidTracks(true);
    for (const size_t track : M.getValidTracks()) {
      inputs += std::to_string(track) + ":" + M.getType(track) + ":" + M.getCodec(track) + ":" +
        std::to_string(M.trackValid(track)) + ":" + std::to_string(M.getSourceTrack(track)) + ":" +
        M.getOutputKey(track) + (withData.count(track) ? ":data," : ",");
    }
    DTSC::trackValidMask = oldMask;
    if (inputs == processGraphInputs) { return processGraph; }
    processGraphInputs = inputs;
    processGraphNodes.clear();
    for (const ProcessGraphNode & node : nodes) { processGraphNodes[node.config] = node; }
    processGraph = buildProcessGraph(M, nodes);
    return processGraph;
  }

  /// Publishes the outputs the processing graph expects for recordings (see src/process_graph.h),
  /// resolved once the stream has its source tracks.
  void InputBuffer::publishProcessGraph(const ProcessGraph & graph) {
    if (!processGraphPage) {
      char pageName[NAME_BUFFER_SIZE];
      snprintf(pageName, sizeof(pageName), SHM_STREAM_PGRAPH, streamName.c_str());
      processGraphPage.init(pageName, PROCESS_GRAPH_PAGE_LEN, true, false);
      if (!processGraphPage) { return; }
    }
    JSON::Value published;
    published["resolved"] = M.getValidTracks().size() != 0;
    published["outputs"] = graph.outputs;
    const std::string json = published.toString();
    if (json == publishedProcessGraph) { return; }
    if (!writeProcessGraphPage(processGraphPage, json)) {
      WARN_MSG("The processing graph (%zu bytes) does not fit its page; recordings wait for no process output", json.size());
      published["outputs"].shrink(0);
      writeProcessGraphPage(processGraphPage, published.toString());
    }
    publishedProcessGraph = json;
  }

  void InputBuffer::publishFeedPaused() {
    if (!streamStatus || streamStatus.len < 16) { return; }
    streamStatus.mapped[STRMSTATE_PROCESS_FEED_PAUSED_OFFSET] = processControlledRealtime && consumerLag.held();
  }

  /// How far the source leads the slowest HOLDBUFFER recorder (see
  /// recorderLeadMs for how one recorder's tracks combine). targetDurationMs
  /// receives the largest target duration among the held tracks (the biggest
  /// fragment, rounded up to whole seconds).
  uint64_t InputBuffer::holdingReaderLeadMs(uint64_t & targetDurationMs) const {
    targetDurationMs = 0;
    std::map<uint64_t, std::map<size_t, uint64_t>> recorders;
    const std::vector<BufferHoldTracker::Position> & positions = bufferHolds.positions();
    for (const BufferHoldTracker::Position & p : positions) {
      if (!M.trackValid(p.track)) { continue; }
      uint64_t lastMs = M.getLastms(p.track);
      uint64_t readerMs = lastMs;
      if (!BufferHoldTracker::atLivePoint(p.keyNum)) {
        DTSC::Keys keys(M.keys(p.track));
        if (!keys.getValidCount()) { continue; }
        if (p.keyNum <= keys.getFirstValid()) {
          readerMs = keys.getTime(keys.getFirstValid());
        } else if (p.keyNum < keys.getEndValid()) {
          readerMs = keys.getTime(p.keyNum);
        }
        uint64_t target = (M.biggestFragment(p.track) / 1000 + 1) * 1000;
        if (target > targetDurationMs) { targetDurationMs = target; }
      }
      uint64_t trackLead = lastMs > readerMs ? lastMs - readerMs : 0;
      std::map<size_t, uint64_t> & leads = recorders[p.reader];
      if (!leads.count(p.track) || trackLead < leads[p.track]) { leads[p.track] = trackLead; }
    }
    uint64_t lead = 0;
    for (std::map<uint64_t, std::map<size_t, uint64_t>>::const_iterator it = recorders.begin(); it != recorders.end(); ++it) {
      uint64_t recorderLead = recorderLeadMs(it->second);
      if (recorderLead > lead) { lead = recorderLead; }
    }
    return lead;
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

    bufferHolds.endScan(Util::bootMS());
    if (processControlledRealtime) {
      uint64_t targetDurationMs = 0;
      uint64_t lead = holdingReaderLeadMs(targetDurationMs);
      bool wasHeld = consumerLag.held();
      uint64_t threshold = bufferHolds.heldTracks().size() ? consumerHoldThreshold(bufferTime, targetDurationMs) : 0;
      if (consumerLag.update(lead, threshold) != wasHeld) {
        INFO_MSG("Processing feed %s: source leads the slowest recorder by %" PRIu64 "ms (hold at %" PRIu64 "ms)",
                 wasHeld ? "resumed" : "paused", lead, threshold);
      }
    } else {
      consumerLag.reset();
    }
    publishFeedPaused();

    ProcessingSourceEofAction eofAction = processingSourceEofAction(config->is_active, hasPush, everHadPush, resumeMode,
                                                                    processControlledRealtime, hasProcessDrainConsumers());
    // After source EOF no data arrives any more, so a recorder still playing out the buffer is
    // its only activity. A recorder that stopped progressing for the stale window no longer counts.
    if (processingReaderKeepsBufferAlive(processControlledRealtime, eofAction, !bufferHolds.positions().empty())) {
      activityCounter = Util::bootSecs();
    }
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
    const ProcessGraph & graph = resolveProcessGraph(procs);
    newProcs = graph.runs;
    std::map<std::string, std::string> skipReasons = graph.skipReasons;

    configuredProcessIdentities.clear();
    for (const std::string & config : newProcs) { configuredProcessIdentities[DTSC::processIdentity(config)] = config; }

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
          procStopped.erase(processConfig);
          it = runningProcs.erase(it);
        } else {
          ++it;
        }
      }
    }

    // Clean up procHardFailed entries for configs no longer in the process list
    // (prevents sticky suppression when a config is removed and re-added)
    // A replaced config left the effective list on purpose and stays hard-failed.
    for (auto hfIt = procHardFailed.begin(); hfIt != procHardFailed.end();) {
      if (!newProcs.count(*hfIt) && !processReplacements.count(*hfIt)) {
        hfIt = procHardFailed.erase(hfIt);
      } else {
        ++hfIt;
      }
    }
    for (auto stIt = procStopped.begin(); stIt != procStopped.end();) {
      if (!newProcs.count(*stIt)) {
        stIt = procStopped.erase(stIt);
      } else {
        ++stIt;
      }
    }

    std::string debugLvl;
    // start up new/changed connectors
    for (const std::string & config : newProcs) {
      if (runningProcs.count(config) && Util::Procs::isActive(runningProcs[config])) { continue; }
      JSON::Value args(PARSEJSON, config);

      // Skip if this process previously hard-failed or was stopped (config change clears this).
      if (procHardFailed.count(config) || procStopped.count(config)) { continue; }

      // A process reads what it selects once that exists: one fed by another process's output
      // starts once that output carries data.
      if (!runningProcs.count(config) && !nodeSelectsExistingTracks(M, processGraphNodes[config])) {
        VERYHIGH_MSG("Process `%s` waits for its input tracks", args["process"].asString().c_str());
        continue;
      }

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
        const std::string status = processExitStatus(exitCode, configuredRestartType, procBoots[config], shortReason);

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
          replaceFailedProcess(config, procType, exitCode, shortReason, longReason);
          continue;
        }
        processPidsWithUsers.erase(deadPid);
        runningProcs.erase(config);
        if (status == "stopped") {
          INFO_MSG("Process `%s` (PID %d) was stopped (%s); it starts again with a new publisher session",
                   procType.c_str(), deadPid, longReason.c_str());
          procStopped.insert(config);
          continue;
        }
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

      // Do not restart processors into a stream that is already draining, or whose publisher left:
      // hasPush still holds the previous tick's state here.
      const uint8_t procStreamState = Util::getStreamStatus(streamName);
      const bool sourceEofFlag = streamStatus && streamStatus.len > STRMSTATE_PROCESS_SOURCE_EOF_OFFSET &&
        streamStatus.mapped[STRMSTATE_PROCESS_SOURCE_EOF_OFFSET];
      const bool sourceEof = processSourceEnded(processControlledRealtime, sourceEofFlag, everHadPush, hasPush);
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
      if (processControlledRealtime) { reserveProcessOutputs(config, args); }
      runningProcs[config] = Util::Procs::StartPiped(argarr, 0, 0, &err);
      processPidsWithUsers.erase(runningProcs[config]);
      INFO_MSG("Started process %zu: %s %s", (size_t)runningProcs[config], argarr[0].c_str(), argarr[1].c_str());
      // Increment per-process boot counter
      procBoots[config]++;
      // Remove the delayed start counter
      procNextBoot.erase(config);
    }
  }

  /// Reserves a track for every output a process will produce into this processing stream, before
  /// starting it: Livepeer profiles (not inhibited by the source), AV encodes and the thumbnail
  /// tracks. The process claims each reservation by output key, so its track indexes are fixed
  /// before any data and a restarted run can only ever continue its own outputs.
  void InputBuffer::reserveProcessOutputs(const std::string & config, const JSON::Value & args) {
    if (args.isMember("sink") && args["sink"].isString() && args["sink"].asStringRef().size()) {
      std::string sink = args["sink"].asStringRef();
      Util::streamVariables(sink, streamName);
      if (sink != streamName) { return; }
    }
    std::vector<std::string> outputs = processReservableOutputs(args);
    if (args["process"].asString() == "Livepeer" && args["target_profiles"].isArray()) {
      jsonForEachConst (args["target_profiles"], prof) {
        if (!prof->isObject() || !(*prof)["name"].isString() || !(*prof)["name"].asStringRef().size()) { continue; }
        if (prof->isMember("track_inhibit") && Util::inhibitorMatchesSource(M, (*prof)["track_inhibit"].asStringRef())) {
          continue;
        }
        outputs.push_back((*prof)["name"].asStringRef());
      }
    }
    const std::string identity = DTSC::processIdentity(config);
    for (const std::string & output : outputs) { meta.reserveOutputTrack(DTSC::outputKey(identity, output)); }
  }

  /// Releases the reservations of processes that will not produce them any more: removed from the
  /// configuration, replaced, hard-failed, or done with restarts disabled.
  void InputBuffer::releaseRetiredReservations() {
    for (const size_t track : M.getReservedTracks()) {
      std::map<std::string, std::string>::const_iterator producer =
        configuredProcessIdentities.find(DTSC::outputKeyIdentity(M.getOutputKey(track)));
      if (producer != configuredProcessIdentities.end() && !processingProcessRetired(JSON::fromString(producer->second))) {
        continue;
      }
      meta.releaseReservedTrack(track);
    }
  }

  /// Returns procs with every replaced config swapped for its PROCESS_REPLACE replacements.
  /// Keys are built the same way checkProcesses builds them, so lookups match.
  JSON::Value InputBuffer::applyProcessReplacements(const JSON::Value & procs) const {
    if (!procs.isArray()) { return procs; }
    JSON::Value effective;
    effective.append(JSON::Value());
    effective.shrink(0);
    jsonForEachConst (procs, it) {
      JSON::Value keyed = *it;
      keyed["source"] = streamName;
      std::map<std::string, JSON::Value>::const_iterator replaced = processReplacements.find(keyed.toString());
      if (replaced == processReplacements.end()) {
        effective.append(*it);
        continue;
      }
      jsonForEachConst (replaced->second, rIt) { effective.append(*rIt); }
    }
    return effective;
  }

  /// Fires PROCESS_REPLACE for a config that just exited unrecoverably and, on a usable response,
  /// layers the replacement configs over the effective process list. The next checkProcesses pass
  /// starts and supervises them like any configured process.
  void InputBuffer::replaceFailedProcess(const std::string & config, const std::string & procType, int exitCode,
                                         const std::string & shortReason, const std::string & longReason) {
    if (replaceAttempted.count(config)) { return; }
    replaceAttempted.insert(config);
    std::string response;
    const std::string payload = processReplaceTriggerPayload(streamName, procType, config, exitCode, shortReason, longReason);
    if (!requestProcessReplacement(payload, response)) { return; }
    JSON::Value replacements = processReplacementConfigs(response);
    if (!replacements.size()) {
      WARN_MSG("PROCESS_REPLACE returned no usable replacement for process `%s`; it stays disabled", procType.c_str());
      return;
    }
    jsonForEachConst (replacements, it) {
      JSON::Value keyed = *it;
      keyed["source"] = streamName;
      replaceAttempted.insert(keyed.toString());
    }
    WARN_MSG("Replacing failed process `%s` with %zu replacement process(es)", procType.c_str(), (size_t)replacements.size());
    processReplacements[config] = replacements;
  }

  bool InputBuffer::requestProcessReplacement(const std::string & payload, std::string & response) {
    if (!Triggers::shouldTrigger("PROCESS_REPLACE", streamName)) { return false; }
    Triggers::Result result;
    Triggers::doTrigger("PROCESS_REPLACE", payload, streamName, false, result);
    if (result.handlerFailed || result.action != Triggers::ACT_VALUE || !result.response.size()) { return false; }
    response = result.response;
    return true;
  }
  /*LTS-END*/

}// namespace Mist

/// \page triggers Triggers
/// \brief Listing of all available triggers and their payloads.
/// MistServer reports certain occurances as configurable triggers to a URL or executable. This page
/// describes the triggers system in full.
///
/// Triggers are the preferred way of responding to server events. Each trigger has a name and a
/// payload, and may be stream-specific or global.
///
/// Triggers may be handled by a URL or an executable. If the handler contains ://, a HTTP URL is
/// assumed. Otherwise, an executable is assumed. If handled by an URL, a POST request is sent to
/// the URL with an extra X-Trigger header containing the trigger name and the payload as the POST
/// body. If handled by an executable, it's started with the trigger name as its only argument, and
/// the payload is piped into the executable over standard input.
///
/// Triggers may be asynchronous notifications or synchronous decisions. Synchronous HTTP handlers
/// may return an X-Mist-Trigger-Action header to distinguish a value, denial, retained caller value,
/// offline source, or configured fallback. Asynchronous trigger responses are not read.
///

#include "triggers.h"

#include "bitfields.h" //for strToBool
#include "defines.h" //for FAIL_MSG and INFO_MSG
#include "downloader.h" //for sending http request
#include "ev.h"
#include "json.h"
#include "procs.h" //for StartPiped
#include "shared_memory.h"
#include "stream.h"
#include "timing.h"
#include "util.h"

#include <algorithm>
#include <cctype>

namespace Triggers{

  Action actionFromString(const std::string & value) {
    std::string normalized = value;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), ::tolower);
    if (normalized == "value") { return ACT_VALUE; }
    if (normalized == "deny") { return ACT_DENY; }
    if (normalized == "keep") { return ACT_KEEP; }
    if (normalized == "offline") { return ACT_OFFLINE; }
    if (normalized == "use-configured" || normalized == "configured") { return ACT_CONFIGURED; }
    return ACT_LEGACY;
  }

  const char *actionName(Action action) {
    switch (action) {
      case ACT_VALUE: return "value";
      case ACT_DENY: return "deny";
      case ACT_KEEP: return "keep";
      case ACT_OFFLINE: return "offline";
      case ACT_CONFIGURED: return "use-configured";
      default: return "legacy";
    }
  }

  bool actionAllowed(const std::string & trigger, Action action) {
    if (action == ACT_VALUE || action == ACT_LEGACY) { return true; }
    if (action == ACT_OFFLINE) { return trigger == "STREAM_SOURCE"; }
    if (action == ACT_CONFIGURED) { return trigger == "STREAM_SOURCE" || trigger == "STREAM_PROCESS"; }
    if (action == ACT_KEEP) {
      return trigger == "PUSH_REWRITE" || trigger == "PLAY_REWRITE" || trigger == "STREAM_SOURCE" ||
        trigger == "STREAM_PROCESS" || trigger == "PUSH_OUT_START";
    }
    if (action == ACT_DENY) {
      return trigger == "PUSH_REWRITE" || trigger == "PLAY_REWRITE" || trigger == "USER_NEW" ||
        trigger == "PUSH_OUT_START" || trigger == "CONN_OPEN" || trigger == "CONN_PLAY" || trigger == "STREAM_LOAD" ||
        trigger == "STREAM_READY" || trigger == "STREAM_UNLOAD" || trigger == "SYSTEM_START" || trigger == "SYSTEM_STOP";
    }
    return false;
  }

  static Result failedResult(const std::string & defaultResponse, Action onFail) {
    Result result;
    result.handlerFailed = true;
    result.reason = "trigger_unavailable";
    if (onFail == ACT_LEGACY) {
      result.action = ACT_VALUE;
      result.response = defaultResponse;
    } else {
      result.action = onFail;
    }
    return result;
  }

  static void submitTriggerStat(const std::string trigger, uint64_t millis, bool ok){
    JSON::Value j;
    j["trigger_stat"]["name"] = trigger;
    j["trigger_stat"]["ms"] = Util::bootMS() - millis;
    j["trigger_stat"]["ok"] = ok;
    Util::sendUDPApi(j);
  }

  ///\brief Handles a trigger by sending a payload to a destination.
  ///\param trigger Trigger event type.
  ///\param value Destination. This can be an (HTTP)URL, or an absolute path to a binary/script
  ///\param payload This data will be sent to the destionation URL/program
  ///\param sync If true, handler is executed blocking and uses the response data.
  ///\returns Typed response and transport status.
  Result handleTrigger(const std::string & trigger, const std::string & value, const std::string & payload, int sync,
                       const std::string & defaultResponse, Action onFail) {
    uint64_t tStartMs = Util::bootMS();
    if (!value.size()){
      INFO_MSG("Blank %s trigger, responding: %s", trigger.c_str(), defaultResponse.c_str());
      return failedResult(defaultResponse, onFail);
    }
    INFO_MSG("Executing %s trigger: %s (%s)", trigger.c_str(), value.c_str(), sync ? "blocking" : "asynchronous");
    if (value.substr(0, 7) == "http://" || value.substr(0, 8) == "https://"){// interpret as url
      HTTP::Downloader DL;
      DL.setHeader("X-Trigger", trigger);
      std::string iid = Util::getGlobalConfig("iid").asString();
      if (iid.size()){
        DL.setHeader("X-Instance", iid);
      }
      std::string hrn = Util::getGlobalConfig("hrn").asString();
      if (hrn.size()){
        DL.setHeader("X-Name", hrn);
      }
      DL.setHeader("X-PID", JSON::Value(getpid()).toString());
      DL.setHeader("X-Trigger-UUID", getenv("MIST_TUUID"));
      DL.setHeader("X-Trigger-UnixMillis", getenv("MIST_TIME"));
      DL.setHeader("Date", getenv("MIST_DATE"));
      DL.setHeader("Content-Type", "text/plain");
      HTTP::URL url(value);
      if (DL.post(url, payload, sync) && (!sync || DL.isOk())) {
        Result result;
        if (!sync) {
          submitTriggerStat(trigger, tStartMs, true);
          result.response = defaultResponse;
          return result;
        }
        result.response = DL.data();
        const std::string actionHeader = DL.getHeader("X-Mist-Trigger-Action");
        if (actionHeader.size()) {
          result.action = actionFromString(actionHeader);
          result.reason = DL.getHeader("X-Mist-Trigger-Reason");
          if (result.action == ACT_LEGACY || !actionAllowed(trigger, result.action)) {
            FAIL_MSG("Invalid action '%s' returned for %s trigger", actionHeader.c_str(), trigger.c_str());
            submitTriggerStat(trigger, tStartMs, false);
            return failedResult(defaultResponse, onFail);
          }
        } else if (trigger == "STREAM_SOURCE" && result.response.substr(0, 8) == "offline:") {
          result.action = ACT_OFFLINE;
          result.reason = result.response.substr(8);
        }
        submitTriggerStat(trigger, tStartMs, true);
        return result;
      }
      FAIL_MSG("Trigger failed to execute (%s), using default response: %s",
               DL.getStatusText().c_str(), defaultResponse.c_str());
      submitTriggerStat(trigger, tStartMs, false);
      return failedResult(defaultResponse, onFail);
    }else{// send payload to stdin of newly forked process
      int fdIn = -1;
      int fdOut = -1;

      char *argv[3];
      argv[0] = (char *)value.c_str();
      argv[1] = (char *)trigger.c_str();
      argv[2] = NULL;
      setenv("MIST_TRIGGER", trigger.c_str(), 1);
      setenv("MIST_TRIG_DEF", defaultResponse.c_str(), 1);
      setenv("MIST_PID", JSON::Value(getpid()).toString().c_str(), 1);
      std::string iid = Util::getGlobalConfig("iid").asString();
      if (iid.size()){
        setenv("MIST_INSTANCE", iid.c_str(), 1);
      }
      std::string hrn = Util::getGlobalConfig("hrn").asString();
      if (hrn.size()){
        setenv("MIST_NAME", hrn.c_str(), 1);
      }
      uint64_t startTime = Util::bootMS();
      pid_t myProc = Util::Procs::StartPiped(argv, &fdIn, &fdOut, 0); // start new process and return stdin file desc.
      unsetenv("MIST_TRIGGER");
      unsetenv("MIST_TRIG_DEF");
      unsetenv("MIST_PID");
      unsetenv("MIST_INSTANCE");
      unsetenv("MIST_NAME");
      if (fdIn == -1 || fdOut == -1 || !myProc) {
        FAIL_MSG("Could not execute trigger executable: %s", strerror(errno));
        submitTriggerStat(trigger, tStartMs, false);
        return failedResult(defaultResponse, onFail);
      }
      write(fdIn, payload.data(), payload.size());
      shutdown(fdIn, SHUT_RDWR);
      close(fdIn);

      if (sync){// if sync!=0 wait for response
        Socket::Connection readConn(-1, fdOut);
        readConn.setBlocking(false);
        Util::ResizeablePointer ret;
        Event::Loop ev;
        ev.addSocket(fdOut, [&](void *) {
          while (readConn.spool()) { readConn.Received().remove(ret, readConn.Received().bytes(0xFFFFFFFFull)); }
          if (!readConn) { ev.remove(fdOut); }
        }, 0);
        bool warned = false;
        while (readConn && startTime + 15000 > Util::bootMS()) {
          ev.await(500);
          uint64_t now = Util::bootMS();
          if (now >= startTime + 10000 && Util::Procs::childRunning(myProc)) {
            if (!warned) {
              FAIL_MSG("Trigger taking too long - killing process");
              warned = true;
            }
            if (now >= startTime + 14000) {
              Util::Procs::Murder(myProc);
            } else {
              Util::Procs::Stop(myProc);
            }
          }
        }
        if (Util::Procs::childRunning(myProc)) { Util::Procs::Murder(myProc); }
        if (warned && !ret.size()) {
          WARN_MSG("Using default trigger response: %s", defaultResponse.c_str());
          submitTriggerStat(trigger, tStartMs, false);
          return failedResult(defaultResponse, onFail);
        }
        submitTriggerStat(trigger, tStartMs, true);
        Result result;
        result.response = std::string(ret, ret.size());
        if (trigger == "STREAM_SOURCE" && result.response.substr(0, 8) == "offline:") {
          result.action = ACT_OFFLINE;
          result.reason = result.response.substr(8);
        }
        return result;
      }
      close(fdOut);
      submitTriggerStat(trigger, tStartMs, true);
      Result result;
      result.response = defaultResponse;
      return result;
    }
  }

  /// Returns true if a trigger of the specified type should be handled for a specified stream (or entire server)
  /// Calls doTrigger with dryRun set to true
  /// \param type Trigger event type.
  /// \param streamName the stream to be handled
  bool shouldTrigger(const std::string & type, const std::string & streamName, std::function<bool(const char *)> paramsCB) {
    std::string unused;
    return doTrigger(type, empty, streamName, true, unused, paramsCB);
  }

  /// Handles triggers for a specific trigger event type, with a payload, for a specified stream, and/or server-wide.
  /// Calls doTrigger with dryRun set to false
  /// \param type Trigger event type.
  /// \param payload Trigger type-specific data
  /// \param streamName The name of a stream.
  /// \returns Boolean, false if further processing should be aborted.
  bool doTrigger(const std::string & type, const std::string & payload, const std::string & streamName) {
    std::string unused;
    return doTrigger(type, payload, streamName, false, unused);
  }

  /// This function attempts to open and parse a shared memory page with the config for a trigger
  /// event type, in order to parse the triggers defined for that trigger event type. The function
  /// can be used for two separate purposes, determined by the value of dryRun:
  /// If this function is called with dryRun=true, returns true if at least one trigger would fire.
  /// This can be used to make sure a payload is only generated if needed.
  /// If this function is called with dryRun=false, handleTrigger is called for all configured triggers.
  /// In that case, the return value is determined by the result of the trigger(s) that were ran.
  /// \param type Trigger event type
  /// \param payload Trigger type-specific data
  /// \param streamName Name of a stream to check for stream-specific triggers
  /// \param dryRun determines the mode of operation for this function
  /// \param response Returns the last received response by reference
  /// \param paramsCB Callback function that gets the configured parameter for triggers that take one
  ///\returns Boolean, false if further processing should be aborted
  bool doTrigger(const std::string & type, const std::string & payload, const std::string & streamName, bool dryRun,
                 std::string & response, std::function<bool(const char *)> paramsCB) {
    Result result;
    result.response = response;
    bool ret = doTrigger(type, payload, streamName, dryRun, result, paramsCB);
    response = result.response;
    return ret;
  }

  bool doTrigger(const std::string & type, const std::string & payload, const std::string & streamName, bool dryRun,
                 Result & result, std::function<bool(const char *)> paramsCB) {
    // open SHM page for this type:
    char thisPageName[NAME_BUFFER_SIZE];
    snprintf(thisPageName, NAME_BUFFER_SIZE, SHM_TRIGGER, type.c_str());
    IPC::sharedPage typePage(thisPageName, 8 * 1024, false, false);
    if (!typePage.mapped){// page doesn't exist?
      VERYHIGH_MSG("No triggers for %s found", type.c_str());
      return false;
    }

    Util::RelAccX trigs(typePage.mapped, false);
    if (!trigs.isReady()){
      WARN_MSG("No triggers for %s: list not ready", type.c_str());
      return false;
    }

    {
      std::string uuid = Util::generateUUID();
      setenv("MIST_TUUID", uuid.c_str(), 1);
    }

    {
      uint64_t currTime = Util::unixMS();
      std::string time = JSON::Value(currTime).asString();
      setenv("MIST_TIME", time.c_str(), 1);
      std::string date = Util::getDateString(currTime / 1000);
      setenv("MIST_DATE", date.c_str(), 1);
    }

    bool retVal = true;

    for (uint32_t i = 0; i < trigs.getRCount(); ++i) {
      std::string uri = std::string(trigs.getPointer("url", i));
      uint8_t sync = trigs.getInt("sync", i);

      char *strPtr = trigs.getPointer("streams", i);
      size_t pLen = trigs.getSize("streams");

      // By default match if no stream name is set
      bool isHandled = !streamName.size();
      bool onlyNegative = true;
      for (size_t bPos = 0; bPos + 4 < pLen; bPos += *(unsigned int *)(strPtr + bPos) + 4) {
        size_t stringLen = *(unsigned int *)(strPtr + bPos);
        if (bPos + 4 + stringLen > pLen || !stringLen) { break; }
        const char *currPtr = strPtr + bPos + 4;

        // Check if the entry starts with an exclamation point
        bool negated = currPtr[0] == '!';
        if (negated) {
          // negative matching
          ++currPtr;
          --stringLen;
          negated = true;
          if (!stringLen) { break; }
        } else {
          onlyNegative = false;
        }

        if (Util::streamMatches(streamName, std::string(currPtr, stringLen))) {
          isHandled = !negated;
          onlyNegative = false; // Prevent catch-all matching since anything matched
          continue;
        }
      }
      // If all entries were negative, match unless rejected
      if (onlyNegative) { isHandled = true; }

      // Check callback if needed
      if (isHandled && paramsCB) { isHandled = paramsCB(trigs.getPointer("params", i)); }

      if (isHandled){
        VERYHIGH_MSG("%s trigger handled by %s", type.c_str(), uri.c_str());
        if (dryRun) {
          unsetenv("MIST_TUUID");
          unsetenv("MIST_TIME");
          unsetenv("MIST_DATE");
          return true;
        }
        std::string defaultResponse = trigs.getPointer("default", i);
        if (!defaultResponse.size()) { defaultResponse = "true"; }
        Action onFail = (Action)trigs.getInt("onfail", i);
        if (!actionAllowed(type, onFail) || onFail == ACT_VALUE) { onFail = ACT_LEGACY; }
        if (!sync) { onFail = ACT_LEGACY; }
        if (sync){
          const std::string priorResponse = result.response;
          Result current = handleTrigger(type, uri, payload, sync, defaultResponse, onFail);
          if (current.action == ACT_KEEP) {
            current.response = priorResponse;
          } else if (current.action != ACT_VALUE) {
            current.response.clear();
          }
          result = current;
          switch (current.action) {
            case ACT_DENY:
            case ACT_OFFLINE: retVal = false; break;
            case ACT_VALUE: retVal &= Util::stringToBool(current.response); break;
            default: break;
          }
          if (current.action == ACT_DENY) { break; }
        }else{
          Result unused = handleTrigger(type, uri, payload, sync, defaultResponse); // do it.
          retVal &= Util::stringToBool(unused.response);
        }
      }
    }

    unsetenv("MIST_TUUID");
    unsetenv("MIST_TIME");
    unsetenv("MIST_DATE");
    return !dryRun && retVal;
  }
}// namespace Triggers

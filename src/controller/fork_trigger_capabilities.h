#pragma once

#include <mist/json.h>

#include <initializer_list>

namespace Controller {
  inline void setTriggerActions(JSON::Value & trigger, std::initializer_list<const char *> actions) {
    trigger.removeMember("actions");
    for (std::initializer_list<const char *>::const_iterator it = actions.begin(); it != actions.end(); ++it) {
      trigger["actions"].append(*it);
    }
  }

  inline void addForkTriggerCapabilities(JSON::Value & triggers) {
    setTriggerActions(triggers["STREAM_SOURCE"], {"value", "offline", "use-configured", "keep"});
    setTriggerActions(triggers["PUSH_REWRITE"], {"value", "deny", "keep"});
    setTriggerActions(triggers["PUSH_OUT_START"], {"value", "deny", "keep"});
    setTriggerActions(triggers["PLAY_REWRITE"], {"value", "deny", "keep"});
    setTriggerActions(triggers["USER_NEW"], {"value", "deny"});

    JSON::Value & streamProcess = triggers["STREAM_PROCESS"];
    streamProcess["when"] = "When a stream's process list is first loaded, before processes start";
    streamProcess["stream_specific"] = true;
    streamProcess["payload"] = "stream name (string)";
    streamProcess["response"] = "when-blocking";
    streamProcess["response_action"] =
      "A non-empty response (JSON array of process objects) will override the configured "
      "processes for this stream instance. An empty response uses the default configured processes.";
    setTriggerActions(streamProcess, {"value", "use-configured", "keep"});

    JSON::Value & processExit = triggers["PROCESS_EXIT"];
    processExit["when"] =
      "When a process exits or the process supervisor deliberately stops it after a configuration change.";
    processExit["stream_specific"] = true;
    processExit["payload"] =
      "stream name (string)\nprocess type (string)\nprocess config (JSON string)\npid (integer)\nexit code "
      "(integer)\nboot count (integer)\nstatus (string: clean, retrying, disabled, unrecoverable, stopped)\n"
      "machine-readable exit reason (string)\nhuman-readable exit reason (string)";
    processExit["response"] = "ignored";
    processExit["response_action"] = "None.";

    triggers["RECORDING_END"]["payload"] =
      "stream name (string)\npush target (string)\nconnector / filetype (string)\nbytes recorded "
      "(integer)\nseconds spent recording (integer)\nunix time recording started (integer)\nunix "
      "time recording stopped (integer)\ntotal milliseconds of media data recorded "
      "(integer)\nmillisecond timestamp of first media packet (integer)\nmillisecond timestamp "
      "of last media packet (integer)\nmachine-readable reason for exit (string, enum)\nhuman-readable reason for exit "
      "(string)\nrecorded track and processing speed summary (JSON object, optional)";

    JSON::Value & recordingSegment = triggers["RECORDING_SEGMENT"];
    recordingSegment["when"] = "When a segment is recorded to disk as part of a DVR workflow";
    recordingSegment["stream_specific"] = true;
    recordingSegment["payload"] = "stream name (string)\nsegment target (string)\nsegment duration ms (integer)\n"
                                  "segment start timestamp ms (integer)\nsegment end timestamp ms (integer)";
    recordingSegment["response"] = "ignored";
    recordingSegment["response_action"] = "None.";

    JSON::Value & livepeerComplete = triggers["LIVEPEER_SEGMENT_COMPLETE"];
    livepeerComplete["when"] =
      "After a source segment has been successfully transcoded by Livepeer and all renditions have been received.";
    livepeerComplete["stream_specific"] = true;
    livepeerComplete["payload"] =
      "stream name (string)\nlivepeer session ID (string)\nsegment number (integer)\nsegment start ms (integer)\n"
      "segment duration ms (integer)\nsource width (integer)\nsource height (integer)\ninput bytes (integer)\n"
      "output bytes total (integer)\nrendition count (integer)\nattempt count (integer)\nbroadcaster URL (string)\n"
      "turnaround ms (integer)\nspeed factor (float)\nrenditions (JSON array with name and bytes per rendition)";
    livepeerComplete["response"] = "ignored";
    livepeerComplete["response_action"] = "None.";

    JSON::Value & avComplete = triggers["PROCESS_AV_VIRTUAL_SEGMENT_COMPLETE"];
    avComplete["when"] = "Every 5 seconds during MistProcAV operation and once on exit.";
    avComplete["stream_specific"] = true;
    avComplete["payload"] =
      "stream name (string)\ntrack type (audio or video)\nseconds since last trigger (integer)\n"
      "input frame count cumulative (integer)\noutput frame count cumulative (integer)\n"
      "input frames this window (integer)\noutput frames this window (integer)\ninput bytes this window (integer)\n"
      "output bytes this window (integer)\ndecode us per frame (integer)\ntransform us per frame (integer)\n"
      "encode us per frame (integer)\ninput codec short (string)\noutput codec short (string)\ninput width (integer)\n"
      "input height (integer)\noutput width (integer)\noutput height (integer)\ninput fpks (integer)\n"
      "output fps measured (float)\nsample rate (integer)\nchannels (integer)\nsource timestamp ms (integer)\n"
      "sink timestamp ms (integer)\nsource advanced ms (integer)\nsink advanced ms (integer)\n"
      "real-time factor in (float)\nreal-time factor out (float)\npipeline lag ms (integer)\n"
      "output bitrate bps (integer)\nis_final (0 or 1)";
    avComplete["response"] = "ignored";
    avComplete["response_action"] = "None.";

    JSON::Value & thumbnail = triggers["THUMBNAIL_UPDATED"];
    thumbnail["when"] = "When MistProcThumbs regenerates the sprite sheet and preview frame.";
    thumbnail["stream_specific"] = true;
    thumbnail["payload"] =
      "stream name (string)\npath to poster.jpg (string)\npath to sprite.jpg (string)\npath to sprite.vtt (string)";
    thumbnail["response"] = "ignored";
    thumbnail["response_action"] = "None.";
  }
} // namespace Controller

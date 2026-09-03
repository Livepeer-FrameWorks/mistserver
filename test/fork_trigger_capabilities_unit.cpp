#include "../src/controller/fork_trigger_capabilities.h"

#include <mist/triggers.h>

#include <iostream>
#include <string>

namespace {
  int fail(const std::string & message) {
    std::cerr << message << std::endl;
    return 1;
  }

  bool hasLine(const JSON::Value & trigger, const std::string & line) {
    const std::string & payload = trigger["payload"].asStringRef();
    return payload == line || payload.find(line + "\n") == 0 || payload.find("\n" + line + "\n") != std::string::npos ||
      (payload.size() >= line.size() && payload.compare(payload.size() - line.size(), line.size(), line) == 0);
  }

  bool expectEvent(const JSON::Value & triggers, const std::string & name, const std::string & response) {
    return triggers.isMember(name) && triggers[name]["stream_specific"].asBool() &&
      triggers[name]["response"].asStringRef() == response && triggers[name]["when"].asStringRef().size() &&
      triggers[name]["payload"].isString();
  }
} // namespace

int main() {
  JSON::Value triggers;
  Controller::addForkTriggerCapabilities(triggers);
  Controller::addForkTriggerCapabilities(triggers);

  const char *typedEvents[] = {"STREAM_SOURCE",  "STREAM_PROCESS", "PUSH_REWRITE",
                               "PUSH_OUT_START", "PLAY_REWRITE",   "USER_NEW"};
  for (size_t eventIndex = 0; eventIndex < sizeof(typedEvents) / sizeof(*typedEvents); ++eventIndex) {
    const JSON::Value & actions = triggers[typedEvents[eventIndex]]["actions"];
    if (!actions.isArray() || !actions.size()) {
      return fail(std::string(typedEvents[eventIndex]) + " has no actions");
    }
    for (size_t actionIndex = 0; actionIndex < actions.size(); ++actionIndex) {
      const Triggers::Action action = Triggers::actionFromString(actions[(unsigned int)actionIndex].asStringRef());
      if (action == Triggers::ACT_LEGACY || !Triggers::actionAllowed(typedEvents[eventIndex], action)) {
        return fail(std::string(typedEvents[eventIndex]) + " advertises an action rejected by runtime policy");
      }
    }
  }
  if (triggers["STREAM_SOURCE"]["actions"].size() != 4 || triggers["STREAM_PROCESS"]["actions"].size() != 3 ||
      triggers["PUSH_REWRITE"]["actions"].size() != 3 || triggers["PUSH_OUT_START"]["actions"].size() != 3 ||
      triggers["PLAY_REWRITE"]["actions"].size() != 3 || triggers["USER_NEW"]["actions"].size() != 2) {
    return fail("rebuilding trigger capabilities duplicated or omitted typed actions");
  }

  if (!expectEvent(triggers, "STREAM_PROCESS", "when-blocking") || !hasLine(triggers["STREAM_PROCESS"], "stream name (string)")) {
    return fail("STREAM_PROCESS does not publish its override contract");
  }
  if (!expectEvent(triggers, "PROCESS_EXIT", "ignored") ||
      !hasLine(triggers["PROCESS_EXIT"], "status (string: clean, retrying, disabled, unrecoverable, stopped)") ||
      !hasLine(triggers["PROCESS_EXIT"], "machine-readable exit reason (string)")) {
    return fail("PROCESS_EXIT does not publish its lifecycle contract");
  }
  if (!hasLine(triggers["RECORDING_END"], "recorded track and processing speed summary (JSON object, optional)")) {
    return fail("RECORDING_END does not publish its structured diagnostics tail");
  }
  if (!expectEvent(triggers, "RECORDING_SEGMENT", "ignored") ||
      !hasLine(triggers["RECORDING_SEGMENT"], "segment end timestamp ms (integer)")) {
    return fail("RECORDING_SEGMENT does not publish its DVR contract");
  }
  if (!expectEvent(triggers, "LIVEPEER_SEGMENT_COMPLETE", "ignored") ||
      !hasLine(triggers["LIVEPEER_SEGMENT_COMPLETE"], "renditions (JSON array with name and bytes per rendition)")) {
    return fail("LIVEPEER_SEGMENT_COMPLETE does not publish its rendition contract");
  }
  if (!expectEvent(triggers, "PROCESS_AV_VIRTUAL_SEGMENT_COMPLETE", "ignored") ||
      !hasLine(triggers["PROCESS_AV_VIRTUAL_SEGMENT_COMPLETE"], "is_final (0 or 1)")) {
    return fail("PROCESS_AV virtual segments do not publish their final-window contract");
  }
  if (!expectEvent(triggers, "THUMBNAIL_UPDATED", "ignored") || !hasLine(triggers["THUMBNAIL_UPDATED"], "path to sprite.vtt (string)")) {
    return fail("THUMBNAIL_UPDATED does not publish its output paths");
  }
  return 0;
}

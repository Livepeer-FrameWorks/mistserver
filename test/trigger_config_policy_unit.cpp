#include "../src/controller/trigger_config_policy.h"

#include <iostream>
#include <string>

namespace {
  int fail(const char *message) {
    std::cerr << message << std::endl;
    return 1;
  }

  JSON::Value config(const std::string & trigger, const std::string & onFail, bool sync) {
    JSON::Value result;
    result[trigger][0u]["handler"] = "http://handler.test/";
    result[trigger][0u]["sync"] = sync;
    result[trigger][0u]["onfail"] = onFail;
    return result;
  }
} // namespace

int main() {
  std::string error = "stale";
  JSON::Value empty;
  if (!Controller::validateTriggerOnFailConfig(empty, error) || !error.empty()) {
    return fail("an absent trigger object must remain valid and clear stale errors");
  }
  if (!Controller::validateTriggerOnFailConfig(config("PLAY_REWRITE", "deny", true), error) ||
      !Controller::validateTriggerOnFailConfig(config("STREAM_SOURCE", "offline", true), error) ||
      !Controller::validateTriggerOnFailConfig(config("STREAM_PROCESS", "use-configured", true), error) ||
      !Controller::validateTriggerOnFailConfig(config("PLAY_REWRITE", "LEGACY", false), error)) {
    return fail("valid synchronous and legacy trigger failure policies were rejected");
  }

  if (Controller::validateTriggerOnFailConfig(config("PLAY_REWRITE", "deny", false), error) ||
      error != "Invalid onfail action 'deny' for PLAY_REWRITE trigger") {
    return fail("an asynchronous decision action must be rejected with a useful error");
  }
  if (Controller::validateTriggerOnFailConfig(config("PLAY_REWRITE", "offline", true), error) ||
      Controller::validateTriggerOnFailConfig(config("STREAM_SOURCE", "value", true), error) ||
      Controller::validateTriggerOnFailConfig(config("STREAM_SOURCE", "unknown", true), error)) {
    return fail("incompatible, value-bearing, and unknown failure actions must be rejected");
  }

  JSON::Value legacy;
  legacy["PLAY_REWRITE"][0u].append("/legacy-handler");
  legacy["PLAY_REWRITE"][0u].append(true);
  if (!Controller::validateTriggerOnFailConfig(legacy, error)) {
    return fail("legacy trigger array entries must remain compatible");
  }
  return 0;
}

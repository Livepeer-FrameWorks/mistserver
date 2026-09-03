#include "../src/controller/push_control_request.h"

#include <cassert>

int main() {
  JSON::Value empty;
  Controller::PushControlRequest parsed = Controller::parsePushControlRequest(empty);
  assert(parsed.killIds.empty());
  assert(parsed.reinitializeIds.empty());
  assert(parsed.reinitializeTargets.empty());
  assert(parsed.invalidReinitializeRequests == 0);

  JSON::Value scalarKill;
  scalarKill["push_kill"] = 101;
  parsed = Controller::parsePushControlRequest(scalarKill);
  assert(parsed.killIds.size() == 1);
  assert(parsed.killIds[0] == 101);

  JSON::Value commands;
  commands["push_kill"].append(201);
  commands["push_kill"].append(202);
  commands["push_reinit"].append(301);
  commands["push_reinit"].append(JSON::fromString("{\"streamname\":\"camera\",\"target\":\"rtmp://origin/live\"}"));
  commands["push_reinit"].append(JSON::fromString("{\"streamname\":\"missing-target\"}"));
  commands["push_reinit"].append("invalid");
  parsed = Controller::parsePushControlRequest(commands);
  assert(parsed.killIds.size() == 2);
  assert(parsed.killIds[0] == 201);
  assert(parsed.killIds[1] == 202);
  assert(parsed.reinitializeIds.size() == 1);
  assert(parsed.reinitializeIds[0] == 301);
  assert(parsed.reinitializeTargets.size() == 1);
  assert(parsed.reinitializeTargets[0].first == "camera");
  assert(parsed.reinitializeTargets[0].second == "rtmp://origin/live");
  assert(parsed.invalidReinitializeRequests == 2);

  JSON::Value scalarTarget;
  scalarTarget["push_reinit"]["streamname"] = "program";
  scalarTarget["push_reinit"]["target"] = "srt://relay:9000";
  parsed = Controller::parsePushControlRequest(scalarTarget);
  assert(parsed.reinitializeTargets.size() == 1);
  assert(parsed.reinitializeTargets[0].first == "program");
  assert(parsed.reinitializeTargets[0].second == "srt://relay:9000");
  return 0;
}

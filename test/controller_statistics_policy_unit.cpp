#include "../src/controller/controller_statistics_policy.h"

#include <iostream>
#include <set>

namespace {
  int fail(const char *message) {
    std::cerr << message << std::endl;
    return 1;
  }
} // namespace

int main() {
  JSON::Value emptyPids = Controller::sourcePidList(std::set<uint64_t>());
  if (!emptyPids.isArray() || emptyPids.size()) {
    return fail("an observed stream without owners must publish sourcepids as an empty array");
  }

  std::set<uint64_t> claimedPids;
  claimedPids.insert(42);
  claimedPids.insert(0);
  claimedPids.insert(7);
  JSON::Value pids = Controller::sourcePidList(claimedPids);
  if (!pids.isArray() || pids.size() != 2 || pids[0u].asInt() != 7 || pids[1u].asInt() != 42) {
    return fail("sourcepids must exclude the unclaimed sentinel and retain deterministic PID order");
  }

  uint64_t totalUp = 10;
  uint64_t totalDown = 20;
  Controller::accumulateInterfaceCounters(true, false, true, 30, 40, totalUp, totalDown);
  if (totalUp != 40 || totalDown != 60) { return fail("eligible interface counters were not accumulated"); }

  Controller::accumulateInterfaceCounters(false, false, true, 100, 100, totalUp, totalDown);
  Controller::accumulateInterfaceCounters(true, true, true, 100, 100, totalUp, totalDown);
  Controller::accumulateInterfaceCounters(true, false, false, 100, 100, totalUp, totalDown);
  if (totalUp != 40 || totalDown != 60) {
    return fail("non-link, loopback, or missing interface counters must be excluded");
  }

  return 0;
}

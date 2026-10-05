#include <mist/proc_stats.h>
#include <mist/timing.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {
  int fail(const char *message) {
    fprintf(stderr, "%s\n", message);
    return 1;
  }

  bool parseUnsigned(const char *text, uint64_t maximum, uint64_t & value) {
    if (!text || !*text || *text == '-') { return false; }
    char *end = 0;
    errno = 0;
    const unsigned long long parsed = strtoull(text, &end, 10);
    if (errno || !end || end == text || *end || parsed > maximum) { return false; }
    value = parsed;
    return true;
  }
} // namespace

int main(int argc, char **argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: %s pid minimum-items\n", argv[0]);
    return 2;
  }
  uint64_t parsedPid = 0;
  uint64_t minimumItems = 0;
  if (!parseUnsigned(argv[1], std::numeric_limits<int>::max(), parsedPid) || !parsedPid) {
    return fail("invalid process id");
  }
  if (!parseUnsigned(argv[2], std::numeric_limits<uint64_t>::max(), minimumItems)) {
    return fail("invalid minimum item count");
  }
  const int pid = (int)parsedPid;

  char pageName[NAME_BUFFER_SIZE];
  snprintf(pageName, sizeof(pageName), SHM_PROC_STATE, pid);
  IPC::sharedPage page(pageName, 0, false, false);
  ProcState state;
  if (!ProcState::readSnapshot(page, state)) { return fail("process did not publish a readable ProcState snapshot"); }
  page.master = false;
  if (state.phase < PRC_PHASE_STARTUP || state.primaryResource == PRC_RESOURCE_UNKNOWN) {
    return fail("process did not publish its lifecycle phase and primary resource");
  }
  if (!(state.flags & PRC_FLAG_CAPACITY_VALID) || !state.capacitySpeedQ16_16 || !state.recommendedFeedQ16_16 ||
      state.frameCount < minimumItems || !state.totalWork || !state.lastUpdateMs) {
    return fail("process did not publish measured capacity and completed work");
  }

  IPC::sharedPage nodePage(SHM_NODE_PRESSURE, 0, false, false);
  NodePressureState nodeState;
  if (!NodePressureState::readSnapshot(nodePage, nodeState)) {
    return fail("controller did not publish a readable NodePressureState snapshot");
  }
  nodePage.master = false;
  const uint64_t nowMs = Util::bootMS();
  if (!nodeState.lastUpdateMs || nodeState.lastUpdateMs > nowMs || nowMs - nodeState.lastUpdateMs > 5000) {
    return fail("controller node-pressure snapshot is stale");
  }

  printf("phase=%u resource=%u items=%llu capacity=%.2fx feed=%.2fx node-cpu=%.3f\n", state.phase,
         state.primaryResource, (unsigned long long)state.frameCount, (double)state.capacitySpeedQ16_16 / 65536.0,
         (double)state.recommendedFeedQ16_16 / 65536.0, (double)nodeState.cpuUseQ0_16 / 65535.0);
  return 0;
}

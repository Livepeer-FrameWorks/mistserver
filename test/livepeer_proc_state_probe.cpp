#include <mist/proc_stats.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <limits>

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s pid\n", argv[0]);
    return 2;
  }
  char *end = 0;
  errno = 0;
  const unsigned long parsed = strtoul(argv[1], &end, 10);
  if (errno || !end || end == argv[1] || *end || !parsed || parsed > std::numeric_limits<int>::max()) {
    fprintf(stderr, "invalid process id\n");
    return 2;
  }

  char pageName[NAME_BUFFER_SIZE];
  snprintf(pageName, sizeof(pageName), SHM_PROC_STATE, (int)parsed);
  IPC::sharedPage page(pageName, 0, false, false);
  ProcState state;
  if (!ProcState::readSnapshot(page, state)) {
    fprintf(stderr, "Livepeer did not publish a readable ProcState snapshot\n");
    return 1;
  }
  page.master = false;
  if (state.primaryResource != PRC_RESOURCE_EXTERNAL || state.phase < PRC_PHASE_READY ||
      !(state.flags & PRC_FLAG_CAPACITY_VALID) || !state.capacitySpeedQ16_16 || !state.recommendedFeedQ16_16 ||
      !state.totalWork || !state.totalExternalWait || !state.lastUpdateMs || state.queueDepth > 2 || state.inflight > 2) {
    fprintf(stderr, "Livepeer ProcState lacks measured external-processing capacity\n");
    return 1;
  }

  printf("phase=%u capacity=%.2fx feed=%.2fx queue=%u inflight=%u\n", state.phase, (double)state.capacitySpeedQ16_16 / 65536.0,
         (double)state.recommendedFeedQ16_16 / 65536.0, state.queueDepth, state.inflight);
  return 0;
}

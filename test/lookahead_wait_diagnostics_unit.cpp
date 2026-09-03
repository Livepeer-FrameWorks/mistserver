#include "../src/lookahead_wait_diagnostics.h"

#include <iostream>

namespace {
  int fail(const char *message) {
    std::cerr << message << std::endl;
    return 1;
  }
} // namespace

int main() {
  Mist::LookaheadWaitDiagnostics wait;
  if (wait.active() || wait.shouldLog(7, 1000) || !wait.active() || wait.track() != 7 || wait.elapsedMs(1000) != 0) {
    return fail("a new lookahead wait must start silently for its gating track");
  }
  if (wait.shouldLog(7, 5999) || !wait.shouldLog(7, 6000) || wait.elapsedMs(6000) != 5000 || wait.shouldLog(7, 10999) ||
      !wait.shouldLog(7, 11000)) {
    return fail("a continuous lookahead wait must log once per interval");
  }

  if (wait.shouldLog(8, 12000) || wait.track() != 8 || wait.elapsedMs(12000) != 0 || wait.shouldLog(8, 16999) ||
      !wait.shouldLog(8, 17000)) {
    return fail("changing the gating track must start a fresh diagnostic interval");
  }

  wait.clear();
  if (wait.active() || wait.elapsedMs(18000) != 0 || wait.shouldLog(8, 18000) || wait.elapsedMs(18000) != 0) {
    return fail("clearing a wait must prevent stale elapsed time from leaking into the next wait");
  }

  if (wait.shouldLog(8, 100) || wait.elapsedMs(100) != 0 || wait.shouldLog(8, 5099) || !wait.shouldLog(8, 5100)) {
    return fail("clock rollback must reset the interval without unsigned underflow");
  }

  return 0;
}

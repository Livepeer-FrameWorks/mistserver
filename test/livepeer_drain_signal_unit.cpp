#define main livepeerProcessorMain
#include "../src/process/process_livepeer.cpp"
#undef main

#include <csignal>
#include <cstdio>
#include <unistd.h>

int main() {
  conf.activate();
  raise(SIGTERM);
  if (!Util::Config::stopRequested()) { return 1; }
  // EOF keeps the sink active to drain. It must not erase a process stop request.
  conf.is_active = true;
  co.is_active = true;
  statQueueDepth.store(1);
  alarm(2);
  const uint64_t start = Util::bootMS();
  const bool drained = Mist::waitForLivepeerDrain();
  alarm(0);
  if (drained || !livepeerStopRequested.load() || Util::bootMS() - start > 250) {
    fprintf(stderr, "SIGTERM did not cancel the source drain without a sink thread\n");
    return 1;
  }
  puts("PASS SIGTERM directly cancels Livepeer drain and upload stop flag");
}

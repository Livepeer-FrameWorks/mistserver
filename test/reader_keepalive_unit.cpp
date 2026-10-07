// A process reader (ProcAV's source thread waiting for its sink, ONNX's ordered vision queue)
// blocked on the rest of its process must keep updating its session, or MistSession ends the
// session after 10 s without updates plus STATS_DELAY, which stops the process.
#include "../src/process/reader_keepalive.h"

#include <mist/timing.h>

#include <atomic>
#include <cstdio>
#include <thread>

namespace {
  int failures = 0;

  void check(bool ok, const char *message) {
    if (ok) { return; }
    fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
  }
} // namespace

int main() {
  std::mutex mutex;
  std::condition_variable cv;
  bool ready = false;
  std::atomic<int> keepAlives{0};
  std::atomic<bool> lockFreeDuringKeepAlive{true};

  // The other side releases the reader after 1.5 s.
  std::thread other([&]() {
    const uint64_t start = Util::bootMS();
    while (Util::bootMS() - start < 1500) { Util::sleep(10); }
    std::lock_guard<std::mutex> guard(mutex);
    ready = true;
    cv.notify_all();
  });

  const uint64_t start = Util::bootMS();
  {
    std::unique_lock<std::mutex> lock(mutex);
    Mist::waitKeepingSession(lock, cv, [&]() { return ready; }, [&]() {
      ++keepAlives;
      if (!mutex.try_lock()) {
        lockFreeDuringKeepAlive = false;
      } else {
        mutex.unlock();
      }
    });
    check(ready, "the wait returned before it was released");
  }
  const uint64_t waited = Util::bootMS() - start;
  other.join();

  // One keepalive per interval: a reader blocked for longer than the session timeout keeps updating
  // its session at least once per second, the rate at which Output::stats writes it.
  check(keepAlives >= 1500 / (int)Mist::READER_KEEPALIVE_INTERVAL.count() / 2,
        "the waiting reader did not keep its session alive while blocked");
  check(lockFreeDuringKeepAlive, "the keepalive ran with the wait's lock held, blocking the other side");
  check(waited < 1500 + 4 * Mist::READER_KEEPALIVE_INTERVAL.count(), "the wait did not end promptly once released");

  // A wait that is ready at once returns without a keepalive.
  keepAlives = 0;
  {
    std::unique_lock<std::mutex> lock(mutex);
    Mist::waitKeepingSession(lock, cv, [&]() { return true; }, [&]() { ++keepAlives; });
  }
  check(keepAlives == 0, "a wait that was ready at once still ran a keepalive");
  return failures ? 1 : 0;
}

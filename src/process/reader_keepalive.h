#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>

namespace Mist {
  /// How often a process reader that waits on the rest of its process updates its session.
  const std::chrono::milliseconds READER_KEEPALIVE_INTERVAL(100);

  /// Waits on cv until ready() holds, calling keepAlive (without the lock held) every
  /// READER_KEEPALIVE_INTERVAL until then. The reader thread of a process is the only one that
  /// updates its session; MistSession ends a session whose connection stopped updating, and that
  /// stops the process, so a reader blocked behind a slow sink or inference must keep updating it.
  template<class Ready, class KeepAlive>
  void waitKeepingSession(std::unique_lock<std::mutex> & lock, std::condition_variable & cv, Ready ready, KeepAlive keepAlive) {
    while (!cv.wait_for(lock, READER_KEEPALIVE_INTERVAL, ready)) {
      lock.unlock();
      keepAlive();
      lock.lock();
    }
  }
} // namespace Mist

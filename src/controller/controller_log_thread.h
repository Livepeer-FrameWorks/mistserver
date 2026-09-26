#pragma once

#include <atomic>
#include <thread>
#include <unistd.h>
#include <utility>

namespace Controller {
  class LogThread {
    public:
      /// stopFlag, when given, is the flag the reader polls: its writers are
      /// other processes, so closing the descriptors alone never ends the read.
      LogThread(std::thread && thread, int inputFd, int outputFd, std::atomic<bool> *stopFlag = 0)
        : worker(std::move(thread)), input(inputFd), output(outputFd), stopRequested(stopFlag) {}

      ~LogThread() { stop(); }

      LogThread(const LogThread &) = delete;
      LogThread & operator=(const LogThread &) = delete;

      void stop() {
        if (!worker.joinable()) { return; }
        if (stopRequested) { stopRequested->store(true); }
        if (output >= 0) {
          close(output);
          output = -1;
        }
        if (input >= 0) {
          close(input);
          input = -1;
        }
        worker.join();
      }

      bool joinable() const { return worker.joinable(); }

    private:
      std::thread worker;
      int input;
      int output;
      std::atomic<bool> *stopRequested;
  };
} // namespace Controller

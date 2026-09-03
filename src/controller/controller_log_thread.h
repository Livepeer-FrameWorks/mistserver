#pragma once

#include <thread>
#include <unistd.h>
#include <utility>

namespace Controller {
  class LogThread {
    public:
      LogThread(std::thread && thread, int inputFd, int outputFd)
        : worker(std::move(thread)), input(inputFd), output(outputFd) {}

      ~LogThread() { stop(); }

      LogThread(const LogThread &) = delete;
      LogThread & operator=(const LogThread &) = delete;

      void stop() {
        if (!worker.joinable()) { return; }
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
  };
} // namespace Controller

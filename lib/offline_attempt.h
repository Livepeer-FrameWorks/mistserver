#pragma once

#include "shared_memory.h"

#include <atomic>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unistd.h>

namespace Util {
  class OfflineAttemptResult {
    public:
      OfflineAttemptResult(bool *result) : resultFlag(result) {
        if (!resultFlag) { return; }
        *resultFlag = false;
        static std::atomic<uint64_t> sequence(0);
        pageName = "/MstAttRes_" + std::to_string(getpid()) + "_" + std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
        page.init(pageName, 1, true, false);
        if (page) { page.mapped[0] = 0; }
      }

      ~OfflineAttemptResult() {
        if (!resultFlag) { return; }
        if (page && page.mapped[0] == 1) { *resultFlag = true; }
        page.master = true;
      }

      OfflineAttemptResult(const OfflineAttemptResult &) = delete;
      OfflineAttemptResult & operator=(const OfflineAttemptResult &) = delete;

      void markOffline() {
        if (page) { page.mapped[0] = 1; }
      }

      void advertiseForExec() {
        if (page) { setenv("MIST_OFFLINE_RESULT_PAGE", pageName.c_str(), 1); }
      }

      template<typename Spawn> auto runWithAdvertisement(Spawn spawn) -> decltype(spawn()) {
        if (!page) { return spawn(); }
        std::lock_guard<std::mutex> lock(environmentMutex());
        const char *existing = getenv("MIST_OFFLINE_RESULT_PAGE");
        const bool hadExisting = existing && *existing;
        const std::string previous = hadExisting ? existing : "";
        setenv("MIST_OFFLINE_RESULT_PAGE", pageName.c_str(), 1);
        const auto result = spawn();
        if (hadExisting) {
          setenv("MIST_OFFLINE_RESULT_PAGE", previous.c_str(), 1);
        } else {
          unsetenv("MIST_OFFLINE_RESULT_PAGE");
        }
        return result;
      }

      const std::string & name() const { return pageName; }

    private:
      static std::mutex & environmentMutex() {
        static std::mutex mutex;
        return mutex;
      }

      bool *resultFlag;
      std::string pageName;
      IPC::sharedPage page;
  };
} // namespace Util

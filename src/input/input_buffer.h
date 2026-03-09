#include "input.h"
#include <mist/dtsc.h>
#include <mist/proc_stats.h>
#include <mist/shared_memory.h>
#include <map>
#include <sys/types.h>

namespace Mist{
  class InputBuffer : public Input{
  public:
    InputBuffer(Util::Config *cfg);
    ~InputBuffer();
    void onCrash();

  private:
    void fillBufferDetails(JSON::Value &details) const;
    JSON::Value processOverride;       /*LTS*/
    bool processOverrideResolved;      /*LTS*/
    uint64_t bufferTime;
    uint64_t idleTime;
    uint64_t cutTime;
    size_t segmentSize;  /*LTS*/
    uint64_t lastReTime; /*LTS*/
    uint64_t lastProcTime; /*LTS*/
    uint64_t firstProcTime; /*LTS*/
    uint64_t finalMillis;
    bool hasPush;//Is a push currently being received?
    bool everHadPush;//Was there ever a push received?
    bool allProcsRunning;
    bool resumeMode;
    uint64_t maxKeepAway;

  protected:
    // Private Functions
    bool preRun();
    bool checkArguments(){return true;}
    void updateMeta();
    bool needHeader(){return false;}
    void getNext(size_t idx = INVALID_TRACK_ID){};
    void seek(uint64_t seekTime, size_t idx = INVALID_TRACK_ID){};
    bool keepRunning(bool updateActCtr = true);

    void removeTrack(size_t tid);

    bool removeKey(size_t tid);
    void removeUnused();
    void finish();

    void userLeadIn();
    void userOnActive(size_t id);
    void userOnDisconnect(size_t id);
    void userLeadOut();
    // This is used for an ugly fix to prevent metadata from disappearing in some cases.
    std::map<size_t, std::string> initData;

    uint64_t findTrack(const std::string &trackVal);
    void checkProcesses(const JSON::Value &procs); // LTS
    void updateProcessingRate();
    std::map<std::string, pid_t> runningProcs;     // LTS
    std::map<std::string, uint32_t> procBoots;
    std::map<std::string, uint64_t> procNextBoot;

    // Rate control state
    uint64_t effectiveSpeed;
    uint64_t lastRateUpdateMs;
    std::map<pid_t, uint64_t> procCpuPrev;          // previous cumulative CPU time (microseconds)
    std::map<pid_t, ProcTimingStats> procStatsPrev;  // previous timing stats snapshot
    uint64_t sysCpuIdlePrev;                         // previous system idle (microseconds)
    uint64_t sysCpuTotalPrev;                        // previous system total (microseconds)

    std::set<size_t> generatePids;
    std::map<size_t, size_t> sourcePids;
    size_t lastBPS; ///< Used for STREAM_BANDWIDTH trigger
  };
}// namespace Mist

typedef Mist::InputBuffer mistIn;

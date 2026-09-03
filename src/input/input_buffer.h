#include "input.h"

#include <mist/dtsc.h>
#include <mist/proc_stats.h>
#include <mist/process_stream_state.h>
#include <mist/shared_memory.h>

#include <map>
#include <set>
#include <sys/types.h>

namespace Mist{
  class InputBuffer : public Input{
  public:
    InputBuffer(Util::Config *cfg);
    ~InputBuffer();
    void onCrash();
    void onDebug();

  private:
    void fillBufferDetails(JSON::Value &details) const;
    JSON::Value processOverride; /*LTS*/
    bool processOverrideResolved; /*LTS*/
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
    bool processControlledRealtime;
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
    bool hasProcessDrainConsumers() const;
    bool hasActiveProcessProducers() const;
    bool processingProcessMatchesSource(const JSON::Value & proc) const;
    bool processingProcessMayMatchTranscodeOutput(const JSON::Value & proc, const JSON::Value & procs) const;
    bool processingProcessRetired(const JSON::Value & proc) const;
    size_t expectedProcessingOutputTracks(const JSON::Value & procs, bool & resolved) const;
    void publishProcessingOutputExpectation(const JSON::Value & procs);
    // This is used for an ugly fix to prevent metadata from disappearing in some cases.
    std::map<size_t, std::string> initData;

    uint64_t findTrack(const std::string &trackVal);
    void checkProcesses(const JSON::Value &procs); // LTS
    void updateProcessingRate();
    std::map<std::string, pid_t> runningProcs;     // LTS
    std::map<std::string, uint32_t> procBoots;
    std::map<std::string, uint64_t> procNextBoot;
    std::set<std::string> procHardFailed; // configs that hit unrecoverable error

    // Generic proc-authored rate and output-contract state (ProcState v4).
    uint64_t effectiveSpeed;
    bool startupSeedApplied;
    uint64_t lastRateUpdateMs;
    uint32_t rateJitterMs;
    uint32_t rampLockoutTicks; ///< # ticks remaining before speed-up allowed
    // Highest ProcState.lastUpdateMs we've already acted on, per pid. Procs
    // publish about every second, independently of the jittered consumer tick.
    // Without this gate, the same publish could drive repeated ramp bumps. We only
    // run per-proc decision logic when lastUpdateMs has advanced.
    std::map<pid_t, uint64_t> lastConsumedUpdateMs;
    // Required procs whose latest fresh sample explicitly allowed speed-up.
    // Cleared after each ramp; this lets offset publishers vote over
    // multiple ticks without allowing one proc to ramp repeatedly by itself.
    std::set<pid_t> procsReadyForSpeedUp;

    // Per-job speed/verdict aggregates. Written to the stream-state SHM page
    // (STRMSTATE_SPEED_* offsets) every controller tick so recording outputs
    // can attach them to RECORDING_END.
    ProcessStreamState speedStats;

    std::set<size_t> generatePids;
    std::map<size_t, size_t> sourceUsers;
    std::map<size_t, size_t> processUsers;
    std::set<pid_t> processPidsWithUsers;
    size_t drainConsumerUsers;
    size_t lastBPS; ///< Used for STREAM_BANDWIDTH trigger
  };
}// namespace Mist

typedef Mist::InputBuffer mistIn;

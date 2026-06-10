#include "input.h"
#include "processing_profile.h"

#include <mist/dtsc.h>
#include <mist/proc_stats.h>
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
    bool processingProcessMatchesSource(const JSON::Value & proc) const;
    bool processingProcessRetired(const JSON::Value & proc) const;
    size_t expectedProcessingOutputTracks(const JSON::Value & procs) const;
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

    // Rate control state. Driven entirely by per-proc normalized pressure
    // (ProcState v2 page); no separate CPU / sleep-ratio polling here.
    uint64_t effectiveSpeed;
    uint64_t lastRateUpdateMs;
    uint32_t rampLockoutTicks; ///< # ticks remaining before speed-up allowed
    // Highest ProcState.lastUpdateMs we've already acted on, per pid. Procs
    // publish every ~5s but the controller ticks every 1s. Without this
    // gate, the same publish would drive several *1.2+1 ramp bumps. We only
    // run per-proc decision logic when lastUpdateMs has advanced.
    std::map<pid_t, uint64_t> lastConsumedUpdateMs;
    // Required procs whose latest fresh sample explicitly allowed speed-up.
    // Cleared after each ramp; this lets offset 5s publishers vote over
    // multiple ticks without allowing one proc to ramp repeatedly by itself.
    std::set<pid_t> procsReadyForSpeedUp;

    // Per-instance processing profile (resolved from this stream's processes
    // array at startup; never mutates shared `processing.*` config).
    bool procProfileResolved; ///< true once classifier has run
    bool negotiatedFullyResolved; ///< true once every running proc has reported its negotiatedKind at least once
    ProcessingProfile procProfile;

    // Per-job speed/verdict aggregates. Written to the stream-state SHM page
    // (STRMSTATE_SPEED_* offsets) every controller tick so recording outputs
    // can attach them to RECORDING_END; also drives the periodic summary log.
    struct SpeedStats {
        uint32_t ticks = 0; ///< controller ticks with a resolved speed
        uint64_t speedSum = 0; ///< sum of effectiveSpeed over ticks (avg = sum/ticks)
        uint32_t speedMin = 0; ///< 0 = unset
        uint32_t speedMax = 0;
        uint32_t hardSlowTicks = 0;
        uint32_t regularSlowTicks = 0;
        uint32_t rampUps = 0;
        uint32_t lockoutTicks = 0; ///< ticks spent under ramp lockout
        uint32_t staleHoldTicks = 0; ///< ticks where a required proc was unobservable/stale
    };
    SpeedStats speedStats;
    // Per-proc observation aggregates for the summary log.
    struct ProcAgg {
        uint8_t kind = 0;
        uint32_t freshSamples = 0;
        uint32_t staleTicks = 0;
        uint64_t pressureSum = 0; ///< sum of pressureQ0_16 over fresh samples
        uint16_t pressureMax = 0;
        uint32_t reasonCounts[8] = {0};
        uint32_t hardSlowVotes = 0;
        uint32_t regularSlowVotes = 0;
    };
    std::map<pid_t, ProcAgg> procAggs;
    std::map<std::string, pid_t> procLastPid; ///< proc config -> last seen pid, for restart counting
    uint32_t procRestarts = 0;
    uint64_t lastSpeedSummaryMs = 0;
    void logSpeedSummary(const char *label);

    std::set<size_t> generatePids;
    std::map<size_t, size_t> sourceUsers;
    std::map<size_t, size_t> processUsers;
    std::set<pid_t> processPidsWithUsers;
    size_t lastBPS; ///< Used for STREAM_BANDWIDTH trigger
  };
}// namespace Mist

typedef Mist::InputBuffer mistIn;

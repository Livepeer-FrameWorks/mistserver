#pragma once

#include <mist/defines.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace Mist {
  enum ProcessingSourceEofAction {
    PROCESSING_EOF_NONE,
    PROCESSING_EOF_WAIT,
    PROCESSING_EOF_DRAIN,
    PROCESSING_EOF_STOP
  };

  inline bool retainDisconnectedSourceTrack(bool resumeMode, bool processControlledRealtime, bool hasDrainConsumer, bool rawHls) {
    return resumeMode || processControlledRealtime || hasDrainConsumer || rawHls;
  }

  /// A publisher's tracks can outlive its disconnect: resume keeps them for
  /// the next session, attached processes drain them. The next session either
  /// resumes a track (same id) or registers a new one, for example when its
  /// codec init differs. A retained track of the same type that was not
  /// resumed is stale from that moment: outputs only reselect when a track
  /// disappears, so it must go then rather than at idle eviction, or a DVR
  /// push keeps reading it and never splits. Process-controlled and raw-HLS
  /// tracks continue with their producer instead.
  inline bool retainedSourceTrackGoesStale(bool processControlledRealtime, bool rawHls) {
    return !processControlledRealtime && !rawHls;
  }

  /// Whether a retained track is replaced by the track a new publisher session
  /// just registered.
  inline bool dropRetainedSourceTrack(size_t retainedTrack, const std::string & retainedType, size_t newSourceTrack,
                                      const std::string & newType) {
    return retainedTrack != newSourceTrack && retainedType == newType;
  }

  /// Whether a buffer track was produced by a process from another track
  /// (renditions, thumbnails, detections) rather than published as source.
  inline bool bufferTrackIsDerived(uint64_t sourceTrack) {
    return sourceTrack != INVALID_TRACK_ID;
  }

  /// The fragment count that decides when a buffer is playable. The source
  /// tracks alone decide it: viewers can play the source while derived tracks
  /// are still waiting on their process, which can take seconds (a remote
  /// transcoder) or never happen. A buffer with no source media tracks falls
  /// back to all tracks.
  inline uint64_t bufferReadinessFragments(uint64_t sourceFrags, uint64_t allFrags, bool hasSourceMedia) {
    return hasSourceMedia ? sourceFrags : allFrags;
  }

  /// Process configs from STREAM_PROCESS can carry credentials bound to one
  /// publisher session (a Livepeer job token names the ingest session). When
  /// every publisher of a resumed live buffer left and a new one registers,
  /// the buffer asks again so its processes run on the new session's config.
  /// Process-controlled buffers have one producer for their whole life.
  inline bool publisherLeftEndsProcessSession(bool processControlledRealtime, size_t remainingSourceUsers) {
    return !processControlledRealtime && !remainingSourceUsers;
  }

  inline ProcessingSourceEofAction processingSourceEofAction(bool active, bool hasPush, bool everHadPush, bool resumeMode,
                                                             bool processControlledRealtime, bool hasDrainConsumer) {
    if (!active || hasPush || !everHadPush) { return PROCESSING_EOF_NONE; }
    if (resumeMode && !processControlledRealtime) { return PROCESSING_EOF_NONE; }
    if (hasDrainConsumer) { return PROCESSING_EOF_WAIT; }
    if (processControlledRealtime) { return PROCESSING_EOF_DRAIN; }
    return PROCESSING_EOF_STOP;
  }

  inline bool processingSelectionEnded(bool live, bool processControlledRealtime, uint8_t streamState) {
    if (!live || !processControlledRealtime) { return false; }
    return streamState == STRMSTAT_SHUTDOWN || streamState == STRMSTAT_OFF;
  }

  inline bool processingInputTrackEnded(bool processBinary, bool live, bool processControlledRealtime, bool claimed, uint8_t streamState) {
    return processBinary && live && processControlledRealtime && !claimed && streamState == STRMSTAT_WAIT;
  }

  inline bool processingTrackProducerEnded(bool live, bool processControlledRealtime, bool sourceEof,
                                           bool processProducersFinished, bool derivedTrack, bool claimed) {
    return live && processControlledRealtime && !claimed && (derivedTrack ? processProducersFinished : sourceEof);
  }

  inline bool processingSelectedProducersEnded(bool live, bool processControlledRealtime, bool processProducersFinished,
                                               bool anySelectedTrackClaimed) {
    return live && processControlledRealtime && processProducersFinished && !anySelectedTrackClaimed;
  }

  inline bool waitForLiveLookAhead(bool live, bool processingEnded, uint64_t needsLookAhead, uint64_t trackNow, uint64_t packetTime) {
    return live && !processingEnded && needsLookAhead && trackNow < packetTime + needsLookAhead;
  }

  inline bool processingTrackDrained(bool live, bool processControlledRealtime, bool processingEnded,
                                     uint64_t trackLast, uint64_t packetTime) {
    return live && processControlledRealtime && processingEnded && trackLast <= packetTime;
  }

  inline uint64_t simulatedLiveReadaheadMs(bool processControlledRealtime, uint64_t ordinaryReadaheadMs) {
    return processControlledRealtime ? 0 : ordinaryReadaheadMs;
  }

  inline bool liveClusterTrackReady(uint64_t clusterEnd, uint64_t trackFirst, uint64_t trackNow, uint64_t trackLast,
                                    bool claimed, bool processingEnded) {
    if (!clusterEnd || trackFirst >= clusterEnd || trackNow >= clusterEnd) { return true; }
    if (!claimed && trackLast < clusterEnd) { return true; }
    return processingEnded && trackLast < clusterEnd;
  }

  inline bool useLiveEbmlLayout(bool metadataLive, bool recording, bool fileTarget, bool recordingSourceWasLive) {
    return metadataLive || (recording && fileTarget && recordingSourceWasLive);
  }

  inline bool processingRecordingNeedsTrackGate(bool recordingToFile, bool hasMetadata, bool processControlledRealtime,
                                                bool streamShuttingDown) {
    return recordingToFile && hasMetadata && processControlledRealtime && !streamShuttingDown;
  }

  /// Bound on how long a draining stream waits for a still-running producer's
  /// outputs before the recording header is written with the tracks that exist.
  const uint64_t PROCESSING_PRODUCER_DRAIN_MS = 30000;

  /// Whether a draining processing stream releases the recording header gate:
  /// only once no more process output can arrive. The input ending is not that
  /// point; producers such as thumbnails create their tracks after the source
  /// ends. A producer still running PROCESSING_PRODUCER_DRAIN_MS after the
  /// source ended is not waited for any longer.
  inline bool processingRecordingGateReleased(bool streamShuttingDown, bool producersFinished,
                                              uint64_t sourceEndedSinceMs, uint64_t nowMs) {
    if (!streamShuttingDown) { return false; }
    if (producersFinished) { return true; }
    return sourceEndedSinceMs && nowMs >= sourceEndedSinceMs + PROCESSING_PRODUCER_DRAIN_MS;
  }

  /// Whether an original (non-process) track must have data before a
  /// processing recording writes its header: video and audio always, other
  /// types (metadata, subtitles), which can stay empty for a whole stream,
  /// only when the recording selects them.
  inline bool processingOriginalGatesHeader(const std::string & type, bool selected) {
    return type == "video" || type == "audio" || selected;
  }

  /// Whether a processing recording leaves out an original track that would
  /// join its selection after the header was written. A recording header
  /// cannot be extended, so an original metadata or subtitle track that first
  /// gets data after it (one the header did not wait for) stays out of this
  /// recording instead of failing it. Original video and audio tracks are
  /// still added: their header waited for them, so one joining later
  /// replaces a producer, which the replacement path ends cleanly. Process
  /// output tracks and every other recording keep their usual selection.
  inline bool processingRecordingSkipsLateOriginal(bool recordingToFile, bool processControlledRealtime,
                                                   bool sentHeader, bool derivedTrack, const std::string & type) {
    return recordingToFile && processControlledRealtime && sentHeader && !derivedTrack &&
      !processingOriginalGatesHeader(type, false);
  }

  /// Takes the track-data snapshot a processing recording-header check
  /// counts, then runs the track selection, and returns the snapshot. Data
  /// presence only grows, so the selection made afterwards covers every track
  /// the snapshot saw with data. A track whose first packet lands while the
  /// selection runs is then selected but not yet counted ready, and the header
  /// waits one more poll for it. The opposite order can count such a track
  /// ready while leaving it unselected, and a header written then omits it.
  template<typename Snapshot, typename Select>
  auto processingRecordingSnapshotThenSelect(Snapshot snapshot, Select select) -> decltype(snapshot()) {
    auto taken = snapshot();
    select();
    return taken;
  }

  inline bool processingRecordingTrackCountsReady(bool expectationResolved, size_t expectedOutputTracks, size_t readyOutputTracks,
                                                  size_t selectedOriginalTracks, size_t readyOriginalTracks,
                                                  size_t selectedOutputTracks, size_t readySelectedOutputTracks) {
    return expectationResolved && readyOutputTracks >= expectedOutputTracks &&
      readyOriginalTracks >= selectedOriginalTracks && readySelectedOutputTracks >= selectedOutputTracks;
  }

  inline bool waitForProcessingRecordingHeader(bool sentHeader, bool outputActive, bool tracksReady) {
    return !sentHeader && outputActive && !tracksReady;
  }

  /// Interval at which an unchanged processing recording-header wait is logged again.
  const uint64_t PROCESSING_RECORDING_WAIT_LOG_REPEAT_MS = 10000;

  /// Throttle for the processing recording-header wait log. The gate is polled
  /// every output loop iteration while it holds, so a wait is logged when it
  /// starts and whenever its reported state (the track counts) changes, and an
  /// unchanged wait is repeated at most every intervalMs.
  class ProcessingRecordingWaitLog {
    public:
      bool shouldLog(const std::string & state, uint64_t nowMs, uint64_t intervalMs = PROCESSING_RECORDING_WAIT_LOG_REPEAT_MS) {
        if (waiting && state == lastState && nowMs >= lastLogMs && nowMs - lastLogMs < intervalMs) { return false; }
        waiting = true;
        lastState = state;
        lastLogMs = nowMs;
        return true;
      }

      void clear() {
        waiting = false;
        lastState.clear();
      }

    private:
      bool waiting = false;
      std::string lastState;
      uint64_t lastLogMs = 0;
  };
} // namespace Mist

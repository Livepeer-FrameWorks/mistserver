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

  /// The buffer of a stream as a reader of a process-controlled live stream sees it.
  enum ProcessingBufferState {
    PROCESSING_BUFFER_ALIVE, ///< waiting for or serving data; also any stream that is not process-controlled and live
    PROCESSING_BUFFER_DRAINING, ///< signalled drain (SHUTDOWN): nothing is left to produce or wait for
    PROCESSING_BUFFER_GONE, ///< exited or killed: what a reader has not read yet is lost with it
  };

  /// Classifies the buffer of a process-controlled live stream by its stream state and whether the
  /// input process that holds the stream (named by its input PID page) still exists. A killed
  /// buffer never says so itself: that input process marks it invalid, so the state no longer says
  /// waiting, ready or draining, or it was killed as well. A buffer that exited leaves no state
  /// (OFF). A buffer that is merely busy (a synchronous trigger call) is alive.
  inline ProcessingBufferState processingBufferState(bool live, bool processControlledRealtime, uint8_t streamState, bool inputAlive) {
    if (!live || !processControlledRealtime) { return PROCESSING_BUFFER_ALIVE; }
    if (!inputAlive) { return PROCESSING_BUFFER_GONE; }
    if (streamState == STRMSTAT_SHUTDOWN) { return PROCESSING_BUFFER_DRAINING; }
    if (streamState == STRMSTAT_WAIT || streamState == STRMSTAT_READY) { return PROCESSING_BUFFER_ALIVE; }
    return PROCESSING_BUFFER_GONE;
  }

  /// Whether a process reading a process-controlled stream treats an unclaimed track as ended:
  /// only once the source ended, so no producer will claim it again.
  inline bool processingInputTrackEnded(bool processBinary, bool live, bool processControlledRealtime, bool claimed, bool sourceEof) {
    return processBinary && live && processControlledRealtime && !claimed && sourceEof;
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

  /// Whether a processing recording still runs its track gate. Before the header the gate
  /// releases only on its track counts; afterwards it stops once the stream drains.
  inline bool processingRecordingNeedsTrackGate(bool recordingToFile, bool hasMetadata, bool processControlledRealtime,
                                                bool gateReleased) {
    return recordingToFile && hasMetadata && processControlledRealtime && !gateReleased;
  }

  /// Whether an original (non-process) track must have data before a
  /// processing recording writes its header: video and audio always, other
  /// types (metadata, subtitles), which can stay empty for a whole stream,
  /// only when the recording selects them.
  inline bool processingOriginalGatesHeader(const std::string & type, bool selected) {
    return type == "video" || type == "audio" || selected;
  }

  /// Whether a processing recording header waits for this original track. Before the source
  /// ended, processingOriginalGatesHeader decides. After it, no original can get data any more:
  /// the originals the header needs are exactly those of them that have data.
  inline bool processingOriginalNeededForHeader(const std::string & type, bool selected, bool sourceEof, bool hasData) {
    if (sourceEof && !hasData) { return false; }
    return processingOriginalGatesHeader(type, selected);
  }

  /// Why a realtime feeder does not register a track its source file declares, given the file's
  /// complete index; empty when it registers it. A track without any frame will never carry data,
  /// and a video track without a keyframe never gets past the buffer, which only starts a track
  /// at a keyframe. Every track with data is registered, however short or late it starts.
  /// keyframeKnown tells whether the index records which video frames are keyframes.
  inline std::string realtimeTrackSkipReason(const std::string & type, size_t frames, bool keyframeKnown, bool hasKeyframe) {
    if (!frames) { return "it has no frames"; }
    if (type == "video" && keyframeKnown && !hasKeyframe) { return "it has no keyframe"; }
    return "";
  }

  /// Whether the buffer removes an older track carrying the same output key as a newer one: once
  /// the producer registered the newer one (claims it) and let go of the older one, the older one
  /// was replaced. While the older one is still claimed, its producer is still writing it.
  inline bool bufferRetiresReplacedOutput(bool olderClaimed, bool newerClaimed) {
    return newerClaimed && !olderClaimed;
  }

  /// Whether the buffer may erase a track that stopped receiving data. A process-controlled
  /// stream holds its whole source for its readers and processes: while any of them is still
  /// connected, a track that stopped updating (every track does once the source ended) is still
  /// being read.
  inline bool bufferIdleTrackEraseAllowed(bool processControlledRealtime, bool hasDrainConsumer) {
    return !processControlledRealtime || !hasDrainConsumer;
  }

  /// Whether a process-controlled buffer that waits for its readers after source EOF counts this
  /// tick as activity: a recorder whose read position or selection changed within the hold
  /// tracker's stale window (BUFFER_HOLD_STALE_MS) is still making progress. Without one, the
  /// inactivity timeout runs from the last data, as for any buffer.
  inline bool processingReaderKeepsBufferAlive(bool processControlledRealtime, ProcessingSourceEofAction eofAction,
                                               bool progressingReader) {
    return processControlledRealtime && eofAction == PROCESSING_EOF_WAIT && progressingReader;
  }

  /// Whether a processing recording that stops reports the buffer as lost (ER_SHM_LOST, which
  /// is retried) instead of the reason it recorded itself. That is the case when the buffer
  /// ended it (asked it to disconnect, signalled it, or went away) before it played out its
  /// selected tracks and its own reason is a clean one. A recording that reached its end keeps
  /// its reason, and so does one that already failed by itself.
  inline bool processingRecordingLostBuffer(bool processingRecording, bool reachedEnd, bool bufferEndedIt, bool cleanReason) {
    return processingRecording && !reachedEnd && bufferEndedIt && cleanReason;
  }

  /// Whether a live read waits for a track to reach the seek position: while it has a producer,
  /// or the buffer keeps it for a producer or publisher that is coming back (heldForResume). An
  /// unclaimed track nobody comes back for gets nothing more, so there is nothing to wait for.
  inline bool seekWaitsForTrack(bool live, bool claimed, bool heldForResume, uint64_t trackNow, uint64_t position) {
    return live && (claimed || heldForResume) && trackNow < position;
  }

  /// Whether a bounded read (one with a stop position) ends a track at its last packet instead
  /// of waiting for more: the track has no producer, the buffer does not keep it for one that is
  /// coming back (a restarting process, a publisher within its resume window), and its data stops
  /// short of the stop position, so no more data in range can arrive. Packets already read stay
  /// in the output. Unbounded reads (viewers, DVR recordings) keep waiting for the track.
  inline bool boundedReadTrackExhausted(bool bounded, bool live, bool claimed, bool heldForResume, uint64_t trackNow, uint64_t stopMs) {
    return bounded && live && !claimed && !heldForResume && trackNow < stopMs;
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

  /// Whether a processing recording can write its header: the processing graph is resolved, every
  /// expected process output its selection takes carries data (see recordingExpectedOutputs in
  /// process_graph.h), as do the originals it needs and the process outputs it selected.
  inline bool processingRecordingTrackCountsReady(bool graphResolved, size_t expectedOutputTracks, size_t readyOutputTracks,
                                                  size_t selectedOriginalTracks, size_t readyOriginalTracks,
                                                  size_t selectedOutputTracks, size_t readySelectedOutputTracks) {
    return graphResolved && readyOutputTracks >= expectedOutputTracks &&
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

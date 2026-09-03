#pragma once

#include <mist/defines.h>

#include <cstddef>
#include <cstdint>

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

  inline bool processingRecordingTrackCountsReady(bool expectationResolved, size_t expectedOutputTracks, size_t readyOutputTracks,
                                                  size_t selectedOriginalTracks, size_t readyOriginalTracks,
                                                  size_t selectedOutputTracks, size_t readySelectedOutputTracks) {
    return expectationResolved && readyOutputTracks >= expectedOutputTracks &&
      readyOriginalTracks >= selectedOriginalTracks && readySelectedOutputTracks >= selectedOutputTracks;
  }

  inline bool waitForProcessingRecordingHeader(bool sentHeader, bool outputActive, bool tracksReady) {
    return !sentHeader && outputActive && !tracksReady;
  }
} // namespace Mist

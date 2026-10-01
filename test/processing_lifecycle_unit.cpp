#include "../src/processing_lifecycle.h"

#include <cstdio>
#include <set>

namespace {
  int fail(const char *message) {
    fprintf(stderr, "%s\n", message);
    return 1;
  }
} // namespace

int main() {
  using namespace Mist;

  if (retainDisconnectedSourceTrack(false, false, false, false)) {
    return fail("ordinary non-resumable source tracks must be removed after disconnect");
  }
  if (!retainDisconnectedSourceTrack(true, false, false, false) || !retainDisconnectedSourceTrack(false, true, false, false) ||
      !retainDisconnectedSourceTrack(false, false, true, false) || !retainDisconnectedSourceTrack(false, false, false, true)) {
    return fail("resume, process drain, and raw-HLS sources must retain disconnected tracks");
  }

  // A retained publisher track goes stale when a new session registers a
  // track of its type that is not the retained one (resume did not apply).
  if (!retainedSourceTrackGoesStale(false, false)) {
    return fail("a live track kept after its publisher left must be remembered as stale-able");
  }
  if (retainedSourceTrackGoesStale(true, false) || retainedSourceTrackGoesStale(false, true)) {
    return fail("process-controlled and raw-HLS tracks continue with their producer");
  }
  if (!dropRetainedSourceTrack(0, "video", 8, "video")) {
    return fail("a new video track from the next session must replace the unresumed retained video track");
  }
  if (dropRetainedSourceTrack(1, "audio", 8, "video")) {
    return fail("a retained audio track stays until the next session registers its own audio");
  }
  if (dropRetainedSourceTrack(0, "video", 0, "video")) { return fail("a resumed track is never dropped"); }

  if (!bufferTrackIsDerived(3) || bufferTrackIsDerived(INVALID_TRACK_ID)) {
    return fail("a track with a source track is derived; a published track is not");
  }
  if (bufferReadinessFragments(3, 0, true) != 3) {
    return fail("a buffer whose source is ready is playable while its renditions have no fragments yet");
  }
  if (bufferReadinessFragments(0, 3, true) != 0) { return fail("a buffer whose source is not ready stays unplayable"); }
  if (bufferReadinessFragments(0xFFFFull, 3, false) != 3) {
    return fail("a buffer without source media tracks is judged by all its tracks");
  }

  if (!publisherLeftEndsProcessSession(false, 0)) {
    return fail("a live buffer whose last publisher left must re-resolve processes for the next session");
  }
  if (publisherLeftEndsProcessSession(false, 1)) {
    return fail("a publisher leaving while another still publishes keeps the current session");
  }
  if (publisherLeftEndsProcessSession(true, 0)) {
    return fail("process-controlled buffers keep their process config for their whole life");
  }

  if (processingSourceEofAction(false, false, true, false, true, false) != PROCESSING_EOF_NONE ||
      processingSourceEofAction(true, true, true, false, true, false) != PROCESSING_EOF_NONE ||
      processingSourceEofAction(true, false, false, false, true, false) != PROCESSING_EOF_NONE) {
    return fail("inactive, connected, and never-started streams must not enter EOF handling");
  }
  if (processingSourceEofAction(true, false, true, true, false, false) != PROCESSING_EOF_NONE) {
    return fail("ordinary resumable streams must remain available for source resume");
  }
  if (processingSourceEofAction(true, false, true, false, false, true) != PROCESSING_EOF_WAIT ||
      processingSourceEofAction(true, false, true, true, true, true) != PROCESSING_EOF_WAIT) {
    return fail("active consumers and processors must hold the buffer in WAIT while draining");
  }
  if (processingSourceEofAction(true, false, true, false, true, false) != PROCESSING_EOF_DRAIN ||
      processingSourceEofAction(true, false, true, true, true, false) != PROCESSING_EOF_DRAIN) {
    return fail("completed process-controlled streams must signal drain, including resume feeders");
  }
  if (processingSourceEofAction(true, false, true, false, false, false) != PROCESSING_EOF_STOP) {
    return fail("ordinary non-resumable streams must stop after producer EOF");
  }

  if (!processingSelectionEnded(true, true, STRMSTAT_SHUTDOWN) || !processingSelectionEnded(true, true, STRMSTAT_OFF) ||
      processingSelectionEnded(true, true, STRMSTAT_WAIT) || processingSelectionEnded(false, true, STRMSTAT_SHUTDOWN) ||
      processingSelectionEnded(true, false, STRMSTAT_SHUTDOWN)) {
    return fail("only process-controlled live shutdown may enable buffered output drain");
  }

  if (!processingInputTrackEnded(true, true, true, false, STRMSTAT_WAIT) ||
      processingInputTrackEnded(false, true, true, false, STRMSTAT_WAIT) ||
      processingInputTrackEnded(true, false, true, false, STRMSTAT_WAIT) ||
      processingInputTrackEnded(true, true, false, false, STRMSTAT_WAIT) ||
      processingInputTrackEnded(true, true, true, true, STRMSTAT_WAIT) ||
      processingInputTrackEnded(true, true, true, false, STRMSTAT_READY)) {
    return fail("only processors may treat an unclaimed process-controlled WAIT track as input EOF");
  }

  if (!processingTrackProducerEnded(true, true, true, false, false, false) ||
      processingTrackProducerEnded(true, true, true, false, true, false) ||
      !processingTrackProducerEnded(true, true, true, true, true, false) ||
      processingTrackProducerEnded(false, true, true, true, true, false) ||
      processingTrackProducerEnded(true, false, true, true, true, false) ||
      processingTrackProducerEnded(true, true, false, false, false, false) ||
      processingTrackProducerEnded(true, true, true, true, true, true)) {
    return fail("source and derived tracks must wait for their own producer lifecycle before draining");
  }

  if (!processingSelectedProducersEnded(true, true, true, false) ||
      processingSelectedProducersEnded(false, true, true, false) || processingSelectedProducersEnded(true, false, true, false) ||
      processingSelectedProducersEnded(true, true, false, false) || processingSelectedProducersEnded(true, true, true, true)) {
    return fail("recordings may finish only after source EOF and all selected producers have released their tracks");
  }

  if (!waitForLiveLookAhead(true, false, 1000, 1500, 1000) || waitForLiveLookAhead(true, true, 1000, 1500, 1000) ||
      waitForLiveLookAhead(false, false, 1000, 1500, 1000) || waitForLiveLookAhead(true, false, 1000, 2000, 1000)) {
    return fail("lookahead must be bypassed only while draining a process-controlled live stream");
  }

  if (!processingTrackDrained(true, true, true, 1000, 1000) || processingTrackDrained(true, false, true, 1000, 1000) ||
      processingTrackDrained(true, true, false, 1000, 1000) || processingTrackDrained(true, true, true, 1001, 1000)) {
    return fail("tracks must drop only after the process-controlled live tail is exhausted");
  }

  if (simulatedLiveReadaheadMs(false, 7000) != 7000 || simulatedLiveReadaheadMs(true, 7000) != 0) {
    return fail("process-controlled providers must not burst through the ordinary simulated-live readahead window");
  }

  if (!liveClusterTrackReady(2000, 2000, 1000, 1000, true, false) ||
      !liveClusterTrackReady(2000, 0, 2000, 1000, true, false) || !liveClusterTrackReady(2000, 0, 1000, 1000, false, false) ||
      liveClusterTrackReady(2000, 0, 1000, 1000, true, false) || !liveClusterTrackReady(2000, 0, 1000, 1000, true, true)) {
    return fail("EBML cluster readiness must wait for active tracks and release ended tails");
  }

  if (!useLiveEbmlLayout(true, false, false, false) || !useLiveEbmlLayout(false, true, true, true) ||
      useLiveEbmlLayout(false, false, true, true) || useLiveEbmlLayout(false, true, false, true) ||
      useLiveEbmlLayout(false, true, true, false)) {
    return fail("live EBML layout must survive metadata shutdown only for file recordings that started live");
  }

  if (processingRecordingNeedsTrackGate(false, true, true, false) || processingRecordingNeedsTrackGate(true, false, true, false) ||
      processingRecordingNeedsTrackGate(true, true, false, false) || processingRecordingNeedsTrackGate(true, true, true, true) ||
      !processingRecordingNeedsTrackGate(true, true, true, false)) {
    return fail("only active process-controlled file recordings may wait for late output tracks");
  }

  if (processingRecordingGateReleased(false, true, 1000, 999999)) {
    return fail("a stream that is not shutting down keeps its recording header gate");
  }
  if (processingRecordingGateReleased(true, false, 1000, 1000 + PROCESSING_PRODUCER_DRAIN_MS - 1) ||
      processingRecordingGateReleased(true, false, 0, 999999)) {
    return fail("a shutting-down stream still waits for a running producer's outputs, such as thumbnails made after "
                "the source ended");
  }
  if (!processingRecordingGateReleased(true, true, 0, 0) ||
      !processingRecordingGateReleased(true, false, 1000, 1000 + PROCESSING_PRODUCER_DRAIN_MS)) {
    return fail(
      "a shutting-down stream releases the gate once its producers finished, or stops waiting after the drain bound");
  }

  if (!processingOriginalGatesHeader("video", false) || !processingOriginalGatesHeader("audio", false)) {
    return fail("every original video and audio track must have data before a processing recording header");
  }
  if (processingOriginalGatesHeader("meta", false)) {
    return fail(
      "an unselected original metadata track, which can stay empty for a whole stream, must not hold the header");
  }
  if (!processingOriginalGatesHeader("meta", true)) {
    return fail("a selected original metadata track must have data before the header");
  }

  if (!processingRecordingSkipsLateOriginal(true, true, true, false, "meta") ||
      !processingRecordingSkipsLateOriginal(true, true, true, false, "subtitle")) {
    return fail("an original metadata track that gets data after the recording header must stay out of that recording");
  }
  if (processingRecordingSkipsLateOriginal(true, true, true, false, "video") ||
      processingRecordingSkipsLateOriginal(true, true, true, false, "audio")) {
    return fail("original video and audio joining after the header must reach the producer-replacement path");
  }
  if (processingRecordingSkipsLateOriginal(true, true, true, true, "meta")) {
    return fail("process output tracks joining after the header must keep their usual selection");
  }
  if (processingRecordingSkipsLateOriginal(true, true, false, false, "meta")) {
    return fail("an original metadata track selected before the header is declared by it");
  }
  if (processingRecordingSkipsLateOriginal(true, false, true, false, "meta") ||
      processingRecordingSkipsLateOriginal(false, true, true, false, "meta")) {
    return fail("live outputs and recordings that are not process-controlled keep their usual selection");
  }

  {
    // Track 0 is the original video; tracks 1 and 2 are the two expected
    // process outputs. The selection takes every track with data, and
    // track 2's first packet lands while it runs.
    std::set<size_t> withData = {0, 1};
    std::set<size_t> selected;
    size_t landsDuringSelection = 2;
    const auto select = [&]() {
      selected = withData;
      if (landsDuringSelection != INVALID_TRACK_ID) {
        withData.insert(landsDuringSelection);
        landsDuringSelection = INVALID_TRACK_ID;
      }
    };
    const auto poll = [&]() {
      const std::set<size_t> snapshot = processingRecordingSnapshotThenSelect([&]() { return withData; }, select);
      size_t readyOutputs = 0;
      for (const size_t track : snapshot) {
        if (track != 0) { ++readyOutputs; }
      }
      size_t selectedOutputs = 0;
      size_t readySelectedOutputs = 0;
      for (const size_t track : selected) {
        if (track == 0) { continue; }
        ++selectedOutputs;
        if (snapshot.count(track)) { ++readySelectedOutputs; }
      }
      const size_t readyOriginals = snapshot.count(0);
      return processingRecordingTrackCountsReady(true, 2, readyOutputs, 1, readyOriginals, selectedOutputs, readySelectedOutputs);
    };
    if (poll()) {
      return fail("a process output whose first packet lands during the selection must not complete the recording "
                  "header's count while it is unselected");
    }
    if (!poll() || !selected.count(2)) {
      return fail("the next header check must release with the late process output selected");
    }
  }

  if (processingRecordingTrackCountsReady(false, 0, 0, 0, 0, 0, 0)) {
    return fail("a process-controlled recording must remain gated during the unresolved boot window");
  }
  if (processingRecordingTrackCountsReady(true, 2, 1, 1, 1, 1, 1)) {
    return fail("all process-authored outputs must exist before a recording header is written");
  }
  if (processingRecordingTrackCountsReady(true, 2, 2, 2, 1, 1, 1)) {
    return fail("every original track must contain data before recording starts");
  }
  if (processingRecordingTrackCountsReady(true, 2, 2, 1, 1, 2, 1)) {
    return fail("every selected processing track must contain data before recording starts");
  }
  if (!processingRecordingTrackCountsReady(true, 2, 2, 1, 1, 1, 1) ||
      !processingRecordingTrackCountsReady(true, 2, 3, 1, 1, 1, 1)) {
    return fail("recording may start once the published contract and selected track set are both ready");
  }
  if (!processingRecordingTrackCountsReady(true, 3, 3, 1, 1, 0, 0)) {
    return fail("a narrow output selection must still wait for every process-authored stream track");
  }

  if (!waitForProcessingRecordingHeader(false, true, false) || waitForProcessingRecordingHeader(true, true, false) ||
      waitForProcessingRecordingHeader(false, false, false) || waitForProcessingRecordingHeader(false, true, true)) {
    return fail("initial seek and header must wait only while an active first header lacks its complete track set");
  }

  {
    ProcessingRecordingWaitLog waitLog;
    if (!waitLog.shouldLog("0/1 0/2", 1000)) { return fail("a recording-header wait must be logged when it starts"); }
    for (uint64_t now = 1100; now < 1000 + PROCESSING_RECORDING_WAIT_LOG_REPEAT_MS; now += 100) {
      if (waitLog.shouldLog("0/1 0/2", now)) {
        return fail("an unchanged recording-header wait polled every loop iteration must not be logged again");
      }
    }
    if (!waitLog.shouldLog("0/1 0/2", 1000 + PROCESSING_RECORDING_WAIT_LOG_REPEAT_MS)) {
      return fail("an unchanged recording-header wait must be logged again after the repeat interval");
    }
    if (!waitLog.shouldLog("1/1 0/2", 1000 + PROCESSING_RECORDING_WAIT_LOG_REPEAT_MS + 100)) {
      return fail("a recording-header wait whose counts change must be logged immediately");
    }
    if (waitLog.shouldLog("1/1 0/2", 1000 + PROCESSING_RECORDING_WAIT_LOG_REPEAT_MS + 200)) {
      return fail("a recording-header wait must not be logged again right after its counts changed");
    }
    if (!waitLog.shouldLog("1/1 0/2", 500)) {
      return fail("a clock that went backwards must not suppress the recording-header wait log");
    }
    waitLog.clear();
    if (!waitLog.shouldLog("1/1 0/2", 600)) {
      return fail("a new recording-header wait after the previous one ended must be logged when it starts");
    }
  }

  return 0;
}

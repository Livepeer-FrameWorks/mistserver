#include "../src/processing_lifecycle.h"

#include <cstdio>

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

  if (processingRecordingTrackCountsReady(false, 0, 0, 0, 0, 0, 0)) {
    return fail("a process-controlled recording must remain gated during the unresolved boot window");
  }
  if (processingRecordingTrackCountsReady(true, 2, 1, 1, 1, 1, 1)) {
    return fail("all process-authored outputs must exist before a recording header is written");
  }
  if (processingRecordingTrackCountsReady(true, 2, 2, 2, 1, 1, 1)) {
    return fail("every selected original track must contain data before recording starts");
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

  return 0;
}

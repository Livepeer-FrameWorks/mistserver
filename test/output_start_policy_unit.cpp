#include "../src/output/output_start_policy.h"

#include <mist/stream_status.h>

#include <iostream>
#include <string>

namespace {
  int fail(const char *message) {
    std::cerr << message << std::endl;
    return 1;
  }
} // namespace

int main() {
  Mist::PlayRewriteGate gate;
  if (gate.begin(false)) { return fail("a disabled PLAY_REWRITE must not be consumed"); }
  if (!gate.begin(true)) { return fail("an enabled PLAY_REWRITE must run once"); }
  if (gate.begin(true)) { return fail("PLAY_REWRITE must not run twice for status plus initialize"); }

  Triggers::Result result;
  result.action = Triggers::ACT_VALUE;
  result.response = "rewritten";
  if (Mist::playRewriteTarget("original", result) != "rewritten") {
    return fail("value action must use the trigger response");
  }
  result.action = Triggers::ACT_KEEP;
  if (Mist::playRewriteTarget("original", result) != "original") {
    return fail("keep action must retain the current stream");
  }
  result.action = Triggers::ACT_DENY;
  if (Mist::playRewriteTarget("original", result).size()) { return fail("deny action must clear the target"); }

  if (!Mist::statusAllowsFallback(false, "requested", "requested")) {
    return fail("ordinary source failure must remain fallback-eligible");
  }
  if (Mist::statusAllowsFallback(true, "requested", "requested")) {
    return fail("deliberate offline must bypass the fallback chain");
  }
  if (Mist::statusAllowsFallback(false, "requested", "already-rewritten")) {
    return fail("an already rewritten stream must not enter fallback again");
  }
  if (Mist::effectiveStatus(STRMSTAT_BOOT, true) != STRMSTAT_OFFLINE || Mist::effectiveStatus(STRMSTAT_BOOT, false) != STRMSTAT_BOOT) {
    return fail("attempt-local offline result must override only that status response");
  }

  // An output attached to a buffer that is still booting (no valid track yet)
  // waits instead of selecting nothing and ending; a processing reader and a
  // buffer that already has tracks never take this wait.
  const uint8_t bootingStates[] = {STRMSTAT_INIT, STRMSTAT_BOOT, STRMSTAT_WAIT};
  for (uint8_t state : bootingStates) {
    if (!Mist::outputWaitsForBootingBuffer(state, 0, false)) {
      return fail("an output must wait for a booting buffer that has no valid tracks yet");
    }
    if (Mist::outputWaitsForBootingBuffer(state, 0, true)) {
      return fail("a processing reader must never wait for a booting buffer");
    }
    if (Mist::outputWaitsForBootingBuffer(state, 2, false)) {
      return fail("a buffer with valid tracks is left to the readiness check");
    }
  }
  const uint8_t settledStates[] = {STRMSTAT_READY, STRMSTAT_OFF, STRMSTAT_SHUTDOWN, STRMSTAT_OFFLINE, STRMSTAT_INVALID};
  for (uint8_t state : settledStates) {
    if (Mist::outputWaitsForBootingBuffer(state, 0, false)) { return fail("only booting states make an output wait"); }
  }

  if (!Util::streamStatusIsTerminal(STRMSTAT_OFF) || !Util::streamStatusIsTerminal(STRMSTAT_OFFLINE) ||
      Util::streamStatusIsTerminal(STRMSTAT_SHUTDOWN)) {
    return fail("OFF and deliberate OFFLINE must be the only terminal shutdown states");
  }
  if (std::string(Util::streamStatusDescription(STRMSTAT_OFFLINE)) != "Stream is offline" ||
      std::string(Util::streamStatusDescription(STRMSTAT_READY)) != "Stream is online") {
    return fail("stream state descriptions must cover deliberate offline and ready states");
  }
  return 0;
}

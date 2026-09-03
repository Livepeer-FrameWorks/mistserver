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

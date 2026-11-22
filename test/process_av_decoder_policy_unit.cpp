#include "../src/process/process_av_decoder_policy.h"

#include <cstdio>
int main() {
  Mist::DecoderErrorStreak errors;
  for (unsigned count = 1; count < 128; ++count) {
    if (errors.invalidPacket()) { return 1; }
  }
  errors.decodedFrame();
  for (unsigned count = 1; count < 128; ++count) {
    if (errors.invalidPacket()) { return 1; }
  }
  if (!errors.invalidPacket()) {
    fprintf(stderr, "invalid compressed input can stall the decoder forever\n");
    return 1;
  }

  if (Mist::hardwareDecoderFailurePermanent(false, false)) {
    fprintf(stderr, "device-open failure or exhausted hardware sessions was classified as permanent\n");
    return 1;
  }
  if (!Mist::hardwareDecoderFailurePermanent(true, false) || Mist::hardwareDecoderFailurePermanent(true, true)) {
    fprintf(stderr, "source refusal and runtime failure have the wrong retry classification\n");
    return 1;
  }
  return 0;
}

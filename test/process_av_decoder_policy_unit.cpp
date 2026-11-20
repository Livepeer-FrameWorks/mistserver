#include "../src/process/process_av_decoder_policy.h"
#include <cstdio>
int main() {
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

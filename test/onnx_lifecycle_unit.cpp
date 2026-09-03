#include "../src/process/onnx_lifecycle.h"

#include <cstdio>

namespace {
  int fail(const char *message) {
    fprintf(stderr, "%s\n", message);
    return 1;
  }
} // namespace

int main() {
  using namespace Mist;

  if (!onnxSinkShouldContinue(true, false, false)) {
    return fail("the ONNX sink must wait while processing can still produce output");
  }
  if (!onnxSinkShouldContinue(true, true, true)) {
    return fail("the ONNX sink must drain queued tail output after processing finishes");
  }
  if (onnxSinkShouldContinue(true, true, false)) {
    return fail("the ONNX sink must stop once processing and its output queues are drained");
  }
  if (onnxSinkShouldContinue(false, false, true)) {
    return fail("an externally stopped or failed ONNX sink must not continue writing");
  }

  return 0;
}

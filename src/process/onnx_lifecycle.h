#pragma once

namespace Mist {
  inline bool onnxSinkShouldContinue(bool sinkActive, bool processingDone, bool outputPending) {
    return sinkActive && (!processingDone || outputPending);
  }
} // namespace Mist

#include "../src/process/onnx_proc_state.h"

#include <cstdio>
#include <cstring>
#include <unistd.h>

namespace {
  int fail(const char *message) {
    fprintf(stderr, "%s\n", message);
    return 1;
  }
} // namespace

int main() {
  using namespace Mist;

  if (onnxExpectedOutputTracks(ONNX_PROC_VISION, false) != 1 || onnxExpectedOutputTracks(ONNX_PROC_VISION, true) != 2 ||
      onnxExpectedOutputTracks(ONNX_PROC_AUDIO, true) != 1 || onnxExpectedOutputTracks(ONNX_PROC_TENSOR, true) != 1 ||
      onnxInputModality(ONNX_PROC_VISION) != PRC_INPUT_VIDEO || onnxInputModality(ONNX_PROC_AUDIO) != PRC_INPUT_AUDIO ||
      onnxInputModality(ONNX_PROC_TENSOR) != PRC_INPUT_TENSOR) {
    return fail("ONNX modality did not resolve to the exact output contract");
  }

  if (onnxExecutionProviderResource("CPUExecutionProvider") != PRC_RESOURCE_CPU ||
      onnxExecutionProviderResource("CUDAExecutionProvider") != PRC_RESOURCE_GPU ||
      onnxExecutionProviderResource("CoreMLExecutionProvider") != PRC_RESOURCE_GPU ||
      onnxExecutionProviderResource("OpenVINOExecutionProvider") != PRC_RESOURCE_UNKNOWN) {
    return fail("ONNX execution providers did not map to conservative resources");
  }

  OnnxProcSample sample;
  sample.wallDeltaMs = 1000;
  OnnxProcContract result = deriveOnnxProcContract(sample);
  if (!(result.flags & PRC_FLAG_SOURCE_LIMITED) || result.reasonCode != PRC_REASON_SOURCE_WAIT || !result.canAcceptMore) {
    return fail("an idle empty ONNX pipeline must report source starvation, not processor pressure");
  }

  sample = OnnxProcSample();
  sample.modality = ONNX_PROC_AUDIO;
  sample.resource = PRC_RESOURCE_CPU;
  sample.wallDeltaMs = 1000;
  sample.sourceDeltaMs = 1000;
  sample.sinkDeltaMs = 1000;
  sample.processedMediaDeltaMs = 1000;
  sample.workDeltaUs = 250000;
  result = deriveOnnxProcContract(sample);
  if (result.inputSpeedQ16_16 != ProcState::speedToQ16(1.0) || result.outputSpeedQ16_16 != ProcState::speedToQ16(1.0) ||
      result.capacitySpeedQ16_16 != ProcState::speedToQ16(4.0) || result.recommendedFeedQ16_16 != ProcState::speedToQ16(3.4) ||
      !(result.flags & PRC_FLAG_CAPACITY_VALID) || result.pressureQ0_16 != 0) {
    return fail("audio media/work deltas did not publish achieved speed and sustainable capacity separately");
  }

  sample = OnnxProcSample();
  sample.modality = ONNX_PROC_TENSOR;
  sample.resource = PRC_RESOURCE_CPU;
  sample.wallDeltaMs = 1000;
  sample.sourceDeltaMs = 8000;
  sample.sinkDeltaMs = 2000;
  sample.processedMediaDeltaMs = 2000;
  sample.workDeltaUs = 500000;
  result = deriveOnnxProcContract(sample);
  if (!(result.flags & PRC_FLAG_PROCESSOR_LIMITED) || result.reasonCode != PRC_REASON_CPU ||
      result.pressureQ0_16 < (uint16_t)(0.49 * 65535.0)) {
    return fail("input running materially faster than tensor capacity must report processor pressure");
  }

  sample.inputQueueDepth = 8;
  sample.queueCapacity = 8;
  result = deriveOnnxProcContract(sample);
  if (result.canAcceptMore || result.reasonCode != PRC_REASON_QUEUE_FULL || result.pressureQ0_16 != 65535) {
    return fail("a full ONNX tensor queue must hard-lock the adaptive feeder");
  }

  sample = OnnxProcSample();
  sample.modality = ONNX_PROC_AUDIO;
  sample.wallDeltaMs = 1000;
  sample.sourceDeltaMs = 1000;
  sample.inputDropsDelta = 1;
  result = deriveOnnxProcContract(sample);
  if (result.canAcceptMore || result.reasonCode != PRC_REASON_QUEUE_FULL) {
    return fail("dropped audio must be surfaced as a hard backpressure signal");
  }

  sample = OnnxProcSample();
  sample.modality = ONNX_PROC_VISION;
  sample.wallDeltaMs = 1000;
  sample.sourceDeltaMs = 1000;
  sample.inputDropsDelta = 3;
  sample.processedItemsDelta = 5;
  sample.workDeltaUs = 500000;
  sample.configuredMaxFps = 5.0;
  result = deriveOnnxProcContract(sample);
  if (!result.canAcceptMore || result.reasonCode == PRC_REASON_QUEUE_FULL) {
    return fail("latest-frame replacement at a satisfied configured vision rate must not throttle VOD feeding");
  }

  sample.workDeltaUs = 1500000;
  result = deriveOnnxProcContract(sample);
  if (result.canAcceptMore || result.reasonCode != PRC_REASON_QUEUE_FULL) {
    return fail("vision frame replacement below the configured inference capacity must signal overload");
  }

  char pageName[NAME_BUFFER_SIZE];
  snprintf(pageName, sizeof(pageName), "/MstOnnxProcStateTest_%d", getpid());
  IPC::sharedPage page(pageName, sizeof(ProcState), true, false);
  if (!page.mapped) { return fail("could not create ONNX ProcState test page"); }
  ProcState::initPage(page);
  publishOnnxOutputContract(page, ONNX_PROC_AUDIO, true);
  OnnxProcPublishState history;
  sample = OnnxProcSample();
  sample.modality = ONNX_PROC_AUDIO;
  sample.resource = PRC_RESOURCE_GPU;
  sample.wallDeltaMs = 1000;
  sample.sourceDeltaMs = 1000;
  sample.sinkDeltaMs = 1000;
  sample.processedMediaDeltaMs = 1000;
  sample.workDeltaUs = 250000;
  sample.processedItemsDelta = 1;
  for (uint64_t i = 1; i <= 3; ++i) {
    if (!publishOnnxProcState(page, sample, i * 250000, i, i * 1000, history)) {
      return fail("could not publish ONNX ProcState sample");
    }
  }
  ProcState snapshot;
  if (!ProcState::readSnapshot(page, snapshot) || snapshot.phase != PRC_PHASE_READY || snapshot.primaryResource != PRC_RESOURCE_GPU ||
      snapshot.totalWork != 750000 || snapshot.frameCount != 3 || snapshot.capacitySpeedQ16_16 != ProcState::speedToQ16(4.0) ||
      snapshot.recommendedFeedQ16_16 != ProcState::speedToQ16(3.4) || !(snapshot.flags & PRC_FLAG_CAPACITY_VALID) ||
      !(snapshot.flags & PRC_FLAG_OUTPUT_CONTRACT_VALID) || snapshot.expectedOutputTracks != 1 ||
      snapshot.inputModality != PRC_INPUT_AUDIO || snapshot.confidenceQ0_16 != 65535) {
    return fail("ONNX ProcState publisher did not persist a complete measured contract");
  }

  sample = OnnxProcSample();
  sample.modality = ONNX_PROC_AUDIO;
  sample.resource = PRC_RESOURCE_GPU;
  sample.wallDeltaMs = 1000;
  if (!publishOnnxProcState(page, sample, 750000, 3, 4000, history) || !ProcState::readSnapshot(page, snapshot) ||
      snapshot.capacitySpeedQ16_16 != ProcState::speedToQ16(4.0) || snapshot.recommendedFeedQ16_16 != ProcState::speedToQ16(3.4) ||
      !(snapshot.flags & PRC_FLAG_SOURCE_LIMITED) || !(snapshot.flags & PRC_FLAG_CAPACITY_VALID)) {
    return fail("a source-starved ONNX sample must retain its last measured capacity");
  }

  return 0;
}

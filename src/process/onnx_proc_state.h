#pragma once

#include <mist/proc_stats.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>

namespace Mist {
  enum OnnxProcModality : uint8_t {
    ONNX_PROC_VISION = 0,
    ONNX_PROC_AUDIO = 1,
    ONNX_PROC_TENSOR = 2,
  };

  inline ProcInputModality onnxInputModality(OnnxProcModality modality) {
    if (modality == ONNX_PROC_AUDIO) { return PRC_INPUT_AUDIO; }
    if (modality == ONNX_PROC_TENSOR) { return PRC_INPUT_TENSOR; }
    return PRC_INPUT_VIDEO;
  }

  inline uint16_t onnxExpectedOutputTracks(OnnxProcModality modality, bool annotatedVideo) {
    return 1 + (modality == ONNX_PROC_VISION && annotatedVideo ? 1 : 0);
  }

  inline void publishOnnxOutputContract(IPC::sharedPage & page, OnnxProcModality modality, bool annotatedVideo) {
    ProcState::publishOutputContract(page, onnxExpectedOutputTracks(modality, annotatedVideo), onnxInputModality(modality));
  }

  struct OnnxProcSample {
      OnnxProcModality modality = ONNX_PROC_VISION;
      ProcPrimaryResource resource = PRC_RESOURCE_UNKNOWN;
      uint64_t wallDeltaMs = 0;
      uint64_t sourceDeltaMs = 0;
      uint64_t sinkDeltaMs = 0;
      uint64_t processedMediaDeltaMs = 0;
      uint64_t workDeltaUs = 0;
      uint64_t processedItemsDelta = 0;
      uint32_t inputQueueDepth = 0;
      uint32_t outputQueueDepth = 0;
      uint32_t queueCapacity = 0;
      uint64_t inputDropsDelta = 0;
      uint64_t outputDropsDelta = 0;
      uint64_t errorsDelta = 0;
      double configuredMaxFps = 0.0;
  };

  struct OnnxProcContract {
      uint32_t inputSpeedQ16_16 = 0;
      uint32_t outputSpeedQ16_16 = 0;
      uint32_t capacitySpeedQ16_16 = 0;
      uint32_t recommendedFeedQ16_16 = ProcState::speedToQ16(1.0);
      uint16_t flags = 0;
      uint16_t pressureQ0_16 = 0;
      uint8_t canAcceptMore = 1;
      uint8_t reasonCode = PRC_REASON_UNKNOWN;
      uint32_t queueDepth = 0;
  };

  struct OnnxProcPublishState {
      uint32_t capacitySamples = 0;
      uint32_t lastCapacityQ16_16 = 0;
      uint32_t lastRecommendedFeedQ16_16 = ProcState::speedToQ16(1.0);
  };

  inline ProcPrimaryResource onnxExecutionProviderResource(const std::string & provider) {
    std::string lower(provider);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
    if (lower.find("cpu") != std::string::npos) { return PRC_RESOURCE_CPU; }
    if (lower.find("cuda") != std::string::npos || lower.find("tensorrt") != std::string::npos ||
        lower.find("coreml") != std::string::npos || lower.find("directml") != std::string::npos ||
        lower.find("rocm") != std::string::npos || lower.find("gpu") != std::string::npos) {
      return PRC_RESOURCE_GPU;
    }
    return PRC_RESOURCE_UNKNOWN;
  }

  inline OnnxProcContract deriveOnnxProcContract(const OnnxProcSample & sample) {
    OnnxProcContract result;
    result.queueDepth = sample.inputQueueDepth;
    if (sample.wallDeltaMs) {
      result.inputSpeedQ16_16 = ProcState::speedToQ16((double)sample.sourceDeltaMs / (double)sample.wallDeltaMs);
      result.outputSpeedQ16_16 = ProcState::speedToQ16((double)sample.sinkDeltaMs / (double)sample.wallDeltaMs);
    }

    const uint64_t capacityMediaMs = sample.processedMediaDeltaMs ? sample.processedMediaDeltaMs : sample.sinkDeltaMs;
    if (capacityMediaMs && sample.workDeltaUs) {
      const double capacity = (double)capacityMediaMs * 1000.0 / (double)sample.workDeltaUs;
      result.capacitySpeedQ16_16 = ProcState::speedToQ16(capacity);
      result.recommendedFeedQ16_16 = ProcState::speedToQ16(std::max(1.0, capacity * 0.85));
      result.flags |= PRC_FLAG_CAPACITY_VALID;
    }

    const bool queueFull = sample.queueCapacity &&
      (sample.inputQueueDepth >= sample.queueCapacity || sample.outputQueueDepth >= sample.queueCapacity);
    bool overloadDrop = sample.inputDropsDelta || sample.outputDropsDelta;
    if (overloadDrop && sample.modality == ONNX_PROC_VISION && sample.configuredMaxFps > 0.0 &&
        sample.processedItemsDelta && sample.workDeltaUs) {
      const double capacityFps = (double)sample.processedItemsDelta * 1000000.0 / (double)sample.workDeltaUs;
      if (capacityFps >= sample.configuredMaxFps) { overloadDrop = false; }
    }

    if (queueFull || overloadDrop) {
      result.canAcceptMore = 0;
      result.reasonCode = PRC_REASON_QUEUE_FULL;
      result.pressureQ0_16 = 65535;
      result.flags |= PRC_FLAG_PROCESSOR_LIMITED;
      return result;
    }

    double pressure = 0.0;
    if (sample.queueCapacity) {
      pressure = std::max(pressure, (double)sample.inputQueueDepth / (double)sample.queueCapacity);
      pressure = std::max(pressure, (double)sample.outputQueueDepth / (double)sample.queueCapacity);
    }
    if (result.inputSpeedQ16_16 && result.capacitySpeedQ16_16 && result.inputSpeedQ16_16 > result.capacitySpeedQ16_16) {
      const double overload = 1.0 - ((double)result.capacitySpeedQ16_16 / result.inputSpeedQ16_16);
      pressure = std::max(pressure, 0.7 + overload * 0.3);
    }
    if (pressure > 1.0) { pressure = 1.0; }
    result.pressureQ0_16 = (uint16_t)(pressure * 65535.0);
    if (pressure > 0.7) {
      result.flags |= PRC_FLAG_PROCESSOR_LIMITED;
      result.reasonCode = sample.resource == PRC_RESOURCE_CPU ? PRC_REASON_CPU : PRC_REASON_UNKNOWN;
    } else if (!sample.sourceDeltaMs && !sample.inputQueueDepth && !sample.outputQueueDepth) {
      result.flags |= PRC_FLAG_SOURCE_LIMITED;
      result.reasonCode = PRC_REASON_SOURCE_WAIT;
    } else if (sample.errorsDelta) {
      result.reasonCode = PRC_REASON_UNKNOWN;
    }
    return result;
  }

  inline bool publishOnnxProcState(IPC::sharedPage & page, const OnnxProcSample & sample, uint64_t totalWorkUs,
                                   uint64_t totalItems, uint64_t nowMs, OnnxProcPublishState & history) {
    if (!ProcState::isValid(page)) { return false; }
    const OnnxProcContract contract = deriveOnnxProcContract(sample);
    uint16_t flags = contract.flags;
    if (contract.capacitySpeedQ16_16) {
      ++history.capacitySamples;
      history.lastCapacityQ16_16 = contract.capacitySpeedQ16_16;
      history.lastRecommendedFeedQ16_16 = contract.recommendedFeedQ16_16;
    }
    if (history.lastCapacityQ16_16) { flags |= PRC_FLAG_CAPACITY_VALID; }

    ProcState *state = (ProcState *)page.mapped;
    state->beginPublish();
    state->totalWork = totalWorkUs;
    state->totalSourceWait = 0;
    state->totalSinkWait = 0;
    state->totalExternalWait = 0;
    state->frameCount = totalItems;
    state->lastUpdateMs = nowMs;
    state->observedSpeedQ16_16 = contract.outputSpeedQ16_16;
    state->inputSpeedQ16_16 = contract.inputSpeedQ16_16;
    state->outputSpeedQ16_16 = contract.outputSpeedQ16_16;
    state->capacitySpeedQ16_16 = history.lastCapacityQ16_16;
    state->recommendedFeedQ16_16 = history.lastRecommendedFeedQ16_16;
    state->flags = flags | (state->flags & PRC_FLAG_OUTPUT_CONTRACT_VALID);
    state->phase = history.capacitySamples >= 3 ? PRC_PHASE_READY : PRC_PHASE_MEASURING;
    state->confidenceQ0_16 = (uint16_t)std::min((uint32_t)65535, history.capacitySamples * 65535 / 3);
    state->pressureQ0_16 = contract.pressureQ0_16;
    state->canAcceptMore = contract.canAcceptMore;
    state->reasonCode = contract.reasonCode;
    state->queueDepth = contract.queueDepth;
    state->inflight = sample.processedItemsDelta && sample.workDeltaUs ? 1 : 0;
    state->retryCount = (uint32_t)std::min<uint64_t>(sample.errorsDelta, UINT32_MAX);
    state->primaryResource = sample.resource;
    state->endPublish();
    return true;
  }
} // namespace Mist

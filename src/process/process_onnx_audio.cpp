#include "process_onnx_audio.h"

#include <algorithm>
#include <cmath>

namespace Mist {
  namespace {
    // Silence detection (mirrors the upstream Parakeet energy VAD fallback): 20 ms frames,
    // 400 ms minimum pause, threshold = max(floor, 0.4 * mean frame RMS) over the region.
    const double VAD_FRAME_SEC = 0.02;
    const double VAD_MIN_SILENCE_SEC = 0.4;
    const float VAD_THRESH_FLOOR = 1e-3f;
    const float VAD_THRESH_FACTOR = 0.4f;
  }

  void AudioWindower::configure(uint64_t rate, double targetSec, double maxSec, double minSec, double maxBufferSec) {
    std::lock_guard<std::mutex> g(mtx_);
    rate_ = rate;
    // Defensive clamps (callers should validate too): non-positive/absurd values would
    // otherwise yield a zero-length chunk (no output) or an unbounded buffer.
    if (!(targetSec > 0.0) || targetSec > 3600.0) { targetSec = 20.0; }
    if (!(maxSec >= targetSec)) { maxSec = targetSec * 1.5; }
    if (maxSec > 3600.0) { maxSec = 3600.0; }
    if (!(minSec > 0.0) || minSec > targetSec) { minSec = targetSec * 0.4; }
    if (!(maxBufferSec >= maxSec)) { maxBufferSec = maxSec * 3.0; }
    targetSamples_ = (size_t)(targetSec * (double)rate);
    maxSamples_ = (size_t)(maxSec * (double)rate);
    minSamples_ = (size_t)(minSec * (double)rate);
    maxBufferSamples_ = (size_t)(maxBufferSec * (double)rate);
    if (targetSamples_ == 0) { targetSamples_ = (size_t)rate; }         // >= 1s
    if (maxSamples_ < targetSamples_) { maxSamples_ = targetSamples_; }
    if (minSamples_ == 0 || minSamples_ > targetSamples_) { minSamples_ = targetSamples_ / 2; }
    if (maxBufferSamples_ < maxSamples_) { maxBufferSamples_ = maxSamples_ * 3; }
  }

  void AudioWindower::advanceFront(size_t samples) {
    // Carry the sub-ms remainder so frontMs_ tracks stream time without cumulative drift.
    frontMsFrac_ += (double)samples * 1000.0 / (double)rate_;
    uint64_t whole = (uint64_t)frontMsFrac_;
    frontMs_ += whole;
    frontMsFrac_ -= (double)whole;
  }

  size_t AudioWindower::findSilenceCut(size_t lo, size_t hi) {
    // Caller holds mtx_. Compute per-frame RMS over [0, hi), then pick the midpoint of the
    // silence run (>= VAD_MIN_SILENCE_SEC) whose midpoint lands in [lo, hi] closest to
    // targetSamples_. Returns 0 when no qualifying pause exists.
    size_t frameSamples = (size_t)(VAD_FRAME_SEC * (double)rate_);
    if (frameSamples == 0) { frameSamples = 1; }
    size_t minSilenceFrames = (size_t)(VAD_MIN_SILENCE_SEC / VAD_FRAME_SEC);
    if (minSilenceFrames == 0) { minSilenceFrames = 1; }
    size_t nFrames = hi / frameSamples;
    if (nFrames == 0) { return 0; }

    std::vector<float> rms(nFrames);
    double meanAcc = 0.0;
    for (size_t fi = 0; fi < nFrames; ++fi) {
      double s = 0.0;
      size_t base = fi * frameSamples;
      for (size_t j = 0; j < frameSamples; ++j) { float v = fifo_[base + j]; s += (double)v * (double)v; }
      rms[fi] = (float)std::sqrt(s / (double)frameSamples);
      meanAcc += rms[fi];
    }
    float thresh = std::max(VAD_THRESH_FLOOR, (float)(meanAcc / (double)nFrames) * VAD_THRESH_FACTOR);

    size_t bestCut = 0;
    size_t bestDist = (size_t)-1;
    size_t runStart = 0;
    bool inRun = false;
    for (size_t fi = 0; fi <= nFrames; ++fi) {
      bool silent = (fi < nFrames) && (rms[fi] <= thresh);
      if (silent && !inRun) { inRun = true; runStart = fi; }
      else if (!silent && inRun) {
        inRun = false;
        size_t runLen = fi - runStart;
        if (runLen >= minSilenceFrames) {
          size_t cut = (runStart + runLen / 2) * frameSamples;   // pause midpoint
          if (cut >= lo && cut <= hi) {
            size_t dist = cut > targetSamples_ ? cut - targetSamples_ : targetSamples_ - cut;
            if (dist < bestDist) { bestDist = dist; bestCut = cut; }
          }
        }
      }
    }
    return bestCut;
  }

  // Read one big-endian signed sample of `bytes` bytes starting at p, as int32.
  static inline int32_t readBE(const unsigned char *p, int bytes) {
    switch (bytes) {
      case 1: return (int32_t)(int8_t)p[0];
      case 2: return (int32_t)(int16_t)((p[0] << 8) | p[1]);
      case 3: {
        int32_t v = (p[0] << 16) | (p[1] << 8) | p[2];
        if (v & 0x800000) { v |= ~0xFFFFFF; } // sign-extend 24-bit
        return v;
      }
      default: {
        // Build in unsigned to avoid signed-overflow UB when p[0] >= 0x80.
        uint32_t v = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
        return (int32_t)v;
      }
    }
  }

  void AudioWindower::feed(const char *data, size_t len, int bitDepth, int channels, uint64_t timeMs) {
    if (!data || len == 0 || channels <= 0 || rate_ == 0) { return; }
    // Only the depths readBE + the (1<<(bitDepth-1)) scaling are valid for. The caller
    // (ProcessSource) already fails closed on others; guard here too so this stays safe as
    // a standalone unit (no out-of-range shift / bogus normalization).
    if (bitDepth != 8 && bitDepth != 16 && bitDepth != 24 && bitDepth != 32) { return; }
    int bytes = bitDepth / 8;
    size_t frameBytes = (size_t)bytes * (size_t)channels;
    if (frameBytes == 0) { return; }
    size_t frames = len / frameBytes;
    // Full-scale divisor for this bit depth, to normalise into [-1, 1].
    const float scale = 1.0f / (float)((int64_t)1 << (bitDepth - 1));
    const unsigned char *p = (const unsigned char *)data;

    std::lock_guard<std::mutex> g(mtx_);
    // Resync the stream clock to this packet whenever the buffer was empty (first packet, or
    // after a drain): the next sample's true time is timeMs, so any prior drift / gap is
    // discarded rather than carried forward.
    if (!haveFront_ || fifo_.empty()) {
      frontMs_ = timeMs;
      frontMsFrac_ = 0.0;
      haveFront_ = true;
    } else {
      // Discontinuity while audio is buffered (seek / source restart): the packet's
      // timestamp should continue where the buffered audio ends. On a large jump the
      // buffered tail belongs to the old timeline — drop it and resync, so emitted
      // window timestamps stay truthful. Stateful consumers poll discontinuities().
      double bufferedMs = (double)fifo_.size() * 1000.0 / (double)rate_;
      uint64_t expected = frontMs_ + (uint64_t)(bufferedMs + frontMsFrac_);
      uint64_t gap = timeMs > expected ? timeMs - expected : expected - timeMs;
      if (gap > 2000) {
        fifo_.clear();
        frontMs_ = timeMs;
        frontMsFrac_ = 0.0;
        discontinuities_++;
      }
    }
    for (size_t f = 0; f < frames; ++f) {
      int64_t acc = 0;
      for (int c = 0; c < channels; ++c) { acc += readBE(p + ((size_t)c * bytes), bytes); }
      p += frameBytes;
      fifo_.push_back((float)((double)acc / (double)channels) * scale);
    }
    // Backpressure: if inference has fallen behind and the backlog exceeds the ceiling, drop
    // the OLDEST samples so memory and latency stay bounded (advancing the clock so surviving
    // audio keeps correct timestamps). Losing the oldest audio is preferable to unbounded
    // growth on a live stream; the caller reports droppedSamples_.
    if (maxBufferSamples_ > 0 && fifo_.size() > maxBufferSamples_) {
      size_t drop = fifo_.size() - maxBufferSamples_;
      fifo_.erase(fifo_.begin(), fifo_.begin() + drop);
      advanceFront(drop);
      droppedSamples_ += drop;
    }
  }

  bool AudioWindower::takeWindow(std::vector<float> &out, uint64_t &baseMs) {
    std::lock_guard<std::mutex> g(mtx_);
    if (targetSamples_ == 0) { return false; }
    size_t n = fifo_.size();
    if (n < targetSamples_) { return false; }              // accumulate to target first
    size_t hi = n < maxSamples_ ? n : maxSamples_;
    size_t cut = findSilenceCut(minSamples_, hi);
    if (cut == 0) {
      if (n >= maxSamples_) { cut = maxSamples_; }          // no pause found: hard cut at max
      else { return false; }                                 // wait for a pause (bounded by max)
    }
    out.assign(fifo_.begin(), fifo_.begin() + cut);
    baseMs = frontMs_;
    fifo_.erase(fifo_.begin(), fifo_.begin() + cut);
    advanceFront(cut);
    // If that drained the buffer, forget the clock so the next packet resyncs to its own
    // timestamp (handles an upstream gap across the boundary).
    if (fifo_.empty()) { haveFront_ = false; }
    return true;
  }

  double AudioWindower::bufferedSeconds() {
    std::lock_guard<std::mutex> g(mtx_);
    return rate_ ? (double)fifo_.size() / (double)rate_ : 0.0;
  }

  uint64_t AudioWindower::droppedSamples() {
    std::lock_guard<std::mutex> g(mtx_);
    return droppedSamples_;
  }

  uint64_t AudioWindower::discontinuities() {
    std::lock_guard<std::mutex> g(mtx_);
    return discontinuities_;
  }

  bool AudioWindower::takeFixed(size_t samples, std::vector<float> &out, uint64_t &baseMs) {
    std::lock_guard<std::mutex> g(mtx_);
    if (!rate_ || !samples || fifo_.size() < samples) { return false; }
    out.assign(fifo_.begin(), fifo_.begin() + samples);
    baseMs = frontMs_;
    fifo_.erase(fifo_.begin(), fifo_.begin() + samples);
    advanceFront(samples);
    return true;
  }

  bool AudioWindower::flush(std::vector<float> &out, uint64_t &baseMs, size_t minSamples) {
    std::lock_guard<std::mutex> g(mtx_);
    if (fifo_.size() < minSamples || fifo_.empty()) { return false; }
    out.assign(fifo_.begin(), fifo_.end());
    baseMs = frontMs_;
    fifo_.clear();
    haveFront_ = false;      // next packet resyncs the clock
    frontMsFrac_ = 0.0;
    return true;
  }
} // namespace Mist

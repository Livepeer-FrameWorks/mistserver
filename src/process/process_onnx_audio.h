#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace Mist {
  // Thread-safe accumulator that turns a stream of raw interleaved PCM packets (as
  // produced upstream by MistProcAV: any bit depth / channel count, big-endian in DTSC)
  // into mono float32 samples and hands the process thread fixed-length windows for ASR.
  //
  // Audio must never be dropped the way video frames are, so this buffers contiguously
  // instead of keeping only the latest packet. All heavy DSP (decode, resample) is done
  // upstream; this only does trivial format adaptation: endian swap, channel downmix,
  // integer -> float in [-1, 1].
  class AudioWindower {
    public:
      // rate: samples/sec of the incoming PCM (must equal the ASR model's sample rate).
      // Chunks are cut on speech pauses to avoid splitting words: accumulate to targetSec,
      // then cut at the nearest silence, never shorter than minSec nor longer than maxSec
      // (a hard cut is forced at maxSec if no pause is found). Chunks are non-overlapping.
      // maxBufferSec bounds the backlog: if inference falls behind and the buffer exceeds it,
      // the oldest audio is dropped (with a warning) to bound memory and latency.
      void configure(uint64_t rate, double targetSec, double maxSec, double minSec, double maxBufferSec);
      bool configured() const { return rate_ != 0; }

      // Decode one PCM packet and append its mono samples. bytes are interleaved,
      // big-endian, `bitDepth` bits per sample, `channels` channels. timeMs is the
      // stream timestamp of the packet's first sample.
      void feed(const char *data, size_t len, int bitDepth, int channels, uint64_t timeMs);

      // Pop the next chunk of mono f32 samples, cut at a speech pause once enough is
      // buffered. baseMs is set to the stream time of the chunk's first sample. Returns
      // false if not ready (still accumulating / waiting for a pause below maxSec).
      bool takeWindow(std::vector<float> &out, uint64_t &baseMs);

      // Pop exactly `samples` mono f32 samples, for fixed-chunk streaming models
      // (Silero VAD: 512 per call). Ignores the pause-windowing parameters. Returns
      // false while fewer samples are buffered. baseMs as in takeWindow.
      bool takeFixed(size_t samples, std::vector<float> &out, uint64_t &baseMs);

      // Pop whatever remains as a final (short) window if at least minSamples are left.
      bool flush(std::vector<float> &out, uint64_t &baseMs, size_t minSamples);

      // Seconds of audio currently buffered but not yet emitted (for observability).
      double bufferedSeconds();

      // Total samples dropped by backpressure so far (0 if inference keeps up). The caller
      // logs/reports this — the windower stays free of logging deps so it unit-tests cleanly.
      uint64_t droppedSamples();

      // Stream discontinuities detected so far: a packet whose timestamp does not
      // continue the buffered audio (seek / source restart) drops the stale buffered
      // tail and resyncs the clock. Stateful consumers (VAD) poll this to reset.
      uint64_t discontinuities();

    private:
      // Sample index in [lo, hi] at which to cut, chosen at the midpoint of the silence
      // run whose midpoint is closest to targetSamples_. Returns 0 if no qualifying pause.
      size_t findSilenceCut(size_t lo, size_t hi);

      // Advance frontMs_ by `samples`, carrying the sub-millisecond remainder so the stream
      // timestamp doesn't drift over a long run. Caller holds mtx_.
      void advanceFront(size_t samples);

      std::mutex mtx_;
      std::deque<float> fifo_;
      uint64_t rate_ = 0;
      size_t targetSamples_ = 0;   // preferred chunk length
      size_t maxSamples_ = 0;      // hard cut even without a pause
      size_t minSamples_ = 0;      // shortest chunk: no pause-cut before this much audio
      size_t maxBufferSamples_ = 0; // backpressure ceiling (0 = unbounded)
      uint64_t frontMs_ = 0;       // stream time of fifo_.front()
      double frontMsFrac_ = 0.0;   // carried sub-ms remainder of frontMs_
      bool haveFront_ = false;
      uint64_t droppedSamples_ = 0; // total dropped by backpressure (for one-shot warnings)
      uint64_t discontinuities_ = 0; // timestamp jumps detected in feed() (see discontinuities())
  };
} // namespace Mist

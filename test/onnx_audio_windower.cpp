// Unit test for AudioWindower's fixed-chunk streaming path and discontinuity
// detection: a packet whose timestamp does not continue the buffered audio (seek /
// source restart) must drop the stale buffered tail, resync the clock, and bump the
// discontinuity counter that stateful consumers (VAD) poll.
#include "../src/process/process_onnx_audio.cpp"

#include <cstdio>
#include <vector>

static int fails = 0;
#define CHECK(cond)                                                                                \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      fprintf(stderr, "FAIL (line %d): %s\n", __LINE__, #cond);                                    \
      ++fails;                                                                                     \
    }                                                                                              \
  } while (0)

// 16-bit big-endian mono packet of `samples` zeros
static std::vector<char> pcm(size_t samples) {
  return std::vector<char>(samples * 2, 0);
}

int main() {
  Mist::AudioWindower w;
  w.configure(16000, 2.0, 3.0, 1.0, 10.0);

  // 1) Basic fixed-chunk pops with continuous timestamps
  std::vector<char> p = pcm(1600); // 100 ms per packet
  w.feed(p.data(), p.size(), 16, 1, 0);
  w.feed(p.data(), p.size(), 16, 1, 100);
  std::vector<float> chunk;
  uint64_t baseMs = 77;
  CHECK(w.takeFixed(512, chunk, baseMs));
  CHECK(chunk.size() == 512);
  CHECK(baseMs == 0);
  CHECK(w.takeFixed(512, chunk, baseMs));
  CHECK(baseMs == 32); // 512 samples @ 16 kHz
  CHECK(w.discontinuities() == 0);

  // 2) Continuous next packet: no discontinuity
  w.feed(p.data(), p.size(), 16, 1, 200);
  CHECK(w.discontinuities() == 0);

  // 3) Timestamp jump while audio is still buffered: stale tail dropped, clock
  // resynced, counter bumped — the case a chunk-timestamp check alone cannot see.
  w.feed(p.data(), p.size(), 16, 1, 60000);
  CHECK(w.discontinuities() == 1);
  CHECK(w.takeFixed(512, chunk, baseMs));
  CHECK(baseMs == 60000); // first chunk after the jump starts on the new timeline

  // 4) Small jitter below the threshold is NOT a discontinuity
  double buffered = w.bufferedSeconds();
  w.feed(p.data(), p.size(), 16, 1, (uint64_t)(60000 + 32 + buffered * 1000.0 + 500));
  CHECK(w.discontinuities() == 1);

  if (fails) {
    fprintf(stderr, "%d windower checks failed\n", fails);
    return 1;
  }
  printf("All windower checks passed\n");
  return 0;
}

// Unit test for ONNX::Utils::computeFbank: checks sampled feature values against
// torchaudio.compliance.kaldi.fbank reference output (dither=0) for a deterministic
// signal, in both the kaldi-default (povey window) and HF/AST (hanning window)
// configurations. Reference values generated with torchaudio 2.x; the C++
// implementation matched to max_abs_diff < 6e-4 over full 80/128-bin feature matrices
// at integration time — the 0.01 tolerance here allows FFT/libm variation across
// platforms while still catching real regressions (wrong window, mel scale, log, ...).
#include "../lib/onnx.h"

#include <cmath>
#include <cstdio>
#include <vector>

static int fails = 0;
static void check(const std::vector<float> &feats, int bins, int frame, int bin, float want) {
  float got = feats[(size_t)frame * bins + bin];
  if (fabsf(got - want) > 0.01f) {
    fprintf(stderr, "FAIL: [%d][%d] = %f, want %f (bins=%d)\n", frame, bin, got, want, bins);
    ++fails;
  }
}

int main() {
  // 0.5 s of 0.4*sin(2*pi*220*t) + 0.2*sin(2*pi*1789*t) at 16 kHz
  std::vector<float> sig(8000);
  for (size_t i = 0; i < sig.size(); ++i) {
    double t = (double)i / 16000.0;
    sig[i] = (float)(0.4 * sin(2.0 * M_PI * 220.0 * t) + 0.2 * sin(2.0 * M_PI * 1789.0 * t));
  }

  // kaldi default: povey window, 80 bins (WeSpeaker convention)
  std::vector<float> povey;
  size_t frames = ONNX::Utils::computeFbank(sig.data(), sig.size(), 16000, 80, false, povey);
  if (frames != 48) {
    fprintf(stderr, "FAIL: povey frames = %zu, want 48\n", frames);
    ++fails;
  } else {
    check(povey, 80, 0, 0, -5.46075f);
    check(povey, 80, 0, 40, 4.86389f);
    check(povey, 80, 10, 5, 0.14872f);
    check(povey, 80, 10, 79, -15.94238f);
    check(povey, 80, 47, 26, -13.19704f);
  }

  // HF/AST: hanning window, 128 bins
  std::vector<float> hann;
  frames = ONNX::Utils::computeFbank(sig.data(), sig.size(), 16000, 128, true, hann);
  if (frames != 48) {
    fprintf(stderr, "FAIL: hanning frames = %zu, want 48\n", frames);
    ++fails;
  } else {
    check(hann, 128, 0, 0, -5.01204f);
    check(hann, 128, 0, 64, 4.58491f);
    check(hann, 128, 10, 5, -11.27939f);
    check(hann, 128, 10, 127, -15.94238f);
    check(hann, 128, 47, 42, -15.14136f);
  }

  // Too-short input yields zero frames, no crash
  std::vector<float> none;
  if (ONNX::Utils::computeFbank(sig.data(), 100, 16000, 80, false, none) != 0 || !none.empty()) {
    fprintf(stderr, "FAIL: short input should produce no frames\n");
    ++fails;
  }

  // CTC greedy decode: argmax per timestep, collapse repeats, drop blanks (class 0).
  // charset: [blank, "a", "b", "c"]. Rows below spell "ab" with a repeat and a blank.
  {
    std::vector<std::string> cs = {"", "a", "b", "c"};
    // t0: a(0.7) t1: a(0.9) repeat t2: blank(0.8) t3: b(0.6) — decodes to "ab"
    std::vector<float> logits = {
      0.1f, 0.7f, 0.1f, 0.1f,
      0.05f, 0.9f, 0.03f, 0.02f,
      0.8f, 0.1f, 0.05f, 0.05f,
      0.2f, 0.1f, 0.6f, 0.1f,
    };
    std::string text;
    float conf = 0.0f;
    size_t kept = ONNX::Utils::ctcGreedyDecode(logits.data(), 4, 4, cs, text, conf);
    if (kept != 2 || text != "ab") {
      fprintf(stderr, "FAIL: CTC decode = \"%s\" (%zu kept), want \"ab\" (2)\n", text.c_str(), kept);
      ++fails;
    }
    // conf = mean of the two kept probs (0.7 and 0.6)
    if (conf < 0.64f || conf > 0.66f) {
      fprintf(stderr, "FAIL: CTC conf = %f, want ~0.65\n", conf);
      ++fails;
    }
  }

  if (fails) {
    fprintf(stderr, "%d fbank checks failed\n", fails);
    return 1;
  }
  printf("All fbank checks passed\n");
  return 0;
}

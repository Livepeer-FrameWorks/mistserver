// Manual test/dev tool (not a unit test): load a single-file audio model through
// ModelFactory::createAudioModel (sidecar labels/sample-rate applied) and drive it with
// synthetic audio. For VAD models it also PROVES the recurrent state loop works:
// feeding the same chunk twice must give different probabilities, and resetting the
// state must reproduce the first probability exactly.
//
// Usage: onnxaudioprobe <model.onnx> <vad|classification|tagging|embedding>
#include "../lib/onnx.h"

#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>
#include <vector>

int main(int argc, char **argv) {
  if (argc < 3) {
    std::cerr << "Usage: " << argv[0] << " <model.onnx> <vad|classification|tagging|embedding>\n"
              << "       " << argv[0] << " --fbank <bins> <hanning:0|1>   (f32 samples on stdin,\n"
              << "         features to stdout — for numeric comparison against torchaudio kaldi fbank)"
              << std::endl;
    return 1;
  }

  if (!strcmp(argv[1], "--id")) {
    // Registry-id mode: resolve + auto-provision + create exactly like MistProcONNX's
    // single-file audio path (verifies provisioning end-to-end; set MIST_MODEL_DIR).
    std::string id = argv[2];
    const ONNX::ModelRegistryEntry *entry = ONNX::ModelRegistry::findModel(id);
    if (!entry || !entry->filename || !entry->filename[0] ||
        entry->modality != ONNX::ModelModality::AUDIO) {
      std::cerr << "'" << id << "' is not a single-file audio registry id" << std::endl;
      return 1;
    }
    std::string path = ONNX::ModelRegistry::resolveModelPath(id);
    if (path.empty()) {
      std::string hint;
      if (!ONNX::ModelRegistry::provision(id, hint)) {
        std::cerr << "provision failed: " << hint << std::endl;
        return 1;
      }
      path = ONNX::ModelRegistry::resolveModelPath(id);
      if (path.empty()) {
        std::cerr << "model did not resolve after provisioning" << std::endl;
        return 1;
      }
    }
    std::unique_ptr<ONNX::AudioModel> m =
        ONNX::ModelFactory::createAudioModel(path, entry->type, 1, "cpu", false);
    if (!m) {
      std::cerr << "createAudioModel failed" << std::endl;
      return 1;
    }
    std::cout << "provisioned+loaded: " << path << " (rate " << m->sampleRate() << ", chunk "
              << m->chunkSamples() << ", labels " << m->config().labels.size() << ")" << std::endl;
    return 0;
  }

  if (!strcmp(argv[1], "--fbank")) {
    int bins = atoi(argv[2]);
    bool hanning = argc > 3 && atoi(argv[3]) != 0;
    std::vector<float> samples;
    float buf[4096];
    size_t got;
    while ((got = fread(buf, sizeof(float), 4096, stdin)) > 0) { samples.insert(samples.end(), buf, buf + got); }
    std::vector<float> feats;
    size_t frames = ONNX::Utils::computeFbank(samples.data(), samples.size(), 16000, bins, hanning, feats);
    fprintf(stderr, "frames=%zu bins=%d\n", frames, bins);
    fwrite(feats.data(), sizeof(float), feats.size(), stdout);
    return frames ? 0 : 1;
  }
  ONNX::ModelType task;
  if (!strcmp(argv[2], "vad")) { task = ONNX::ModelType::AUDIO_VAD; }
  else if (!strcmp(argv[2], "classification")) { task = ONNX::ModelType::AUDIO_CLASSIFICATION; }
  else if (!strcmp(argv[2], "tagging")) { task = ONNX::ModelType::AUDIO_TAGGING; }
  else if (!strcmp(argv[2], "embedding")) { task = ONNX::ModelType::AUDIO_EMBEDDING; }
  else {
    std::cerr << "Unknown task '" << argv[2] << "'" << std::endl;
    return 1;
  }

  std::unique_ptr<ONNX::AudioModel> model =
      ONNX::ModelFactory::createAudioModel(argv[1], task, 1, "cpu", false);
  if (!model) {
    std::cerr << "createAudioModel failed for " << argv[1] << std::endl;
    return 1;
  }
  std::cout << "rate: " << model->sampleRate() << std::endl;
  std::cout << "chunk: " << model->chunkSamples() << " (0 = windowed)" << std::endl;
  std::cout << "labels: " << model->config().labels.size() << std::endl;
  std::cout << "norm: " << (model->config().zeroMeanUnitVar ? "zero-mean/unit-var" : "none") << std::endl;

  int rate = model->sampleRate();
  int fails = 0;

  if (model->chunkSamples() > 0) {
    // Streaming: one second of 220 Hz tone in fixed chunks, then the state-loop proof.
    size_t chunk = model->chunkSamples();
    std::vector<float> tone(chunk);
    for (size_t i = 0; i < chunk; ++i) { tone[i] = 0.5f * sinf(2.0f * 3.14159265f * 220.0f * i / rate); }

    model->reset();
    ONNX::AudioResult first = model->process(tone.data(), chunk, 0);
    ONNX::AudioResult second = model->process(tone.data(), chunk, 32);
    model->reset();
    ONNX::AudioResult redo = model->process(tone.data(), chunk, 0);

    if (!first.ok || !second.ok || !redo.ok) {
      std::cerr << "FAIL: chunk inference did not complete" << std::endl;
      ++fails;
    } else {
      float p1 = first.scores[0].confidence, p2 = second.scores[0].confidence,
            pr = redo.scores[0].confidence;
      std::cout << "vad probs: first=" << p1 << " second=" << p2 << " after-reset=" << pr << std::endl;
      if (p1 == p2) {
        std::cerr << "FAIL: identical input chunks gave identical output — state is NOT looping" << std::endl;
        ++fails;
      }
      if (p1 != pr) {
        std::cerr << "FAIL: reset did not reproduce the first-chunk output — reset broken" << std::endl;
        ++fails;
      }
    }
    // A stretch of silence should score lower than it does right after (fresh state)
    std::vector<float> silence(chunk, 0.0f);
    for (int i = 0; i < 10; ++i) { model->process(silence.data(), chunk, 64 + i * 32); }
    ONNX::AudioResult quiet = model->process(silence.data(), chunk, 400);
    if (quiet.ok) { std::cout << "vad prob after 10 silence chunks: " << quiet.scores[0].confidence << std::endl; }
  } else {
    // Windowed: one second of noise
    std::vector<float> noise(rate);
    unsigned seed = 12345;
    for (int i = 0; i < rate; ++i) {
      seed = seed * 1103515245 + 12345;
      noise[i] = ((int)(seed >> 16 & 0x7fff) - 16384) / 32768.0f;
    }
    ONNX::AudioResult r = model->process(noise.data(), noise.size(), 0);
    if (!r.ok) {
      std::cerr << "FAIL: window inference did not complete" << std::endl;
      ++fails;
    }
    std::cout << "window [" << r.startMs << "," << r.endMs << "]ms:" << std::endl;
    for (size_t i = 0; i < r.scores.size(); ++i) {
      std::cout << "  " << r.scores[i].class_name << " " << r.scores[i].confidence << std::endl;
    }
    if (!r.embedding.empty()) {
      float norm = 0.0f;
      for (size_t i = 0; i < r.embedding.size(); ++i) { norm += r.embedding[i] * r.embedding[i]; }
      std::cout << "  embedding dim=" << r.embedding.size() << " l2norm=" << norm << std::endl;
    }
  }

  if (fails) {
    std::cerr << fails << " audio checks failed" << std::endl;
    return 1;
  }
  std::cout << "Audio probe OK" << std::endl;
  return 0;
}

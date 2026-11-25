// Exercise the real session loader with a runtime that has a compiled-in OpenVINO
// provider and no external plugin. Session creation still uses a real CPU Identity model.
#include "../lib/onnx.h"

#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <unistd.h>

namespace {
  const OrtApi *realApi;
  unsigned builtinCalls = 0;
  unsigned sessionCreates = 0;
  unsigned unboundedProbes = 0;
  std::map<const OrtSessionOptions *, int> threads;
  bool rejectBuiltin = false;
  OrtStatus *ORT_API_CALL createSession(const OrtEnv *env, const ORTCHAR_T *path, const OrtSessionOptions *options,
                                        OrtSession **session) noexcept {
    ++sessionCreates;
    if (!threads.count(options) || threads.at(options) < 1) { ++unboundedProbes; }
    return realApi->CreateSession(env, path, options, session);
  }
  OrtStatus *ORT_API_CALL createOptions(OrtSessionOptions **options) noexcept {
    OrtStatus *status = realApi->CreateSessionOptions(options);
    if (!status) { threads[*options] = 0; }
    return status;
  }
  OrtStatus *ORT_API_CALL cloneOptions(const OrtSessionOptions *source, OrtSessionOptions **options) noexcept {
    OrtStatus *status = realApi->CloneSessionOptions(source, options);
    if (!status) { threads[*options] = threads[source]; }
    return status;
  }
  OrtStatus *ORT_API_CALL setThreads(OrtSessionOptions *options, int count) noexcept {
    threads[options] = count;
    return realApi->SetIntraOpNumThreads(options, count);
  }
  void ORT_API_CALL releaseOptions(OrtSessionOptions *options) noexcept {
    threads.erase(options);
    realApi->ReleaseSessionOptions(options);
  }
  OrtStatus *ORT_API_CALL appendBuiltin(OrtSessionOptions *, const char *name, const char *const *keys,
                                        const char *const *values, size_t count) noexcept {
    ++builtinCalls;
    if (strcmp(name, "OpenVINO") || count != 1 || strcmp(keys[0], "device_type") || strcmp(values[0], "AUTO")) {
      return realApi->CreateStatus(ORT_INVALID_ARGUMENT, "expected compiled-in OpenVINO with device_type=AUTO");
    }
    return rejectBuiltin ? realApi->CreateStatus(ORT_FAIL, "compiled-in OpenVINO unavailable") : nullptr;
  }
  const OrtApi *ORT_API_CALL getTestApi(uint32_t version) noexcept {
    realApi = OrtGetApiBase()->GetApi(version);
    static OrtApi table = *realApi;
    table.SessionOptionsAppendExecutionProvider = appendBuiltin;
    table.CreateSession = createSession;
    table.CreateSessionOptions = createOptions;
    table.CloneSessionOptions = cloneOptions;
    table.SetIntraOpNumThreads = setThreads;
    table.ReleaseSessionOptions = releaseOptions;
    return &table;
  }
  const OrtApiBase *testOrtGetApiBase() {
    static OrtApiBase base = *OrtGetApiBase();
    base.GetApi = getTestApi;
    return &base;
  }
} // namespace

#define OrtGetApiBase testOrtGetApiBase
#undef MIST_ONNX_PROFILE
#define MIST_ONNX_PROFILE "openvino"
#include "../lib/onnx.cpp"
#undef OrtGetApiBase

int main() {
  char dir[] = "/tmp/mist-openvino-fallback-XXXXXX";
  if (!mkdtemp(dir)) { return 1; }
  std::string path = std::string(dir) + "/identity.onnx";
  // IR8, opset13, FLOAT input/output of shape [1], Identity node.
  const unsigned char model[] = {8,   8,   58,  55,  10,  16, 10, 1,   120, 18, 1,  121, 34,  8,  73, 100, 101,
                                 110, 116, 105, 116, 121, 18, 1,  103, 90,  15, 10, 1,   120, 18, 10, 10,  8,
                                 8,   1,   18,  4,   10,  2,  8,  1,   98,  15, 10, 1,   121, 18, 10, 10,  8,
                                 8,   1,   18,  4,   10,  2,  8,  1,   66,  4,  10, 0,   16,  13};
  std::ofstream file(path, std::ios::binary);
  file.write(reinterpret_cast<const char *>(model), sizeof(model));
  file.close();
  int failures = 0;
  {
    sessionCreates = 0;
    ONNX::SessionRunner runner;
    runner.setFedShape([](size_t, const std::vector<int64_t> &) { return std::vector<int64_t>{1}; });
    std::string err;
    if (!runner.load(path, 1, "cpu", err) || sessionCreates != 1) {
      std::cerr << "FAIL: CPU vision load must create one session, got " << sessionCreates << ": " << err << '\n';
      ++failures;
    }
  }
  for (const std::string & requested : {std::string("openvino"), std::string("")}) {
    builtinCalls = 0;
    sessionCreates = 0;
    ONNX::SessionRunner runner;
    runner.setFedShape([](size_t, const std::vector<int64_t> &) { return std::vector<int64_t>{1}; });
    std::string err;
    if (!runner.load(path, 1, requested, err) || builtinCalls != 1 || runner.activeEP() != "OpenVINO") {
      std::cerr << "FAIL: plugin absent must try compiled-in OpenVINO once: " << err << '\n';
      ++failures;
    }
    if (sessionCreates != 1) {
      std::cerr << "FAIL: OpenVINO must reuse prior input metadata, got " << sessionCreates << " session creates\n";
      ++failures;
    }
  }
  {
    unsigned char changed[sizeof(model)];
    memcpy(changed, model, sizeof(model));
    for (size_t i = 0; i + 3 < sizeof(changed); ++i) {
      if (changed[i] == 10 && changed[i + 1] == 2 && changed[i + 2] == 8 && changed[i + 3] == 1) { changed[i + 3] = 2; }
    }
    std::ofstream replacement(path, std::ios::binary | std::ios::trunc);
    replacement.write(reinterpret_cast<const char *>(changed), sizeof(changed));
    replacement.close();
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
      sessionCreates = 0;
      ONNX::SessionRunner runner;
      runner.setFedShape([](size_t, const std::vector<int64_t> &) { return std::vector<int64_t>{2}; });
      std::string err;
      const unsigned expectedCreates = attempt ? 1 : 2;
      if (!runner.load(path, 1, "openvino", err) || sessionCreates != expectedCreates || runner.inputs().size() != 1 ||
          runner.inputs()[0].shape != std::vector<int64_t>{2}) {
        std::cerr << "FAIL: changed model metadata must refresh once, then be reused; attempt=" << attempt
                  << " creates=" << sessionCreates << " error=" << err << '\n';
        ++failures;
      }
    }
  }
  rejectBuiltin = true;
  builtinCalls = 0;
  {
    ONNX::SessionRunner runner;
    std::string err;
    if (runner.load(path, 1, "openvino", err) || builtinCalls != 1) {
      std::cerr << "FAIL: unavailable explicit OpenVINO must fail after both paths\n";
      ++failures;
    }
  }
  unlink(path.c_str());
  rmdir(dir);
  if (unboundedProbes) {
    std::cerr << "FAIL: metadata probes must set a bounded thread count, got " << unboundedProbes << " unbounded sessions\n";
    ++failures;
  }
  return failures ? 1 : 0;
}

// Exercise real SessionRunner loads with a deterministic EP cache boundary. ORT still
// parses and executes the Identity graph; the callback simulates compiled blob I/O.
#include "../lib/auth.h"
#include "../lib/json.h"
#include "../lib/onnx.h"

#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <spawn.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;
extern char **environ;
namespace {
  const OrtApi *realApi;
  std::map<const OrtSessionOptions *, std::string> cacheDirs;
  std::string runtimePath, cacheRoot, deviceVersion = "fixture-1";
  unsigned compiles = 0, cacheHits = 0, lockedHits = 0;
  bool rejectCompile = false;
  bool rewriteHit = false;
  int compilerReady = -1;
  uint64_t blobBytes = 64;
  int failures = 0;

  int testDladdr(const void *, Dl_info *info) {
    memset(info, 0, sizeof(*info));
    info->dli_fname = runtimePath.c_str();
    return 1;
  }

  OrtStatus *ORT_API_CALL createOptions(OrtSessionOptions **options) noexcept {
    OrtStatus *status = realApi->CreateSessionOptions(options);
    if (!status) { cacheDirs.erase(*options); }
    return status;
  }
  void ORT_API_CALL releaseOptions(OrtSessionOptions *options) noexcept {
    cacheDirs.erase(options);
    realApi->ReleaseSessionOptions(options);
  }
  OrtStatus *ORT_API_CALL addConfig(OrtSessionOptions *options, const char *key, const char *value) noexcept {
    try {
      if (std::string(key).find(".load_config") != std::string::npos) {
        JSON::Value config = JSON::fromString(value);
        cacheDirs[options] = config["AUTO"]["CACHE_DIR"].asString();
      }
      return realApi->AddSessionConfigEntry(options, key, value);
    } catch (const std::exception & error) { return realApi->CreateStatus(ORT_FAIL, error.what()); }
  }
  OrtStatus *ORT_API_CALL registerPlugin(OrtEnv *, const char *, const ORTCHAR_T *) noexcept {
    return nullptr;
  }
  OrtStatus *ORT_API_CALL getDevices(const OrtEnv *env, const OrtEpDevice *const **out, size_t *count) noexcept {
    const OrtEpDevice *const *devices = nullptr;
    size_t total = 0;
    if (OrtStatus *status = realApi->GetEpDevices(env, &devices, &total)) { return status; }
    static const OrtEpDevice *cpu;
    for (size_t i = 0; i < total; ++i) {
      if (!strcmp(realApi->EpDevice_EpName(devices[i]), "CPUExecutionProvider")) {
        cpu = devices[i];
        *out = &cpu;
        *count = 1;
        return nullptr;
      }
    }
    return realApi->CreateStatus(ORT_FAIL, "real CPU device missing");
  }
  const char *ORT_API_CALL deviceName(const OrtEpDevice *) noexcept {
    return "OpenVINOExecutionProvider.AUTO";
  }
  void ORT_API_CALL deviceMetadata(const OrtKeyValuePairs *, const char *const **keys, const char *const **values,
                                   size_t *count) noexcept {
    // MEMORY_STATISTICS is telemetry and DEVICE_LUID is a per-process handle (the Linux GPU
    // plugin reports a new one in every process); neither may change the cache key.
    static const char *names[] = {"ov_device", "version", "FULL_DEVICE_NAME", "MEMORY_STATISTICS", "DEVICE_LUID"};
    static const char *data[] = {"CPU", nullptr, "cache-fixture", nullptr, nullptr};
    static unsigned queries = 0;
    static std::string usage, luid;
    usage = std::to_string(++queries);
    luid = "40" + std::to_string(queries) + "c83906560000";
    data[1] = deviceVersion.c_str();
    data[3] = usage.c_str();
    data[4] = luid.c_str();
    *keys = names;
    *values = data;
    *count = 5;
  }
  OrtStatus *ORT_API_CALL appendPlugin(OrtSessionOptions *, OrtEnv *, const OrtEpDevice *const *, size_t,
                                       const char *const *, const char *const *, size_t) noexcept {
    return nullptr;
  }

  OrtStatus *ORT_API_CALL createSession(const OrtEnv *env, const ORTCHAR_T *path, const OrtSessionOptions *options,
                                        OrtSession **session) noexcept {
    try {
      auto configured = cacheDirs.find(options);
      if (configured == cacheDirs.end() || configured->second.empty()) {
        return realApi->CreateSession(env, path, options, session);
      }
      const std::string blob = configured->second + "/" + Secure::sha256(std::string(path)) + ".blob";
      const bool hit = fs::exists(blob);
      if (compilerReady >= 0) {
        if (write(compilerReady, "1", 1) != 1) {
          return realApi->CreateStatus(ORT_FAIL, "compiler readiness pipe failed");
        }
        close(compilerReady);
        compilerReady = -1;
        usleep(1500000);
      }
      if (hit) {
        ++cacheHits;
        const int probe = open((cacheRoot + "/.lock").c_str(), O_RDWR | O_CREAT, 0644);
        if (probe < 0 || flock(probe, LOCK_EX | LOCK_NB)) { ++lockedHits; }
        if (probe >= 0) { close(probe); }
        if (rewriteHit) { std::ofstream(blob, std::ios::binary | std::ios::trunc) << std::string(128, 'R'); }
      } else {
        ++compiles;
      }
      if (rejectCompile) {
        std::ofstream partial(blob, std::ios::binary);
        partial << "partial";
        return realApi->CreateStatus(ORT_FAIL, "injected compiler failure after a partial write");
      }
      OrtStatus *status = realApi->CreateSession(env, path, options, session);
      if (!status && !hit) {
        std::ofstream file(blob, std::ios::binary);
        file.seekp(blobBytes - 1);
        file.put('x');
      }
      return status;
    } catch (const std::exception & error) { return realApi->CreateStatus(ORT_FAIL, error.what()); }
  }

  const OrtApi *ORT_API_CALL getTestApi(uint32_t version) noexcept {
    realApi = OrtGetApiBase()->GetApi(version);
    static OrtApi table = *realApi;
    table.CreateSessionOptions = createOptions;
    table.ReleaseSessionOptions = releaseOptions;
    table.AddSessionConfigEntry = addConfig;
    table.RegisterExecutionProviderLibrary = registerPlugin;
    table.GetEpDevices = getDevices;
    table.EpDevice_EpName = deviceName;
    table.GetKeyValuePairs = deviceMetadata;
    table.SessionOptionsAppendExecutionProvider_V2 = appendPlugin;
    table.CreateSession = createSession;
    return &table;
  }
  const OrtApiBase *testOrtGetApiBase() {
    static OrtApiBase base = *OrtGetApiBase();
    base.GetApi = getTestApi;
    return &base;
  }
} // namespace

#define OrtGetApiBase testOrtGetApiBase
#define dladdr testDladdr
#undef MIST_ONNX_PROFILE
#define MIST_ONNX_PROFILE "openvino"
#include "../lib/onnx.cpp"
#undef dladdr
#undef OrtGetApiBase

#define CHECK(value)                                                \
  do {                                                              \
    if (!(value)) {                                                 \
      std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #value); \
      ++failures;                                                   \
    }                                                               \
  } while (0)

namespace {
  void writeModel(const std::string & path, unsigned dimension = 1) {
    unsigned char model[] = {8,   8,   58,  55,  10,  16, 10, 1,   120, 18, 1,  121, 34,  8,  73, 100, 101,
                             110, 116, 105, 116, 121, 18, 1,  103, 90,  15, 10, 1,   120, 18, 10, 10,  8,
                             8,   1,   18,  4,   10,  2,  8,  1,   98,  15, 10, 1,   121, 18, 10, 10,  8,
                             8,   1,   18,  4,   10,  2,  8,  1,   66,  4,  10, 0,   16,  13};
    for (size_t i = 0; i + 3 < sizeof(model); ++i) {
      if (model[i] == 10 && model[i + 1] == 2 && model[i + 2] == 8 && model[i + 3] == 1) { model[i + 3] = dimension; }
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char *>(model), sizeof(model));
  }
  bool load(const std::string & path, int threads = 1) {
    ONNX::SessionRunner runner;
    std::string error;
    if (!runner.load(path, threads, "openvino", error)) {
      if (!rejectCompile) { std::fprintf(stderr, "load failed: %s\n", error.c_str()); }
      return false;
    }
    const size_t count = runner.inputs()[0].shape[0];
    std::vector<float> data(count, 42);
    OrtValue *input = runner.createFloatTensor(data.data(), count, {static_cast<int64_t>(count)}, error);
    if (!input) { return false; }
    std::vector<OrtValue *> outputs;
    bool ok = runner.run({input}, outputs, error) && outputs.size() == 1;
    void *result = nullptr;
    if (ok) {
      OrtStatus *status = realApi->GetTensorMutableData(outputs[0], &result);
      ok = !status && result && static_cast<float *>(result)[0] == 42;
      if (status) { realApi->ReleaseStatus(status); }
    }
    for (OrtValue *output : outputs) { realApi->ReleaseValue(output); }
    realApi->ReleaseValue(input);
    return ok;
  }
  std::pair<size_t, uint64_t> blobs() {
    size_t count = 0;
    uint64_t bytes = 0;
    for (const auto & entry : fs::recursive_directory_iterator(cacheRoot)) {
      if (entry.is_regular_file() && entry.path().extension() == ".blob") {
        ++count;
        bytes += entry.file_size();
      }
    }
    return {count, bytes};
  }
} // namespace

int main(int argc, char **argv) {
  if (argc == 5 && std::string(argv[1]) == "--slow") {
    const std::string root(argv[2]);
    runtimePath = root + "/libonnxruntime.so";
    cacheRoot = root + "/models/openvino-cache";
    setenv("MIST_MODEL_DIR", (root + "/models").c_str(), 1);
    compilerReady = std::stoi(argv[4]);
    return load(argv[3]) ? 0 : 1;
  }
  char directory[] = "/tmp/mist-compile-cache-XXXXXX";
  if (!mkdtemp(directory)) { return 2; }
  const std::string root(directory), model = root + "/identity.onnx";
  runtimePath = root + "/libonnxruntime.so";
  std::ofstream(root + "/libonnxruntime_providers_openvino_plugin.so").put('x');
  setenv("MIST_MODEL_DIR", (root + "/models").c_str(), 1);
  cacheRoot = root + "/models/openvino-cache";
  writeModel(model);
  CHECK(load(model));
  CHECK(compiles == 1);
  CHECK(load(model));
  CHECK(compiles == 1 && cacheHits == 1);
  CHECK(lockedHits == 0);

  fs::path published;
  for (const auto & entry : fs::recursive_directory_iterator(cacheRoot)) {
    if (entry.is_regular_file() && entry.path().extension() == ".blob") {
      published = entry.path();
      break;
    }
  }
  const int mappedFile = open(published.c_str(), O_RDONLY);
  CHECK(mappedFile >= 0);
  void *mapped = mmap(nullptr, 64, PROT_READ, MAP_PRIVATE, mappedFile, 0);
  CHECK(mapped != MAP_FAILED);
  if (mapped != MAP_FAILED) {
    rewriteHit = true;
    CHECK(load(model));
    rewriteHit = false;
    struct stat held;
    CHECK(fstat(mappedFile, &held) == 0 && held.st_size == 64);
    CHECK(static_cast<unsigned char *>(mapped)[0] == 0);
    munmap(mapped, 64);
  }
  if (mappedFile >= 0) { close(mappedFile); }

  int ready[2];
  CHECK(pipe(ready) == 0);
  const std::string slowModel = root + "/slow.onnx", descriptor = std::to_string(ready[1]);
  writeModel(slowModel);
  char *args[] = {argv[0],
                  const_cast<char *>("--slow"),
                  const_cast<char *>(root.c_str()),
                  const_cast<char *>(slowModel.c_str()),
                  const_cast<char *>(descriptor.c_str()),
                  nullptr};
  posix_spawn_file_actions_t actions;
  CHECK(posix_spawn_file_actions_init(&actions) == 0);
  CHECK(posix_spawn_file_actions_addclose(&actions, ready[0]) == 0);
  pid_t compiler = -1;
  const int spawned = posix_spawn(&compiler, argv[0], &actions, nullptr, args, environ);
  CHECK(spawned == 0);
  posix_spawn_file_actions_destroy(&actions);
  close(ready[1]);
  if (!spawned) {
    char byte;
    CHECK(read(ready[0], &byte, 1) == 1 && byte == '1');
    CHECK(load(model));
    int status;
    const pid_t early = waitpid(compiler, &status, WNOHANG);
    CHECK(early == 0);
    if (!early) { CHECK(waitpid(compiler, &status, 0) == compiler); }
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  }
  close(ready[0]);

  const unsigned beforeChange = compiles;
  writeModel(model, 2);
  CHECK(load(model));
  CHECK(compiles == beforeChange + 1);
  deviceVersion = "fixture-2";
  CHECK(load(model));
  CHECK(compiles == beforeChange + 2);

  const std::string failed = root + "/failed.onnx";
  writeModel(failed);
  rejectCompile = true;
  CHECK(!load(failed));
  rejectCompile = false;
  const unsigned beforeRetry = compiles;
  CHECK(load(failed));
  CHECK(compiles == beforeRetry + 1);

  for (unsigned i = 0; i < 70; ++i) {
    const std::string path = root + "/model-" + std::to_string(i) + ".onnx";
    writeModel(path);
    CHECK(load(path));
  }
  CHECK(blobs().first <= 64);
  blobBytes = 513ULL * 1024 * 1024;
  const std::string large = root + "/large.onnx";
  writeModel(large);
  CHECK(load(large));
  CHECK(blobs().second <= 512ULL * 1024 * 1024);

  const std::string orphan = cacheRoot + "/.session-orphan", active = cacheRoot + "/.session-active";
  fs::create_directories(orphan);
  fs::create_directories(active);
  std::ofstream(orphan + "/.lease").put('x');
  const int lease = open((active + "/.lease").c_str(), O_CREAT | O_RDWR, 0644);
  CHECK(lease >= 0 && flock(lease, LOCK_EX | LOCK_NB) == 0);
  blobBytes = 64;
  CHECK(load(model));
  CHECK(!fs::exists(orphan) && fs::exists(active));
  close(lease);
  CHECK(load(model));
  CHECK(!fs::exists(active));
  CHECK(lockedHits == 0);
  fs::remove_all(root);
  return failures ? 1 : 0;
}

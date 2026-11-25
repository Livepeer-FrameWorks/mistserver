// Exercise the real session loader with a runtime that has a compiled-in OpenVINO
// provider and no external plugin. Session creation still uses a real CPU Identity model.
#include "../lib/onnx.h"

#include <cstring>
#include <fstream>
#include <iostream>
#include <unistd.h>

namespace {
  const OrtApi *realApi;
  unsigned builtinCalls = 0;
  bool rejectBuiltin = false;
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
  for (const std::string & requested : {std::string("openvino"), std::string("")}) {
    builtinCalls = 0;
    ONNX::SessionRunner runner;
    std::string err;
    if (!runner.load(path, 1, requested, err) || builtinCalls != 1 || runner.activeEP() != "OpenVINO") {
      std::cerr << "FAIL: plugin absent must try compiled-in OpenVINO once: " << err << '\n';
      ++failures;
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
  return failures ? 1 : 0;
}

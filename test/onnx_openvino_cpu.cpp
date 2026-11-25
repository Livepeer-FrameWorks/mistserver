#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include "onnxruntime_c_api.h"
#include "onnxruntime_session_options_config_keys.h"

int main(int argc, char **argv) {
  if (argc != 2) { return 2; }
  std::printf("ORT_RUNTIME_VERSION %s\n", OrtGetApiBase()->GetVersionString());
  if (std::strcmp(OrtGetApiBase()->GetVersionString(), "1.32.0") != 0) { return 2; }
  const OrtApi *api = OrtGetApiBase()->GetApi(ORT_API_VERSION);
  if (!api) { return 2; }
  auto check = [&](OrtStatus *status) {
    if (!status) { return; }
    std::fprintf(stderr, "ORT_ERROR %s\n", api->GetErrorMessage(status));
    api->ReleaseStatus(status);
    std::exit(2);
  };
  OrtEnv *env = nullptr;
  check(api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "o3-amd", &env));
  check(api->RegisterExecutionProviderLibrary(env, "openvino", argv[1]));
  const OrtEpDevice *const *devices = nullptr;
  size_t count = 0;
  check(api->GetEpDevices(env, &devices, &count));
  for (size_t i = 0; i < count; ++i) {
    const char *name = api->EpDevice_EpName(devices[i]);
    std::printf("EP_DEVICE index=%zu factory=%s\n", i, name ? name : "(unnamed)");
  }
  const OrtEpDevice *chosen = nullptr;
  for (const char *factory : {"OpenVINOExecutionProvider.AUTO", "OpenVINOExecutionProvider"}) {
    for (size_t i = 0; i < count; ++i) {
      const char *name = api->EpDevice_EpName(devices[i]);
      if (!name || std::strcmp(name, factory) != 0) { continue; }
      const char *const *keys = nullptr, *const *values = nullptr;
      size_t pairs = 0;
      api->GetKeyValuePairs(api->EpDevice_EpMetadata(devices[i]), &keys, &values, &pairs);
      for (size_t k = 0; k < pairs; ++k) {
        std::printf("OPENVINO_DEVICE factory=%s %s=%s\n", name, keys[k], values[k]);
        if (std::strcmp(keys[k], "ov_device") == 0 && std::strcmp(values[k], "CPU") == 0) { chosen = devices[i]; }
      }
    }
    if (chosen) { break; }
  }
  OrtSessionOptions *options = nullptr;
  check(api->CreateSessionOptions(&options));
  check(api->SetIntraOpNumThreads(options, 1));
  check(api->SetInterOpNumThreads(options, 1));
  check(api->SetSessionGraphOptimizationLevel(options, ORT_DISABLE_ALL));
  check(api->AddSessionConfigEntry(options, kOrtSessionOptionsDisableCPUEPFallback, "1"));
  if (chosen) {
    check(api->SessionOptionsAppendExecutionProvider_V2(options, env, &chosen, 1, nullptr, nullptr, 0));
    std::puts("O3_PATH plugin CPU device");
  } else {
    std::puts("O3_PLUGIN_NO_AMD_CPU_DEVICE trying compiled-in OpenVINO AUTO");
    const char *keys[] = {"device_type"}, *values[] = {"AUTO"};
    check(api->SessionOptionsAppendExecutionProvider(options, "OpenVINO", keys, values, 1));
    std::puts("O3_PATH compiled-in OpenVINO AUTO");
  }
  // IR v8, opset 13: Add(float[1] x, float[1] z) -> float[1] y.
  const unsigned char model[] = {8,   8,   58, 79, 10,  14, 10, 1,  120, 10,  1,  122, 18,  1,   121, 34, 3,   65,
                                 100, 100, 18, 10, 111, 51, 45, 97, 109, 100, 45, 97,  100, 100, 90,  15, 10,  1,
                                 120, 18,  10, 10, 8,   8,  1,  18, 4,   10,  2,  8,   1,   90,  15,  10, 1,   122,
                                 18,  10,  10, 8,  8,   1,  18, 4,  10,  2,   8,  1,   98,  15,  10,  1,  121, 18,
                                 10,  10,  8,  8,  1,   18, 4,  10, 2,   8,   1,  66,  4,   10,  0,   16, 13};
  OrtSession *session = nullptr;
  check(api->CreateSessionFromArray(env, model, sizeof(model), options, &session));
  OrtMemoryInfo *memory = nullptr;
  check(api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &memory));
  float left = 2, right = 3;
  int64_t shape[] = {1};
  OrtValue *inputs[2] = {nullptr, nullptr};
  check(api->CreateTensorWithDataAsOrtValue(memory, &left, sizeof(left), shape, 1, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &inputs[0]));
  check(api->CreateTensorWithDataAsOrtValue(memory, &right, sizeof(right), shape, 1,
                                            ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &inputs[1]));
  const char *inputNames[] = {"x", "z"}, *outputNames[] = {"y"};
  const OrtValue *values[] = {inputs[0], inputs[1]};
  OrtValue *output = nullptr;
  check(api->Run(session, nullptr, inputNames, values, 2, outputNames, 1, &output));
  void *data = nullptr;
  check(api->GetTensorMutableData(output, &data));
  const bool passed = data && *static_cast<float *>(data) == 5;
  if (!passed) { std::fprintf(stderr, "FAIL Add inference did not return five\n"); }
  api->ReleaseValue(output);
  api->ReleaseValue(inputs[0]);
  api->ReleaseValue(inputs[1]);
  api->ReleaseMemoryInfo(memory);
  api->ReleaseSession(session);
  api->ReleaseSessionOptions(options);
  api->ReleaseEnv(env);
  if (passed) { std::puts("OPENVINO_CPU_INFERENCE_VERIFIED"); }
  return passed ? 0 : 1;
}

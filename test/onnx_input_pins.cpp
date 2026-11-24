// Unit test for pinning the symbolic input dims of sessions whose input Mist builds itself
// (ORTHelpers::planInputPins, SessionRunner::setFedShape, DetectionModel::initialize), and
// for the OpenVINO compiled-model cache directory and its cross-process lock
// (ORTHelpers::openVINOCacheDir, ORTHelpers::lockOpenVINOCache). The
// OpenVINO NPU compiler only takes static shapes, so a vision model exported with a
// dynamic batch must reach the session with every input dim fixed.
//
// The models are tiny Identity graphs written as ONNX protobuf bytes, so the test needs
// no model download and runs on the CPU provider of every build profile.
#include "../lib/onnx.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

static int fails = 0;
#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::cerr << "FAIL (line " << __LINE__ << "): " #cond << std::endl; \
      ++fails;                                                            \
    }                                                                     \
  } while (0)

namespace {
  // Minimal protobuf writer for the handful of ONNX messages the test models need.
  std::string varint(uint64_t v) {
    std::string out;
    while (v >= 0x80) {
      out += (char)((v & 0x7F) | 0x80);
      v >>= 7;
    }
    out += (char)v;
    return out;
  }
  std::string intField(int field, uint64_t v) {
    return varint((uint64_t)field << 3) + varint(v);
  }
  std::string bytesField(int field, const std::string & bytes) {
    return varint(((uint64_t)field << 3) | 2) + varint(bytes.size()) + bytes;
  }

  // A dim: "" = dynamic without a name, digits = fixed size, anything else = dim_param.
  std::string dimension(const std::string & dim) {
    if (dim.empty()) { return bytesField(1, ""); }
    if (dim.find_first_not_of("0123456789") == std::string::npos) {
      return bytesField(1, intField(1, std::stoull(dim)));
    }
    return bytesField(1, bytesField(2, dim));
  }

  std::string valueInfo(const std::string & name, const std::vector<std::string> & dims) {
    std::string shape;
    for (const std::string & dim : dims) { shape += dimension(dim); }
    std::string tensorType = intField(1, 1 /* FLOAT */) + bytesField(2, shape);
    return bytesField(1, name) + bytesField(2, bytesField(1, tensorType));
  }

  struct Port {
      std::string name;
      std::vector<std::string> dims;
  };

  // One Identity node per input; output i mirrors input i.
  std::string identityModel(const std::vector<Port> & inputs) {
    std::string graph;
    for (const Port & in : inputs) {
      graph += bytesField(1, bytesField(1, in.name) + bytesField(2, in.name + "_out") + bytesField(4, "Identity"));
    }
    graph += bytesField(2, "pins");
    for (const Port & in : inputs) { graph += bytesField(11, valueInfo(in.name, in.dims)); }
    for (const Port & in : inputs) { graph += bytesField(12, valueInfo(in.name + "_out", in.dims)); }
    std::string opset = bytesField(1, "") + intField(2, 13);
    return intField(1, 8) + bytesField(7, graph) + bytesField(8, opset);
  }

  void writeFile(const std::string & path, const std::string & content) {
    std::ofstream f(path.c_str(), std::ios::binary);
    f << content;
  }

  bool hasOverride(const ONNX::ORTHelpers::InputPins & pins, const std::string & name, int64_t value) {
    for (const auto & pin : pins.overrides) {
      if (pin.first == name && pin.second == value) { return true; }
    }
    return false;
  }

  // Exposes the session of the vision adapter to check what initialize() created.
  class PinProbeModel : public ONNX::GenericModel {
    public:
      PinProbeModel(const std::string & path, int inputSize) : GenericModel(path, inputSize) {}
      const std::vector<ONNX::TensorSpec> & sessionInputs() const { return runner_.inputs(); }
  };

  std::vector<int64_t> nchw(int64_t h, int64_t w) {
    return {1, 3, h, w};
  }
} // namespace

int main() {
  std::string base = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
  if (base[base.size() - 1] != '/') { base += "/"; }
  std::string tmpl = base + "onnx_input_pins_XXXXXX";
  std::vector<char> buf(tmpl.begin(), tmpl.end());
  buf.push_back('\0');
  if (!mkdtemp(buf.data())) {
    std::cerr << "mkdtemp failed" << std::endl;
    return 1;
  }
  std::string dir(buf.data());

  const std::string vision = dir + "/vision.onnx";
  writeFile(vision, identityModel({{"images", {"batch_size", "3", "height", "width"}}}));
  const std::string unnamed = dir + "/unnamed.onnx";
  writeFile(unnamed, identityModel({{"images", {"", "3", "32", "32"}}}));
  const std::string shared = dir + "/shared.onnx";
  writeFile(shared, identityModel({{"a", {"n", "3"}}, {"b", {"n", "5"}}}));

  ONNX::ORTHelpers::FedShape fedVision = [](size_t, const std::vector<int64_t> &) { return nchw(32, 48); };

  // The probe reads every declared dim and its dim_param name.
  std::vector<ONNX::ORTHelpers::DeclaredInput> declared;
  CHECK(ONNX::ORTHelpers::probeInputs(vision, declared));
  CHECK(declared.size() == 1);
  if (declared.size() == 1) {
    CHECK(declared[0].name == "images");
    CHECK(declared[0].shape == std::vector<int64_t>({-1, 3, -1, -1}));
    CHECK(declared[0].dimNames == std::vector<std::string>({"batch_size", "", "height", "width"}));

    // Without fed shapes the model stays dynamic, so the NPU check excludes it.
    ONNX::ORTHelpers::InputPins none = ONNX::ORTHelpers::planInputPins(declared, ONNX::ORTHelpers::FedShape());
    CHECK(none.overrides.empty());
    CHECK(none.firstDynamic == "images");

    ONNX::ORTHelpers::InputPins pins = ONNX::ORTHelpers::planInputPins(declared, fedVision);
    CHECK(pins.overrides.size() == 3);
    CHECK(hasOverride(pins, "batch_size", 1));
    CHECK(hasOverride(pins, "height", 32));
    CHECK(hasOverride(pins, "width", 48));
    CHECK(pins.firstDynamic.empty());
    CHECK(pins.unpinned.empty());
  }

  // A session loaded with fed shapes reports the pinned static input shape.
  {
    ONNX::SessionRunner runner;
    runner.setFedShape(fedVision);
    std::string err;
    CHECK(runner.load(vision, 1, "cpu", err));
    CHECK(runner.inputs().size() == 1 && runner.inputs()[0].shape == nchw(32, 48));
  }
  {
    ONNX::SessionRunner runner;
    std::string err;
    CHECK(runner.load(vision, 1, "cpu", err));
    CHECK(runner.inputs().size() == 1 && runner.inputs()[0].shape == std::vector<int64_t>({-1, 3, -1, -1}));
  }

  // The vision adapter pins batch 1 and its configured input size before session creation.
  {
    PinProbeModel model(vision, 64);
    CHECK(model.initialize(1));
    CHECK(model.sessionInputs().size() == 1 && model.sessionInputs()[0].shape == nchw(64, 64));
    CHECK(model.getInputHeight() == 64 && model.getInputWidth() == 64);
  }

  // A dynamic dim without a dim_param name cannot be pinned by name and stays dynamic.
  CHECK(ONNX::ORTHelpers::probeInputs(unnamed, declared));
  if (declared.size() == 1) {
    ONNX::ORTHelpers::InputPins pins = ONNX::ORTHelpers::planInputPins(declared, fedVision);
    CHECK(pins.overrides.empty());
    CHECK(pins.firstDynamic == "images");
    CHECK(pins.unpinned.size() == 1);
  }

  // A dim_param shared by dims fed different sizes is left free rather than mis-pinned.
  CHECK(ONNX::ORTHelpers::probeInputs(shared, declared));
  CHECK(declared.size() == 2);
  if (declared.size() == 2) {
    ONNX::ORTHelpers::FedShape fedApart = [](size_t index, const std::vector<int64_t> & d) {
      std::vector<int64_t> shape(d);
      shape[0] = index == 0 ? 1 : 2;
      return shape;
    };
    ONNX::ORTHelpers::InputPins apart = ONNX::ORTHelpers::planInputPins(declared, fedApart);
    CHECK(apart.overrides.empty());
    CHECK(apart.firstDynamic == "a");
    CHECK(apart.unpinned.size() == 2);
    ONNX::ORTHelpers::FedShape fedOnes = [](size_t, const std::vector<int64_t> & d) {
      return std::vector<int64_t>(d.size(), 1);
    };
    ONNX::ORTHelpers::InputPins together = ONNX::ORTHelpers::planInputPins(declared, fedOnes);
    CHECK(together.overrides.size() == 1 && hasOverride(together, "n", 1));
    CHECK(together.firstDynamic.empty());
  }

  // The OpenVINO cache lives in the model directory and is created when missing.
  std::string why;
  setenv("MIST_MODEL_DIR", (dir + "/models/nested").c_str(), 1);
  std::string cache = ONNX::ORTHelpers::openVINOCacheDir(why);
  CHECK(cache == dir + "/models/nested/openvino-cache");
  CHECK(why.empty());
  CHECK(Util::isDirectory(dir + "/models/nested/openvino-cache"));
  CHECK(ONNX::ORTHelpers::openVINOCacheDir(why) == cache);

  // Session creation on the cache is exclusive across processes, with a bounded wait.
  {
    int ready[2];
    CHECK(pipe(ready) == 0);
    pid_t child = fork();
    if (child == 0) {
      std::string childWhy;
      int held = ONNX::ORTHelpers::lockOpenVINOCache(cache, 1000, childWhy);
      char byte = held >= 0 ? '1' : '0';
      if (write(ready[1], &byte, 1) != 1) { _exit(2); }
      usleep(1500 * 1000);
      _exit(held >= 0 ? 0 : 1);
    }
    char byte = 0;
    CHECK(read(ready[0], &byte, 1) == 1 && byte == '1');
    CHECK(ONNX::ORTHelpers::lockOpenVINOCache(cache, 200, why) < 0);
    CHECK(why.find("timed out") != std::string::npos);
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    int held = ONNX::ORTHelpers::lockOpenVINOCache(cache, 200, why);
    CHECK(held >= 0);
    if (held >= 0) { close(held); }
    close(ready[0]);
    close(ready[1]);
  }

  // An uncreatable directory yields no cache and a reason instead of failing.
  writeFile(dir + "/plainfile", "x");
  setenv("MIST_MODEL_DIR", (dir + "/plainfile/models").c_str(), 1);
  CHECK(ONNX::ORTHelpers::openVINOCacheDir(why).empty());
  CHECK(!why.empty());
  setenv("MIST_MODEL_DIR", (dir + "/plainfile").c_str(), 1);
  CHECK(ONNX::ORTHelpers::openVINOCacheDir(why).empty());
  CHECK(!why.empty());

  // A directory that exists but cannot be written also yields no cache (root bypasses modes).
  if (geteuid() != 0) {
    mkdir((dir + "/readonly").c_str(), 0755);
    mkdir((dir + "/readonly/openvino-cache").c_str(), 0755);
    chmod((dir + "/readonly/openvino-cache").c_str(), 0555);
    setenv("MIST_MODEL_DIR", (dir + "/readonly").c_str(), 1);
    CHECK(ONNX::ORTHelpers::openVINOCacheDir(why).empty());
    CHECK(why.find("cannot write") != std::string::npos);
    chmod((dir + "/readonly/openvino-cache").c_str(), 0755);
  }

  if (system(("rm -rf '" + dir + "'").c_str()) != 0) { std::cerr << "cleanup of " << dir << " failed" << std::endl; }
  if (fails) {
    std::cerr << fails << " input pin checks failed" << std::endl;
    return 1;
  }
  std::cout << "All input pin checks passed" << std::endl;
  return 0;
}

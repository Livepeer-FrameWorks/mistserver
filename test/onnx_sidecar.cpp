// Unit test for ONNX model sidecar parsing: class labels (labels.txt / HuggingFace
// config.json "id2label") and preprocessor_config.json discovery next to a model path.
// See ONNX::Utils::loadModelSidecars for the lookup order under test. The loaders read
// only the sidecar files; the multi-model-directory guard also counts sibling .onnx
// files, so those cases write empty dummy .onnx files.
#include "../lib/onnx.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

static int fails = 0;
#define CHECK(cond)                                                                                \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      std::cerr << "FAIL (line " << __LINE__ << "): " #cond << std::endl;                          \
      ++fails;                                                                                     \
    }                                                                                              \
  } while (0)

static void writeFile(const std::string &path, const std::string &content) {
  std::ofstream f(path.c_str());
  f << content;
}

int main() {
  std::string base = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
  if (base[base.size() - 1] != '/') { base += "/"; }
  std::string tmpl = base + "onnx_sidecar_XXXXXX";
  std::vector<char> buf(tmpl.begin(), tmpl.end());
  buf.push_back('\0');
  if (!mkdtemp(buf.data())) {
    std::cerr << "mkdtemp failed" << std::endl;
    return 1;
  }
  std::string dir(buf.data());
  dir += "/";

  // 1) stem-prefixed labels file: one label per line, CRLF stripped, trailing blanks dropped
  writeFile(dir + "modelA.labels.txt", "cat\ndog\r\n\n");
  ONNX::Utils::SidecarConfig a = ONNX::Utils::loadModelSidecars(dir + "modelA.onnx");
  CHECK(a.labels.size() == 2);
  CHECK(a.labels[0] == "cat");
  CHECK(a.labels[1] == "dog");
  CHECK(!a.hasPreproc);

  // 2) HF layout: config.json id2label + preprocessor_config.json with mean/std + size
  writeFile(dir + "config.json", "{\"id2label\":{\"0\":\"normal\",\"1\":\"nsfw\"}}");
  writeFile(dir + "preprocessor_config.json",
            "{\"image_mean\":[0.5,0.5,0.5],\"image_std\":[0.5,0.5,0.5],"
            "\"size\":{\"height\":224,\"width\":224},\"do_normalize\":true,\"do_rescale\":true}");
  ONNX::Utils::SidecarConfig b = ONNX::Utils::loadModelSidecars(dir + "model.onnx");
  CHECK(b.labels.size() == 2);
  CHECK(b.labels[0] == "normal");
  CHECK(b.labels[1] == "nsfw");
  CHECK(b.hasPreproc);
  CHECK(b.preproc.resizeMode == ONNX::PreprocessConfig::DIRECT_RESIZE);
  CHECK(b.preproc.normMode == ONNX::PreprocessConfig::IMAGENET);
  CHECK(b.preproc.mean[0] > 0.49f && b.preproc.mean[0] < 0.51f);
  CHECK(b.preproc.std[2] > 0.49f && b.preproc.std[2] < 0.51f);
  CHECK(b.inputSize == 224);

  // stem-prefixed labels still win over the generic config.json for modelA
  ONNX::Utils::SidecarConfig a2 = ONNX::Utils::loadModelSidecars(dir + "modelA.onnx");
  CHECK(a2.labels.size() == 2);
  CHECK(a2.labels[0] == "cat");

  // 3) preprocessor without mean/std -> plain 0-1 scaling; shortest_edge sizing means
  // the HF scale-short-edge + center-crop convention (CLIP), not a direct resize
  writeFile(dir + "modelC.preprocessor.json", "{\"do_rescale\":true,\"size\":{\"shortest_edge\":256}}");
  ONNX::Utils::SidecarConfig c = ONNX::Utils::loadModelSidecars(dir + "modelC.onnx");
  CHECK(c.hasPreproc);
  CHECK(c.preproc.normMode == ONNX::PreprocessConfig::SCALE_01);
  CHECK(c.preproc.resizeMode == ONNX::PreprocessConfig::CENTER_CROP);
  CHECK(c.inputSize == 256);

  // 4) no sidecars at all -> empty config
  ONNX::Utils::SidecarConfig d = ONNX::Utils::loadModelSidecars(dir + "nosuchdir/none.onnx");
  CHECK(d.labels.empty());
  CHECK(!d.hasPreproc);
  CHECK(d.inputSize == 0);

  // 5) shared multi-model directory: the generic HF-named sidecars must NOT apply
  // (a stray HF download would rewire every model in the dir), while labels.txt is
  // still shared and <stem>-prefixed sidecars still apply.
  std::string multi = dir + "multi/";
  mkdir(multi.c_str(), 0755);
  writeFile(multi + "one.onnx", "x");
  writeFile(multi + "two.onnx", "x");
  writeFile(multi + "config.json", "{\"id2label\":{\"0\":\"polluted\"}}");
  writeFile(multi + "preprocessor_config.json",
            "{\"image_mean\":[0.5,0.5,0.5],\"image_std\":[0.5,0.5,0.5],"
            "\"size\":{\"height\":224,\"width\":224},\"do_normalize\":true}");
  ONNX::Utils::SidecarConfig e = ONNX::Utils::loadModelSidecars(multi + "one.onnx");
  CHECK(e.labels.empty());
  CHECK(!e.hasPreproc);
  writeFile(multi + "labels.txt", "shared_a\nshared_b\n");
  ONNX::Utils::SidecarConfig e2 = ONNX::Utils::loadModelSidecars(multi + "two.onnx");
  CHECK(e2.labels.size() == 2);
  CHECK(e2.labels[0] == "shared_a");
  CHECK(!e2.hasPreproc);
  writeFile(multi + "one.preprocessor.json", "{\"do_rescale\":true,\"size\":{\"height\":96,\"width\":96}}");
  ONNX::Utils::SidecarConfig e3 = ONNX::Utils::loadModelSidecars(multi + "one.onnx");
  CHECK(e3.hasPreproc);
  CHECK(e3.inputSize == 96);

  // 6) malformed id2label keys must not clobber class 0 (atoi("LABEL_1") == 0)
  std::string single = dir + "single/";
  mkdir(single.c_str(), 0755);
  writeFile(single + "config.json",
            "{\"id2label\":{\"LABEL_1\":\"junk\",\"0\":\"normal\",\"1\":\"nsfw\"}}");
  ONNX::Utils::SidecarConfig f = ONNX::Utils::loadModelSidecars(single + "model.onnx");
  CHECK(f.labels.size() == 2);
  CHECK(f.labels[0] == "normal");
  CHECK(f.labels[1] == "nsfw");

  const char *cleanup[] = {"modelA.labels.txt", "config.json", "preprocessor_config.json",
                           "modelC.preprocessor.json", "multi/one.onnx", "multi/two.onnx",
                           "multi/config.json", "multi/preprocessor_config.json",
                           "multi/labels.txt", "multi/one.preprocessor.json",
                           "single/config.json"};
  for (size_t i = 0; i < sizeof(cleanup) / sizeof(cleanup[0]); ++i) {
    unlink((dir + cleanup[i]).c_str());
  }
  rmdir((dir + "multi").c_str());
  rmdir((dir + "single").c_str());
  rmdir(buf.data());

  if (fails) {
    std::cerr << fails << " sidecar checks failed" << std::endl;
    return 1;
  }
  std::cout << "All sidecar checks passed" << std::endl;
  return 0;
}

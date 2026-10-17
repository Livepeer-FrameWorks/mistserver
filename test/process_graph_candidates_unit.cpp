#include "../src/process_graph.h"

#include <mist/defines.h>
#include <mist/dtsc.h>
#include <mist/json.h>

#include <cstdio>
#include <string>
#include <unistd.h>

// The candidate metadata a buffer resolves its processing graph with, and a recording waits for
// its expected outputs with, is rebuilt on every buffer tick and recording track check. It is
// sized for its candidates when it is created, so building it never grows its track list (which
// logged "Resizing metadata track list" for every check of a stream with more than four tracks).
namespace {
  int failures = 0;

  void check(bool ok, const char *what) {
    if (!ok) {
      fprintf(stderr, "FAIL: %s\n", what);
      ++failures;
    }
  }

  void addTrack(DTSC::Meta & M, const char *type, const char *codec, const std::string & outputKey) {
    const size_t idx = M.addTrack();
    M.setType(idx, type);
    M.setCodec(idx, codec);
    M.setID(idx, idx + 1);
    if (outputKey.size()) { M.setOutputKey(idx, outputKey); }
  }

  /// Runs fn with stderr captured and returns what it logged.
  template<typename F> std::string captureLog(F fn) {
    fflush(stderr);
    FILE *capture = tmpfile();
    if (!capture) { return "tmpfile failed"; }
    const int saved = dup(2);
    dup2(fileno(capture), 2);
    fn();
    fflush(stderr);
    dup2(saved, 2);
    close(saved);
    std::string logged;
    rewind(capture);
    char buffer[4096];
    size_t len;
    while ((len = fread(buffer, 1, sizeof(buffer), capture)) > 0) { logged.append(buffer, len); }
    fclose(capture);
    return logged;
  }
} // namespace

int main() {
  Util::printDebugLevel = DLVL_INFO;
  DTSC::Meta M;
  M.reInit("", true);
  addTrack(M, "video", "H264", "");
  addTrack(M, "audio", "AAC", "");
  addTrack(M, "audio", "opus", "");
  addTrack(M, "video", "JPEG", "");
  addTrack(M, "video", "H264", "proc:a/720p");
  addTrack(M, "video", "H264", "proc:a/480p");
  check(M.getValidTracks().size() == 6, "the stream has six tracks");

  // Shaped like the graph's outputs: a process's declared output (type, codec, validity mask)
  // with the key, original and data fields declaredNodeOutputs adds.
  JSON::Value expected;
  for (const char *name : {"proc:a/360p", "proc:b/thumbs"}) {
    JSON::Value output;
    output["key"] = name;
    output["type"] = "video";
    output["codec"] = "H264";
    output["mask"] = TRACK_VALID_ALL;
    output["original"] = false;
    output["data"] = true;
    expected.append(output);
  }
  std::map<std::string, std::string> targetParams;
  targetParams["video"] = "all";
  targetParams["audio"] = "all";
  JSON::Value capa;
  capa["codecs"][0u][0u].append("H264");
  capa["codecs"][0u][1u].append("AAC");

  size_t selected = 0;
  size_t ready = 0;
  const std::string recordingLog =
    captureLog([&]() { Mist::recordingExpectedOutputs(M, expected, targetParams, capa, "", selected, ready); });
  check(recordingLog.find("Resizing metadata track list") == std::string::npos,
        "a recording's expected-output check does not grow its candidate track list");
  check(selected == 2, "the recording selects both expected outputs");

  DTSC::Meta C;
  std::map<size_t, std::string> producers;
  const std::string bufferLog = captureLog([&]() {
    producers =
      Mist::buildCandidates(C, M, std::map<std::string, std::map<std::string, JSON::Value>>(), std::set<std::string>());
  });
  check(bufferLog.find("Resizing metadata track list") == std::string::npos, "the buffer's graph candidates do not grow their track list");
  check(C.getValidTracks().size() == 6, "every existing track is a candidate");
  check(producers.size() == 2, "both process outputs are attributed to their process");

  if (failures) {
    fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  fprintf(stderr, "process graph candidates: OK\n");
  return 0;
}

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
  M.setRotation(0, 90);
  M.setMirror(0, true);
  DTSC::Meta rotatedCandidates;
  rotatedCandidates.reInit("", true);
  const size_t rotated = Mist::addCandidateTrack(rotatedCandidates, Mist::describeExistingTrack(M, 0, true));
  check(rotatedCandidates.getMirror(rotated), "process graph candidates preserve source mirror");
  check(rotatedCandidates.getRotation(rotated) == 90, "process graph candidates preserve source rotation");

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
  const std::string recordingLog = captureLog([&]() {
    Mist::recordingExpectedOutputs(M, Mist::tracksWithDataAnyMask(M), expected, targetParams, capa, "", selected, ready);
  });
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

  {
    DTSC::Meta source;
    source.reInit("", true);
    addTrack(source, "audio", "AAC", "");
    addTrack(source, "video", "H264", "");
    addTrack(source, "audio", "opus", "proc:audio/output");
    source.update(0, 0, 0, 1, 0, true, 1);
    source.update(0, 0, 2, 1, 0, true, 1);
    source.validateTrack(1, TRACK_VALID_INT_PROCESS);
    for (const size_t track : source.getValidTracks()) { source.breakClaim(track); }
    const uint8_t mask = DTSC::trackValidMask;
    DTSC::trackValidMask = TRACK_VALID_EXT_PUSH;
    const std::set<size_t> first = Mist::processGraphOriginalTracks(source, true);
    check(first == std::set<size_t>{0}, "graph coverage excludes header-only video and reserved process outputs");
    check(Mist::processGraphOriginalTracks(source, false) == std::set<size_t>({0, 1}),
          "the recording gate also sees masked source originals with no first packet yet");
    source.update(0, 0, 1, 1, 0, true, 1);
    check(first == std::set<size_t>{0}, "a later first packet cannot expand an already captured graph snapshot");
    check(Mist::processGraphOriginalTracks(source, true) == std::set<size_t>({0, 1}),
          "the next graph snapshot includes a masked original once its data arrives");
    check(Mist::renditionDecisionKey(source).find("1:") != std::string::npos, "a masked original changes the rendition-decision identity");
    check(DTSC::trackValidMask == TRACK_VALID_EXT_PUSH, "source snapshots preserve the caller visibility mask");
    DTSC::trackValidMask = mask;
  }

  {
    DTSC::Meta source;
    source.reInit("", true);
    addTrack(source, "video", "H264", "proc:a/360p");
    source.setWidth(0, 640);
    source.setHeight(0, 360);
    source.update(0, 0, 0, 1, 0, true, 1);
    source.breakClaim(0);
    JSON::Value expected;
    JSON::Value output = Mist::describeExistingTrack(source, 0, true);
    expected.append(output);
    std::set<std::string> acknowledged;
    size_t selected = 0, ready = 0;
    Mist::recordingExpectedOutputs(source, Mist::tracksWithDataAnyMask(source), expected, targetParams, capa, "",
                                   selected, ready, &acknowledged);
    check(selected == 1 && ready == 0, "a first output packet cannot bypass the buffer's visibility update");
    acknowledged.insert("proc:a/360p");
    Mist::recordingExpectedOutputs(source, Mist::tracksWithDataAnyMask(source), expected, targetParams, capa, "",
                                   selected, ready, &acknowledged);
    check(selected == 1 && ready == 1, "the buffer acknowledges output readiness after applying source masks");
  }

  if (failures) {
    fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  fprintf(stderr, "process graph candidates: OK\n");
  return 0;
}

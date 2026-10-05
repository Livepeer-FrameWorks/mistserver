#include "../src/process_supervisor.h"

#include <iostream>
#include <string>

namespace {
  int fail(const char *message) {
    std::cerr << message << std::endl;
    return 1;
  }
} // namespace

int main() {
  using namespace Mist;

  if (std::string(processExitStatus(2, "fixed", 1)) != "unrecoverable" || std::string(processExitStatus(0, "fixed", 1)) != "clean" ||
      std::string(processExitStatus(1, "disabled", 1)) != "disabled" || std::string(processExitStatus(1, "disabled", 0)) != "retrying" ||
      std::string(processExitStatus(1, "backoff", 8)) != "retrying") {
    return fail("process exit status does not match the restart contract");
  }

  if (!processSupervisorMayStart(true, STRMSTAT_INIT, false) || !processSupervisorMayStart(true, STRMSTAT_BOOT, false) ||
      !processSupervisorMayStart(true, STRMSTAT_WAIT, false) || !processSupervisorMayStart(true, STRMSTAT_READY, false) ||
      processSupervisorMayStart(true, STRMSTAT_WAIT, true) || processSupervisorMayStart(true, STRMSTAT_SHUTDOWN, false) ||
      processSupervisorMayStart(true, STRMSTAT_OFF, false) || processSupervisorMayStart(false, STRMSTAT_READY, false)) {
    return fail("the supervisor must start processors only while the stream can still produce media");
  }

  {
    JSON::Value thumbs, av, avAudio, avRaw, livepeer;
    thumbs["process"] = "Thumbs";
    av["process"] = "AV";
    avAudio["process"] = "AV";
    avAudio["codec"] = "opus";
    avRaw["process"] = "AV";
    avRaw["codec"] = "UYVY";
    livepeer["process"] = "Livepeer";
    const std::vector<std::string> thumbOutputs = processReservableOutputs(thumbs);
    if (thumbOutputs.size() != 3 || thumbOutputs[0] != "sprite" || thumbOutputs[1] != "vtt" ||
        thumbOutputs[2] != "preview" || processReservableOutputs(av) != std::vector<std::string>{"video"} ||
        processReservableOutputs(avAudio) != std::vector<std::string>{"audio"} ||
        processReservableOutputs(avRaw).size() || processReservableOutputs(livepeer).size()) {
      return fail("AV and Thumbs outputs are reserved by their output names, raw AV outputs are not");
    }
  }
  if (producerResumeDeadline(1000, 0) != 1000 + PRODUCER_RESUME_GRACE_MS ||
      producerResumeDeadline(1000, 6000) != 6000 + PRODUCER_RESUME_GRACE_MS) {
    return fail("a producer gets the restart grace after its next start, which may be a restart delay later");
  }
  if (!trackHeldForProducer(true, 31000, 30999) || trackHeldForProducer(true, 31000, 31000) ||
      trackHeldForProducer(false, 31000, 1000)) {
    return fail("a track is kept for its producer only while that producer restarts and within its deadline");
  }

  const std::string payload = processExitTriggerPayload("camera", "AV", "{\"process\":\"AV\"}", 1234, -9, 3, "retrying",
                                                        "signal", "terminated by signal");
  const std::string expected = "camera\nAV\n{\"process\":\"AV\"}\n1234\n-9\n3\nretrying\nsignal\nterminated by signal";
  if (payload != expected) { return fail("PROCESS_EXIT payload fields or ordering changed"); }

  const std::string stopped = processExitTriggerPayload("camera", "ONNX", "{}", 55, 0, 1, "stopped",
                                                        "inhibited by stream tags", "inhibited by stream tags");
  if (stopped != "camera\nONNX\n{}\n55\n0\n1\nstopped\ninhibited by stream tags\ninhibited by stream tags") {
    return fail("deliberate supervisor stops must retain their attribution");
  }

  return 0;
}

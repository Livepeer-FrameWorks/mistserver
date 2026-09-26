#include "../src/output/recording_summary.h"

#include <iostream>

namespace {
  int fail(const char *message) {
    std::cerr << message << std::endl;
    return 1;
  }

  size_t addVideo(DTSC::Meta & meta, uint32_t height, uint64_t lastms) {
    const size_t track = meta.addTrack();
    meta.setID(track, track + 1);
    meta.setType(track, "video");
    meta.setCodec(track, "H264");
    meta.setWidth(track, 854);
    meta.setHeight(track, height);
    meta.validateTrack(track, TRACK_VALID_ALL);
    meta.update(0, 0, track, 1, 0, true, 1);
    meta.update(lastms, 0, track, 1, 0, true, 1);
    return track;
  }
} // namespace

// A 480p source transcoded to a 480p rendition: the source passthrough left
// the recording early, the rendition ran to the end. The summary must let a
// consumer tell the two apart by more than their span.
int main() {
  DTSC::Meta meta("", true);
  const size_t source = addVideo(meta, 480, 10933);
  const size_t rendition = addVideo(meta, 480, 29933);
  meta.setSourceTrack(rendition, source);

  JSON::Value src, out;
  Mist::describeRecordedTrack(meta, source, src);
  Mist::describeRecordedTrack(meta, rendition, out);

  if (src.isMember("source")) { return fail("the source passthrough must not name a source track"); }
  if (!out.isMember("source") || out["source"].asStringRef() != meta.getTrackIdentifier(source)) {
    return fail("a rendition must name its source track the way stream JSON does");
  }
  if (out["height"].asInt() != 480 || out["lastms"].asInt() != 29933) {
    return fail("the rendition's dimensions and span must be reported");
  }

  // Livepeer masks the source for VOD and the buffer tears it down when the
  // input ends. The rendition must still name the source the recording saw
  // while writing it, or it is taken for the source and judged missing.
  DTSC::Meta masked("", true);
  const size_t goneSource = addVideo(masked, 720, 20000);
  const size_t sameHeight = addVideo(masked, 720, 20000);
  masked.setSourceTrack(sameHeight, goneSource);
  const std::string remembered = masked.getTrackIdentifier(goneSource);
  masked.removeTrack(goneSource);
  JSON::Value late;
  Mist::describeRecordedTrack(masked, sameHeight, late);
  if (late.isMember("source")) { return fail("an invalid source must not be resolved from live metadata"); }
  Mist::describeRecordedTrack(masked, sameHeight, late, remembered);
  if (!late.isMember("source") || late["source"].asStringRef() != remembered) {
    return fail("a rendition must keep the source name remembered while it was written");
  }
  return 0;
}

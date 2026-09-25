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
  return 0;
}

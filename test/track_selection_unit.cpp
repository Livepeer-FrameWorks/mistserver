#include <mist/dtsc.h>
#include <mist/stream.h>

#include <cstdio>
#include <set>
#include <string>

namespace {
  int fail(const char *message) {
    fprintf(stderr, "%s\n", message);
    return 1;
  }

  size_t addVideoTrack(DTSC::Meta & meta, size_t id, const std::string & codec, uint32_t width, uint32_t height, uint64_t bytesPerSecond) {
    const size_t track = meta.addTrack(id, id, 16, 2, true);
    meta.setID(track, id);
    meta.setType(track, "video");
    meta.setCodec(track, codec);
    meta.setWidth(track, width);
    meta.setHeight(track, height);
    meta.setBps(track, bytesPerSecond);
    return track;
  }
} // namespace

int main() {
  DTSC::Meta meta;
  meta.reInit("", true);
  const size_t h264 = addVideoTrack(meta, 1, "H264", 1920, 1080, 500000);
  const size_t jpeg = addVideoTrack(meta, 2, "JPEG", 320, 180, 8000);
  const size_t png = addVideoTrack(meta, 3, "PNG", 160, 90, 4000);
  const std::set<size_t> tracks = {h264, jpeg, png};

  std::set<size_t> selected = Util::pickTracks(meta, tracks, "video", "<640x360");
  if (!selected.empty()) {
    return fail("video resolution comparators must not treat image/sprite tracks as renditions");
  }
  selected = Util::pickTracks(meta, tracks, "video", ">640x360");
  if (selected != std::set<size_t>{h264}) {
    return fail("video resolution comparators must retain matching encoded-video renditions");
  }
  selected = Util::pickTracks(meta, tracks, "video", "<100kbps");
  if (!selected.empty()) { return fail("video bitrate comparators must not match low-rate image/sprite tracks"); }
  selected = Util::pickTracks(meta, tracks, "JPEG", "<640x360");
  if (selected != std::set<size_t>{jpeg}) { return fail("an explicit JPEG comparator must still select JPEG tracks"); }
  selected = Util::pickTracks(meta, tracks, "PNG", "<100kbps");
  if (selected != std::set<size_t>{png}) { return fail("an explicit PNG comparator must still select PNG tracks"); }

  // Before a track's first fragment closes its bitrate is unknown (0). maxbps
  // must still select it, or processes selecting video=maxbps stall until the
  // next keyframe (a long-GOP short VOD ends first).
  DTSC::Meta fresh;
  fresh.reInit("", true);
  const size_t unrated = addVideoTrack(fresh, 1, "H264", 1280, 720, 0);
  const std::set<size_t> freshTracks = {unrated};
  if (Util::pickTracks(fresh, freshTracks, "video", "maxbps") != std::set<size_t>{unrated}) {
    return fail("maxbps must select a video track whose bitrate is not known yet");
  }
  // A known bitrate still wins over an unrated track.
  const size_t rated = addVideoTrack(fresh, 2, "H264", 640, 360, 100000);
  const std::set<size_t> mixed = {unrated, rated};
  if (Util::pickTracks(fresh, mixed, "video", "maxbps") != std::set<size_t>{rated}) {
    return fail("maxbps must prefer a track with a known bitrate");
  }

  return 0;
}

#include "../src/input/realtime_track.h"

#include <mist/dtsc.h>

#include <iostream>

namespace {
  int failures = 0;

  void check(bool ok, const std::string & message) {
    if (ok) { return; }
    std::cerr << "FAIL: " << message << std::endl;
    ++failures;
  }

  size_t addVideo(DTSC::Meta & meta) {
    const size_t track = meta.addTrack(8, 8, 256, 2, true);
    meta.setID(track, track + 1);
    meta.setType(track, "video");
    meta.setCodec(track, "H264");
    meta.setWidth(track, 1280);
    meta.setHeight(track, 720);
    return track;
  }
} // namespace

/// A track reports a bitrate once a second of it is buffered, even while its first fragment is
/// still open (one long GOP, or a short VOD that is a single GOP): the processing graph decides
/// rendition retention on it. A closed fragment then replaces the estimate.
int main() {
  {
    DTSC::Meta meta;
    meta.reInit("", true);
    const size_t track = addVideo(meta);
    meta.update(0, 0, track, 20000, 0, true, 20000);
    for (uint64_t time = 40; time < 3000; time += 40) { meta.update(time, 0, track, 5000, 0, false, 5000); }
    const uint64_t bytes = 20000 + 5000 * 74;
    const uint64_t expect = bytes * 1000 / 2960;
    check(meta.getBps(track) > expect * 9 / 10 && meta.getBps(track) < expect * 11 / 10,
          "a single open GOP must report its bitrate, got " + std::to_string(meta.getBps(track)) + " B/s, want about " +
            std::to_string(expect));
  }
  {
    DTSC::Meta meta;
    meta.reInit("", true);
    const size_t track = addVideo(meta);
    meta.update(0, 0, track, 20000, 0, true, 20000);
    for (uint64_t time = 40; time < 960; time += 40) { meta.update(time, 0, track, 5000, 0, false, 5000); }
    check(!meta.getBps(track), "under a second of data is no bitrate yet");
  }
  {
    // A VOD input feeding a processing stream in realtime carries the bitrate its header knows,
    // so the stream reports it from the first packet on.
    DTSC::Meta vod;
    vod.reInit("", true);
    const size_t source = addVideo(vod);
    vod.setBps(source, 500000);
    vod.setMaxBps(source, 900000);
    DTSC::Meta live;
    live.reInit("", true);
    const size_t target = live.addTrack(8, 8, 256, 2, true);
    Mist::describeRealtimeTrack(live, target, vod, source);
    check(live.getBps(target) == 500000 && live.getMaxBps(target) == 900000,
          "the realtime track must carry the header bitrate, got " + std::to_string(live.getBps(target)));
    check(live.getWidth(target) == 1280 && live.getCodec(target) == "H264", "the realtime track keeps its description");
    live.update(0, 0, target, 20000, 0, true, 20000);
    for (uint64_t time = 40; time < 1500; time += 40) { live.update(time, 0, target, 1000, 0, false, 1000); }
    check(live.getBps(target) == 500000,
          "an open fragment must not replace a known bitrate, got " + std::to_string(live.getBps(target)));
  }
  {
    DTSC::Meta vod;
    vod.reInit("", true);
    const size_t source = vod.addTrack(8, 8, 256, 2, true);
    vod.setType(source, "audio");
    vod.setCodec(source, "AAC");
    vod.setRate(source, 48000);
    vod.setChannels(source, 6);
    DTSC::Meta live;
    live.reInit("", true);
    const size_t target = live.addTrack(8, 8, 256, 2, true);
    Mist::describeRealtimeTrack(live, target, vod, source);
    check(live.getChannels(target) == 6 && live.getRate(target) == 48000,
          "paused source metadata must describe its six audio channels before any packet arrives");
  }
  if (failures) {
    std::cerr << failures << " failure(s)" << std::endl;
    return 1;
  }
  return 0;
}

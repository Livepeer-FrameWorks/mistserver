
#include <mist/dtsc.h>
#include <mist/stream.h>

#include <cstdio>
#include <list>
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
    meta.update(0, 0, track, 1, 0, true, 1);
    meta.setBps(track, bytesPerSecond);
    return track;
  }

  size_t addAudioTrack(DTSC::Meta & meta, size_t id, const std::string & codec) {
    const size_t track = meta.addTrack(id, id, 16, 2, true);
    meta.setID(track, id);
    meta.setType(track, "audio");
    meta.setCodec(track, codec);
    meta.setRate(track, 48000);
    meta.setChannels(track, 2);
    meta.update(0, 0, track, 1, 0, true, 1);
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

  DTSC::Meta restream;
  restream.reInit("", true);
  const size_t source = addVideoTrack(restream, 10, "H264", 1920, 1080, 1125000);
  const size_t rung = addVideoTrack(restream, 11, "H264", 1920, 1080, 812500);
  restream.setSourceTrack(rung, source);
  const size_t opus = addAudioTrack(restream, 12, "opus");
  const size_t aac = addAudioTrack(restream, 13, "AAC");
  restream.setSourceTrack(aac, opus);
  std::set<size_t> videoTracks = {source, rung};
  std::list<size_t> ranked;
  Util::sortTracks(videoTracks, restream, Util::TRKSORT_OPTIMAL, ranked);
  if (ranked.front() != source) {
    return fail("an equal-quality later rendition must not displace the original track");
  }
  const size_t larger = addVideoTrack(restream, 14, "H264", 2560, 1440, 1000000);
  videoTracks.insert(larger);
  Util::sortTracks(videoTracks, restream, Util::TRKSORT_OPTIMAL, ranked);
  if (ranked.front() != larger) { return fail("a genuinely higher-quality video track must still rank first"); }
  restream.validateTrack(larger, 0);
  std::map<std::string, std::string> params;
  params["video"] = "restream_auto";
  params["video_codecs"] = "H264";
  params["audio_codecs"] = "AAC";
  if (Util::wouldSelect(restream, params) != std::set<size_t>{source, aac}) {
    return fail("automatic restream must prefer original video and compatible converted audio");
  }
  restream.setMaxBps(source, 2000000);
  params["max_video_bps"] = "10000000";
  if (Util::wouldSelect(restream, params) != std::set<size_t>{source, aac}) {
    return fail("a past bitrate spike must not permanently exclude compliant source video");
  }
  params["max_video_bps"] = "8000000";
  if (Util::wouldSelect(restream, params) != std::set<size_t>{rung, aac}) {
    return fail("automatic restream must use a compliant processed video when original exceeds a hard cap");
  }
  params["video"] = "restream_source";
  if (!Util::wouldSelect(restream, params).empty()) {
    return fail("source-only restream must not silently use a processed video");
  }
  params["video"] = "restream_processed";
  if (Util::wouldSelect(restream, params) != std::set<size_t>{rung, aac}) {
    return fail("processed-only restream must choose one processed video and compatible audio");
  }

  DTSC::Meta videoOnly;
  videoOnly.reInit("", true);
  const size_t video = addVideoTrack(videoOnly, 21, "H264", 1280, 720, 300000);
  params.clear();
  params["video"] = "restream_auto";
  if (Util::wouldSelect(videoOnly, params) != std::set<size_t>{video}) {
    return fail("automatic restream must accept a video-only stream");
  }
  DTSC::Meta audioOnly;
  audioOnly.reInit("", true);
  const size_t audio = addAudioTrack(audioOnly, 22, "AAC");
  if (Util::wouldSelect(audioOnly, params) != std::set<size_t>{audio}) {
    return fail("automatic restream must accept an audio-only stream");
  }
  DTSC::Meta pendingAudio;
  pendingAudio.reInit("", true);
  const size_t pendingVideo = addVideoTrack(pendingAudio, 30, "H264", 1280, 720, 300000);
  const size_t pendingOpus = addAudioTrack(pendingAudio, 31, "opus");
  (void)pendingVideo;
  (void)pendingOpus;
  params["audio_codecs"] = "AAC";
  if (!Util::wouldSelect(pendingAudio, params).empty()) {
    return fail("an unsupported source audio track must wait for a compatible conversion");
  }
  DTSC::Meta pendingVideoMeta;
  pendingVideoMeta.reInit("", true);
  const size_t pendingHEVC = addVideoTrack(pendingVideoMeta, 40, "HEVC", 1280, 720, 300000);
  const size_t pendingAAC = addAudioTrack(pendingVideoMeta, 41, "AAC");
  (void)pendingHEVC;
  (void)pendingAAC;
  params.erase("audio_codecs");
  params["video_codecs"] = "H264";
  if (!Util::wouldSelect(pendingVideoMeta, params).empty()) {
    return fail("an incompatible source video must wait for a compatible rendition");
  }

  DTSC::Meta sizes;
  sizes.reInit("", true);
  const size_t landscape = addVideoTrack(sizes, 50, "H264", 640, 360, 100000);
  const size_t portrait = addVideoTrack(sizes, 51, "H264", 360, 640, 100000);
  const size_t ultrawide = addVideoTrack(sizes, 52, "H264", 3840, 1080, 100000);
  const size_t square = addVideoTrack(sizes, 53, "H264", 1080, 1080, 100000);
  const size_t small = addVideoTrack(sizes, 54, "H264", 320, 180, 100000);
  const size_t image = addVideoTrack(sizes, 55, "JPEG", 160, 90, 1000);
  const std::set<size_t> sizeTracks = {landscape, portrait, ultrawide, square, small, image};
  if (Util::pickTracks(sizes, sizeTracks, "video", "<0x480") != std::set<size_t>{landscape, portrait, small}) {
    return fail("short-side selector must inhibit larger rungs for landscape and portrait sources");
  }
  if (Util::pickTracks(sizes, sizeTracks, "video", "<0x360") != std::set<size_t>{small}) {
    return fail("short-side comparison must be strict at the minimum rung boundary");
  }
  if (Util::pickTracks(sizes, sizeTracks, "video", ">0x360") != std::set<size_t>{ultrawide, square}) {
    return fail("greater-than short-side selection must handle wide and square sources");
  }
  if (Util::pickTracks(sizes, sizeTracks, "video", ">0x1080").size()) {
    return fail("ultrawide area must not satisfy a taller short-side threshold");
  }
  if (Util::pickTracks(sizes, sizeTracks, "video", ">1920x1080") != std::set<size_t>{ultrawide}) {
    return fail("explicit width and height comparators must retain their area semantics");
  }
  if (Util::pickTracks(sizes, sizeTracks, "JPEG", "<0x360") != std::set<size_t>{image}) {
    return fail("explicit image short-side selectors must still work");
  }
  // The 720p rung's track_retain as pkg/mist RetainSelector composes it, at every band edge. Mist
  // compares strictly, short sides in pixels and bitrates as kbps*1024/8 bytes per second.
  const std::string retain720 =
    "audio=none&subtitle=none&meta=none&video=h264,hevc,|source,|>0x683,|<0x757,|>2197kbps,|<8790kbps";
  struct RetainProbe {
      const char *what;
      const char *codec;
      uint32_t width, height;
      uint64_t bps;
      bool original, retains;
  };
  const uint64_t inBand = 4500000 / 8;
  const RetainProbe retainProbes[] = {
    {"at the lower bitrate bound", "H264", 1280, 720, 2197 * 128, true, false},
    {"just above the lower bitrate bound", "H264", 1280, 720, 2197 * 128 + 1, true, true},
    {"just below the upper bitrate bound", "H264", 1280, 720, 8790 * 128 - 1, true, true},
    {"at the upper bitrate bound", "H264", 1280, 720, 8790 * 128, true, false},
    {"at the lower short-side bound", "H264", 1214, 683, inBand, true, false},
    {"just above the lower short-side bound", "H264", 1216, 684, inBand, true, true},
    {"just below the upper short-side bound", "H264", 1344, 756, inBand, true, true},
    {"at the upper short-side bound", "H264", 1346, 757, inBand, true, false},
    {"in portrait", "H264", 720, 1280, inBand, true, true},
    {"as HEVC", "HEVC", 1280, 720, inBand, true, true},
    {"as AV1", "AV1", 1280, 720, inBand, true, false},
    {"as a process output", "H264", 1280, 720, inBand, false, false},
    {"as a JPEG image", "JPEG", 1280, 720, inBand, true, false},
  };
  for (const RetainProbe & probe : retainProbes) {
    DTSC::Meta one;
    one.reInit("", true);
    const size_t track = addVideoTrack(one, 60, probe.codec, probe.width, probe.height, probe.bps);
    if (!probe.original) { one.setSourceTrack(track, track); }
    if (!Util::wouldSelect(one, retain720).empty() != probe.retains) {
      fprintf(stderr, "720p track_retain %s: selected %d, want %d\n", probe.what, (int)!probe.retains, (int)probe.retains);
      return 1;
    }
  }

  // The transcode source: the highest-bitrate original continuous-video track, the lowest index
  // on a tie or while no original reports a bitrate. Process outputs and images never qualify.
  DTSC::Meta ladder;
  ladder.reInit("", true);
  const size_t full = addVideoTrack(ladder, 70, "H264", 1920, 1080, 5000000 / 8);
  const size_t whack = addVideoTrack(ladder, 71, "H264", 1280, 720, 15000000 / 8);
  const size_t derived = addVideoTrack(ladder, 72, "H264", 3840, 2160, 40000000 / 8);
  ladder.setSourceTrack(derived, full);
  const size_t cover = addVideoTrack(ladder, 73, "JPEG", 640, 360, 50000000 / 8);
  const size_t sheet = addVideoTrack(ladder, 74, "PNG", 640, 360, 60000000 / 8);
  const std::set<size_t> ladderTracks = {full, whack, derived, cover, sheet};
  if (Util::pickTracks(ladder, ladderTracks, "video", "source,!jpeg,!png,|maxbps") != std::set<size_t>{whack}) {
    return fail("the transcode source must be the highest-bitrate original continuous-video track");
  }
  DTSC::Meta unratedLadder;
  unratedLadder.reInit("", true);
  const size_t firstUnrated = addVideoTrack(unratedLadder, 80, "H264", 1280, 720, 0);
  const size_t secondUnrated = addVideoTrack(unratedLadder, 81, "H264", 1920, 1080, 0);
  if (Util::pickTracks(unratedLadder, {firstUnrated, secondUnrated}, "video", "source,!jpeg,!png,|maxbps") !=
      std::set<size_t>{firstUnrated}) {
    return fail("without known bitrates the transcode source must be the lowest-index original");
  }

  sizes.setSourceTrack(ultrawide, landscape);
  sizes.setSourceTrack(square, landscape);
  if (!Util::inhibitorMatchesSource(sizes, "video=<0x480", sizeTracks)) {
    return fail("source-only short-side inhibition must ignore larger processed tracks");
  }
  return 0;
}

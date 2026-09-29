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

  return 0;
}

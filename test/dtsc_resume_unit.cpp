#include <mist/defines.h>
#include <mist/dtsc.h>

#include <iostream>
#include <string>

namespace {

  std::string fromHex(const std::string & hex) {
    std::string out;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) { out.push_back((char)std::stoi(hex.substr(i, 2), 0, 16)); }
    return out;
  }

  // Init data from ffmpeg's libx264/aac FLV output (854x480, 48 kHz mono) as an
  // RTMP publisher sends it, and the track description FLV gives without
  // onMetaData: no video size, and the AAC tag flags' fixed 44100 Hz stereo.
  const std::string avcc =
    fromHex("0164001effe1001a6764001eacd940d83de6f0110000030001000003001e0f162d9601000468ef8fcbfdf8f800");
  const std::string asc = fromHex("118856e500");

  DTSC::TrackMetadata flvVideo() {
    DTSC::TrackMetadata trk;
    trk.type = "video";
    trk.codec = "H264";
    trk.init = avcc;
    return trk;
  }

  DTSC::TrackMetadata flvAudio() {
    DTSC::TrackMetadata trk;
    trk.type = "audio";
    trk.codec = "AAC";
    trk.init = asc;
    trk.rate = 44100;
    trk.size = 16;
    trk.channels = 2;
    return trk;
  }

  int failures = 0;

  void expect(bool ok, const std::string & what) {
    if (!ok) {
      std::cerr << "FAIL: " << what << std::endl;
      ++failures;
    }
  }

} // namespace

int main() {
  DTSC::Meta meta;
  meta.reInit("", true);

  // A publisher's first session stores the size and audio format its init describes.
  size_t video = meta.addOrResumeTrack(flvVideo());
  size_t audio = meta.addOrResumeTrack(flvAudio());
  expect(meta.getWidth(video) == 854 && meta.getHeight(video) == 480, "video size is taken from the init");
  expect(meta.getRate(audio) == 48000 && meta.getChannels(audio) == 1, "AAC rate and channels are taken from the init");

  // It disconnects; a reconnect describing the same stream the same way resumes both tracks.
  meta.breakClaim(video);
  meta.breakClaim(audio);
  expect(meta.addOrResumeTrack(flvVideo()) == video, "a reconnect with the same H264 init resumes the video track");
  expect(meta.addOrResumeTrack(flvAudio()) == audio, "a reconnect with the same AAC init resumes the audio track");

  // A reconnect with another stream does not resume them.
  meta.breakClaim(video);
  meta.breakClaim(audio);
  DTSC::TrackMetadata stereo = flvAudio();
  stereo.init = fromHex("1190"); // AAC-LC 48 kHz stereo
  expect(meta.addOrResumeTrack(stereo) != audio, "a reconnect with a different AAC config gets a new track");
  DTSC::TrackMetadata smaller = flvVideo();
  smaller.width = 640;
  smaller.height = 360;
  expect(meta.addOrResumeTrack(smaller) != video, "a reconnect announcing another size gets a new track");

  // Tracks described through the delayed path resume the same way.
  DTSC::Meta delayed;
  delayed.reInit("", true);
  size_t delayedAudio = delayed.addOrResumeDelayedTrack(flvAudio());
  // A delayed track becomes valid once its input has media for it; only valid tracks are resumable.
  delayed.validateTrack(delayedAudio);
  delayed.breakClaim(delayedAudio);
  expect(delayed.addOrResumeDelayedTrack(flvAudio()) == delayedAudio, "a delayed AAC track resumes");

  if (failures) { return 1; }
  std::cout << "resume tests passed" << std::endl;
  return 0;
}

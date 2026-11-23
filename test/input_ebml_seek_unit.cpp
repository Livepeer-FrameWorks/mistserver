#include "../src/input/ebml_seek_policy.h"
#include "../src/input/input_ebml.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

namespace {

  int failures = 0;

  void check(bool ok, const std::string & what) {
    if (!ok) {
      fprintf(stderr, "FAIL: %s\n", what.c_str());
      ++failures;
    }
  }

  class InputEBMLSeekProbe : public Mist::InputEBML {
    public:
      explicit InputEBMLSeekProbe(Util::Config *config) : Mist::InputEBML(config) {}

      // A VOD file input (the processing-output case) reads the whole header
      // rather than stopping at the first cluster like a streamed input.
      bool needsLock() { return true; }

      bool load(const char *path) {
        standAlone = true;
        return inFile.open(path) && readHeader();
      }

      bool acceptsLegacyClockHeader(bool current = false) {
        char path[] = "/tmp/mist-ebml-clock-cache-XXXXXX";
        const int fd = mkstemp(path);
        if (fd < 0) { return true; }
        close(fd);
        config->getOption("input", true).append(path);
        config->getOption("realtime", true).append(0u);
        streamName = "ebmlclockcache" + std::to_string(getpid()) + (current ? "new" : "old");
        config->getOption("streamname", true).append(streamName);
        meta.reInit("", true);
        meta.setVod(true);
        const size_t track = meta.addTrack();
        meta.setID(track, 1);
        meta.setType(track, "video");
        meta.setCodec(track, "H264");
        meta.update(80, -40, track, 5, 0, true, 40);
        meta.inputLocalVars["version"] = current ? 5 : 4;
        if (current) {
          JSON::Value & clock = meta.inputLocalVars["decode_clocks"]["1"];
          clock["depth"] = 3;
          clock["delay"] = 120;
          clock["origin"] = 1000;
        }
        meta.inputLocalVars["complete"] = 1;
        const std::string header = std::string(path) + ".dtsh";
        M.toFile(header);
        DTSC::Meta cached("", header);
        if (cached.inputLocalVars["version"].asInt() != (current ? 5 : 4) || cached.getValidTracks().empty()) {
          fprintf(stderr, "FAIL: legacy clock fixture did not create a valid DTSH file\n");
          ++failures;
        }
        const bool accepted = readExistingHeader();
        unlink(header.c_str());
        unlink(path);
        if (current) {
          const auto clock = parser.packBuf.find(1);
          return accepted && clock != parser.packBuf.end() && clock->second.initialized &&
            clock->second.maxDelay == 3 && clock->second.decodeDelay == 120 && clock->second.decodeOrigin == 1000;
        }
        return accepted;
      }

      size_t trackOfType(const std::string & type) {
        std::set<size_t> tracks = M.getValidTracks();
        for (size_t t : tracks) {
          if (M.getType(t) == type) { return t; }
        }
        return INVALID_TRACK_ID;
      }

      uint64_t firstKeyTime(size_t track) {
        DTSC::Keys keys(M.keys(track));
        return keys.getTime(keys.getFirstValid());
      }

      /// Seeks to seekTime for `track` and returns the time of the first packet
      /// the input delivers for it, or UINT64_MAX when none arrives.
      uint64_t firstPacketAfterSeek(uint64_t seekTime, size_t track) {
        seek(seekTime, track);
        getNext(track);
        if (!thisPacket) { return UINT64_MAX; }
        return thisTime;
      }
  };

  void policyCases() {
    Mist::EBMLSeekTrack video{{2000, 4000, 6000}, {5000, 9000, 13000}};
    Mist::EBMLSeekTrack audio{{0, 2000, 4000}, {1000, 5000, 9000}};
    // Audio starts before video: loading from 0 must start at audio's first cluster.
    check(Mist::ebmlSeekPosition({video, audio}, 0) == 1000, "seek 0 across audio+video starts at the earliest cluster");
    check(Mist::ebmlSeekPosition({audio}, 0) == 1000, "seek 0 for audio alone starts at audio's first cluster");
    // Before this change the video track's key 0 decided the position.
    check(Mist::ebmlSeekPosition({video}, 0) == 5000, "seek before a track's first key uses that track's first key");
    check(Mist::ebmlSeekPosition({video, audio}, 4500) == 9000, "mid-file seek uses each track's last key at or before the time");
    check(Mist::ebmlSeekPosition({video, audio}, 2500) == 5000, "mid-file seek takes the earliest of the tracks' key clusters");
    Mist::EBMLSeekTrack unknown{{0, 2000}, {0, 7000}};
    check(Mist::ebmlSeekPosition({unknown}, 0) == 0, "an unknown key position alone gives no seek position");
    check(Mist::ebmlSeekPosition({unknown}, 2500) == 7000, "unknown positions are skipped in favour of known ones");
    check(Mist::ebmlSeekPosition({}, 0) == 0, "no tracks gives no seek position");
  }

  void fileCase(const char *path) {
    Util::Config config("input-ebml-seek-unit");
    config.is_active = true;
    InputEBMLSeekProbe input(&config);
    if (!input.load(path)) {
      check(false, std::string("could not read the EBML fixture ") + path);
      return;
    }
    size_t audio = input.trackOfType("audio");
    size_t video = input.trackOfType("video");
    check(audio != INVALID_TRACK_ID && video != INVALID_TRACK_ID, "fixture has an audio and a video track");
    if (audio == INVALID_TRACK_ID || video == INVALID_TRACK_ID) { return; }
    uint64_t videoStart = input.firstKeyTime(video);
    check(videoStart >= 1500, "fixture video starts late (setup), got " + std::to_string(videoStart));
    uint64_t audioFirst = input.firstPacketAfterSeek(0, audio);
    check(audioFirst < 100,
          "audio page for key 0 starts at the file's first audio packet, got " + std::to_string(audioFirst) +
            " ms (video starts at " + std::to_string(videoStart) + ")");
  }

} // namespace

int main(int argc, char **argv) {
  policyCases();
  {
    Util::Config config("ebml-clock-cache-probe");
    config.is_active = true;
    InputEBMLSeekProbe input(&config);
    check(!input.acceptsLegacyClockHeader(), "cached signed-offset Matroska indexes must be rebuilt for the new decode clock");
  }
  {
    Util::Config config("ebml-clock-cache-restore-probe");
    config.is_active = true;
    InputEBMLSeekProbe input(&config);
    check(input.acceptsLegacyClockHeader(true), "cached Matroska indexes must restore the original decode clock");
  }
  if (argc > 1) { fileCase(argv[1]); }
  if (failures) { return 1; }
  fprintf(stderr, "EBML seek: OK\n");
  return 0;
}

#include "../src/output/output_mp4.h"

#include <mist/mp4_generic.h>

#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>
#include <unistd.h>

class MP4InitProbe : public Mist::OutMP4 {
  public:
    bool recordingProbe = false;
    bool isRecording() override { return recordingProbe; }
    MP4InitProbe(Socket::Connection & conn, Util::Config & cfg, JSON::Value & capa) : Mist::OutMP4(conn, cfg, capa) {}
    void fileHeaderWaitsForAudio(bool arrives, bool selected) {
      recordingProbe = true;
      streamName = "mp4initwait" + std::to_string(getpid()) + (arrives ? "ready" : "missing") + (selected ? "selected" : "none");
      DTSC::Meta writer;
      writer.reInit(streamName, true);
      writer.setLive(true);
      const size_t video = writer.addTrack();
      writer.setID(video, 1);
      writer.setType(video, "video");
      writer.setCodec(video, "H264");
      writer.setWidth(video, 320);
      writer.setHeight(video, 180);
      writer.update(0, 0, video, 100, 0, true, 100);
      const size_t audio = writer.addTrack();
      writer.setID(audio, 2);
      writer.setType(audio, "audio");
      writer.setCodec(audio, "EAC-3");
      writer.setRate(audio, 48000);
      writer.setChannels(audio, 6);
      writer.validateTrack(audio);
      Comms::Users bufferUsers;
      bufferUsers.reload(streamName, true);
      meta.reInit(streamName, false);
      meta.ignorePid(0);
      isInitialized = true;
      targetParams["video"] = "all";
      targetParams["audio"] = selected ? "all" : "none";
      targetParams["subtitle"] = "none";
      targetParams["meta"] = "none";
      userSelect[video];
      std::thread arrive([&]() {
        if (!arrives) { return; }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        writer.setInit(audio, std::string("\x02\x00\x20\x3f\x00", 5));
        writer.update(0, 0, audio, 100, 0, true, 32);
      });
      const auto start = std::chrono::steady_clock::now();
      sendHeader();
      const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
      arrive.join();
      assert(elapsed < 2500);
      if (!selected) { assert(elapsed < 500); }
      if (!arrives && selected) { assert(elapsed >= 1400); }
      if (fileHeaderTracks.count(audio) != (arrives && selected)) {
        fprintf(stderr, "recording selected the wrong EAC-3 tracks after its bounded setup wait\n");
        exit(1);
      }
    }
    void missingSampleOrderFails() {
      recordingProbe = true;
      assert(isRecording());
      meta.reInit("", true);
      meta.setVod(true);
      const size_t video = meta.addTrack();
      meta.setID(video, 1);
      meta.setType(video, "video");
      meta.setCodec(video, "H264");
      meta.update(0, 0, video, 4, 0, true, 40);
      thisIdx = video;
      thisPacket.genericFill(0, 0, 1, "data", 4, 0, true);
      sortSet.clear();
      fileSize = 100;
      currPos = 20;
      Util::exitReason[0] = 0;
      sendNext();
      if (std::string(Util::exitReason).find("MP4 packet order is unavailable") == std::string::npos) {
        fprintf(stderr, "missing MP4 sample order silently truncated a recording before its declared end\n");
        exit(1);
      }
    }
    void bframeDuration(bool preroll = false) {
      meta.reInit("", true);
      meta.setVod(true);
      const size_t video = meta.addTrack();
      meta.setID(video, 1);
      meta.setType(video, "video");
      meta.setCodec(video, "H264");
      meta.setWidth(video, 320);
      meta.setHeight(video, 180);
      if (preroll) {
        const uint64_t decode[] = {0, 1, 2, 40, 80, 120, 160};
        const uint64_t presentation[] = {0, 120, 40, 80, 240, 160, 200};
        for (size_t part = 0; part < 7; ++part) {
          meta.update(decode[part], presentation[part] - decode[part], video, 4, part * 4, !part, 40);
        }
      } else {
        for (int part = 0; part < 4; ++part) { meta.update(part * 40, 80, video, 4, part * 4, !part, 40); }
      }
      userSelect[video];
      Util::ResizeablePointer head;
      uint64_t size = 0;
      assert(mp4Header(head, size, 0));
      MP4::Box ftyp((char *)head, false);
      MP4::Box moov((char *)head + ftyp.boxedSize(), false);
      MP4::TRAK track = ((MP4::MOOV &)moov).getChild<MP4::TRAK>();
      MP4::MDIA media = track.getChild<MP4::MDIA>();
      MP4::STTS stts = media.getChild<MP4::MINF>().getChild<MP4::STBL>().getChild<MP4::STTS>();
      uint64_t samplesDuration = 0;
      for (size_t i = 0; i < stts.getEntryCount(); ++i) {
        const MP4::STTSEntry entry = stts.getSTTSEntry(i);
        samplesDuration += uint64_t(entry.sampleCount) * entry.sampleDelta;
      }
      const uint64_t mediaDuration = media.getChild<MP4::MDHD>().getDuration();
      const uint64_t expectedMedia = preroll ? 200 : 160;
      const uint64_t expectedPresentation = preroll ? 280 : 160;
      const uint64_t trackDuration = track.getChild<MP4::TKHD>().getDuration();
      MP4::ELST edits = track.getChild<MP4::EDTS>().getChild<MP4::ELST>();
      if (mediaDuration != expectedMedia || samplesDuration != mediaDuration || trackDuration != expectedPresentation ||
          edits.getSegmentDuration(0) != expectedPresentation) {
        fprintf(stderr, "MP4 decode/presentation duration mismatch: media=%llu samples=%llu track=%llu expectedTrack=%llu\n",
                (unsigned long long)mediaDuration, (unsigned long long)samplesDuration,
                (unsigned long long)trackDuration, (unsigned long long)expectedPresentation);
        exit(1);
      }
    }
    void check() {
      meta.reInit("", true);
      meta.setLive(true);
      const size_t video = meta.addTrack();
      meta.setID(video, 1);
      meta.setType(video, "video");
      meta.setCodec(video, "H264");
      meta.setWidth(video, 320);
      meta.setHeight(video, 180);
      meta.update(0, 0, video, 100, 0, true, 100);
      const size_t audio = meta.addTrack();
      meta.setID(audio, 2);
      meta.setType(audio, "audio");
      meta.setCodec(audio, "EAC-3");
      meta.setRate(audio, 48000);
      meta.setChannels(audio, 6);
      meta.update(0, 0, audio, 100, 0, true, 32);
      userSelect[video];
      userSelect[audio];
      Util::ResizeablePointer head;
      uint64_t size = 0;
      if (!mp4Header(head, size, 1)) {
        fprintf(stderr, "unready EAC-3 suppressed ready MP4 video\n");
        exit(1);
      }
      MP4::Box ftyp((char *)head, false);
      MP4::Box moov((char *)head + ftyp.boxedSize(), false);
      assert(ftyp.isType("ftyp") && moov.isType("moov"));
      assert(((MP4::MOOV &)moov).getChildren("trak").size() == 1);
      assert(getSupportedTracks().count(video) && !getSupportedTracks().count(audio));
      meta.setInit(audio, std::string("\x02\x00\x20\x3f\x00", 5));
      assert(getSupportedTracks().count(audio));
      sentHeader = true;
      fileHeaderTracks.insert(video);
      assert(!getSupportedTracks().count(audio));
      sentHeader = false;
      assert(getSupportedTracks().count(audio));
    }
};

int main(int argc, char **argv) {
  Util::Config cfg("mp4-init-probe");
  JSON::Value capa;
  Mist::OutMP4::init(&cfg, capa);
  Socket::Connection conn;
  MP4InitProbe probe(conn, cfg, capa);
  if (argc > 1 && std::string(argv[1]) == "missing-order") {
    probe.missingSampleOrderFails();
    return 0;
  }
  if (argc > 1 && std::string(argv[1]) == "bframe-preroll-duration") {
    probe.bframeDuration(true);
    return 0;
  }
  if (argc > 1 && std::string(argv[1]) == "bframe-duration") {
    probe.bframeDuration();
    return 0;
  }
  probe.check();
  for (unsigned int scenario = 0; scenario < 3; ++scenario) {
    const bool arrives = scenario == 0;
    const bool selected = scenario != 2;
    int fds[2];
    assert(pipe(fds) == 0);
    Socket::Connection output(fds[1], -1);
    MP4InitProbe recording(output, cfg, capa);
    Util::Config::is_active = true;
    recording.fileHeaderWaitsForAudio(arrives, selected);
    output.close();
    std::string bytes;
    char buffer[4096];
    ssize_t count;
    while ((count = read(fds[0], buffer, sizeof(buffer))) > 0) { bytes.append(buffer, count); }
    close(fds[0]);
    MP4::Box ftyp((char *)bytes.data(), false);
    MP4::Box moov((char *)bytes.data() + ftyp.boxedSize(), false);
    assert(((MP4::MOOV &)moov).getChildren("trak").size() == (arrives && selected ? 2 : 1));
  }
}

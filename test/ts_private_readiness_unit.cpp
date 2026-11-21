#include <mist/ts_stream.h>

#include <cstdio>
#include <set>
#include <string>

int main() {
  DTSC::Meta meta;
  meta.reInit("", true);
  const size_t video = meta.addTrack(1, 1, 16, 2, true);
  const size_t audio = meta.addTrack(2, 2, 16, 2, true);
  const size_t unknown = meta.addTrack(3, 3, 16, 2, true);
  for (size_t track : {video, audio, unknown}) {
    meta.setID(track, track + 1);
    meta.setType(track, track == video ? "video" : "audio");
    meta.setCodec(track, track == video ? "H264" : "AAC");
    meta.setRate(track, 44100);
    meta.setChannels(track, 2);
  }
  TS::Stream baseline, withPrivate;
  TS::Packet pat;
  pat.FromPointer(TS::PAT);
  baseline.parse(pat, 0);
  withPrivate.parse(pat, 0);
  TS::Packet packet;
  std::set<size_t> selected{video, audio};
  packet.FromPointer(TS::createPMT(selected, meta));
  baseline.parse(packet, 188);
  selected.insert(unknown);
  packet.FromPointer(TS::createPMT(selected, meta));
  TS::ProgramMappingTable pmt;
  pmt = packet;
  auto entry = pmt.getEntry(0);
  while (entry) {
    if (entry.getElementaryPid() == TS::getUniqTrackID(meta, unknown)) {
      entry.setStreamType(0x06);
      break;
    }
    entry.advance();
  }
  pmt.calcCRC();
  withPrivate.parse(pmt, 188);
  const char annexB[] = "\000\000\000\001\147\102\300\012\332\173\001\020\000\000\003\000\020"
                        "\000\000\003\003\040\361\042\152\000\000\000\001\150\316\017\310"
                        "\000\000\000\001\145\210\204";
  const std::string adts("\xff\xf1\x50\x80\x01\x7f\xfc\x01\x02\x03\x04", 11);
  size_t count = 2;
  for (size_t frame = 0; frame < 10; ++frame) {
    for (size_t track : {video, audio}) {
      std::string pes;
      if (track == video) {
        TS::Packet::getPESVideoLeadIn(pes, sizeof(annexB) - 1, 90000 + frame * 3600, 0, true);
        pes.append(annexB, sizeof(annexB) - 1);
      } else {
        TS::Packet::getPESAudioLeadIn(pes, adts.size(), 90000 + frame * 3600, 128000);
        pes += adts;
      }
      packet.clear();
      packet.setPID(TS::getUniqTrackID(meta, track));
      packet.setContinuityCounter(frame % 16);
      packet.setUnitStart(true);
      packet.fillFree(pes.data(), pes.size());
      packet.addStuffing();
      baseline.parse(packet, count * 188);
      withPrivate.parse(packet, count++ * 188);
      if (baseline.hasPacketOnEachTrack()) {
        if (!withPrivate.hasPacketOnEachTrack()) {
          fprintf(stderr, "unrecognized private PID delayed H264+AAC readiness at packet %zu\n", count);
          return 1;
        }
        printf("PASS H264+AAC first ready packet %zu is unchanged with unknown private PID\n", count);
        return 0;
      }
    }
  }
  fprintf(stderr, "fixture produced no baseline packets\n");
  return 1;
}

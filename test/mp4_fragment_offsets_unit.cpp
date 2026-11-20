#include <mist/mp4_stream.h>

#include <cstdio>

int main() {
  for (bool absolute : {false, true}) {
    MP4::TrackHeader track;
    MP4::TRAK trak;
    MP4::TKHD tkhd(1, 80, 16, 16);
    trak.setContent(tkhd, 0);
    MP4::MDIA mdia;
    MP4::MDHD mdhd(80);
    mdhd.setTimeScale(1000);
    mdia.setContent(mdhd, 0);
    MP4::HDLR hdlr("video", "fragment offset");
    mdia.setContent(hdlr, 1);
    MP4::MINF minf;
    MP4::STBL stbl;
    MP4::STSD stsd(0);
    MP4::VP09 entry;
    stsd.setEntry(entry, 0);
    stbl.setContent(stsd, 0);
    minf.setContent(stbl, 0);
    mdia.setContent(minf, 2);
    trak.setContent(mdia, 1);
    track.read(trak);
    MP4::TFHD header;
    header.setTrackID(1);
    header.setFlags(absolute ? MP4::tfhdBaseOffset : MP4::tfhdBaseIsMoof);
    if (absolute) { header.setBaseDataOffset(2000); }
    MP4::TRUN run;
    run.setFlags(MP4::trundataOffset | MP4::trunsampleDuration | MP4::trunsampleSize);
    run.setDataOffset(64);
    MP4::trunSampleInformation sample = {};
    sample.sampleDuration = 40;
    sample.sampleSize = 4;
    run.setSampleInformation(sample, 0);
    run.setSampleInformation(sample, 1);
    MP4::TRAF fragment;
    fragment.setContent(header, 0);
    fragment.setContent(run, 1);
    track.nextMoof();
    track.read(fragment);
    uint64_t offset = 0;
    track.getPart(1, &offset, 0, 0, 0, 0, 1000);
    const uint64_t expected = (absolute ? 2000 : 1000) + 68;
    if (offset != expected) {
      fprintf(stderr, "fragment sample offset %llu, expected %llu (absolute=%d)\n", (unsigned long long)offset,
              (unsigned long long)expected, absolute);
      return 1;
    }
  }
  return 0;
}

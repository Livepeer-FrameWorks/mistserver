#include <mist/mp4_stream.h>

#include <deque>
#include <iostream>

namespace {
  void readTrack(MP4::TrackHeader & result, uint32_t trackId, uint32_t timeScale, int32_t firstOffset, int32_t secondOffset) {
    MP4::TRAK trak;
    MP4::TKHD tkhd(trackId, 80, 16, 16);
    trak.setContent(tkhd, 0);

    MP4::MDIA mdia;
    MP4::MDHD mdhd(80);
    mdhd.setTimeScale(timeScale);
    mdia.setContent(mdhd, 0);
    MP4::HDLR hdlr("video", "test");
    mdia.setContent(hdlr, 1);

    MP4::MINF minf;
    MP4::STBL stbl;
    size_t child = 0;

    MP4::STSD stsd(0);
    MP4::VP09 sampleEntry;
    stsd.setEntry(sampleEntry, 0);
    stbl.setContent(stsd, child++);

    MP4::STTS stts(0);
    MP4::STTSEntry timing = {2, 40};
    stts.setSTTSEntry(timing, 0);
    stbl.setContent(stts, child++);

    MP4::CTTS ctts;
    ctts.setVersion(1);
    MP4::CTTSEntry first = {1, firstOffset};
    MP4::CTTSEntry second = {1, secondOffset};
    ctts.setCTTSEntry(first, 0);
    ctts.setCTTSEntry(second, 1);
    stbl.setContent(ctts, child++);

    MP4::STSZ stsz(0);
    stsz.setEntrySize(4, 0);
    stsz.setEntrySize(4, 1);
    stbl.setContent(stsz, child++);

    MP4::STSC stsc(0);
    stsc.setSTSCEntry(MP4::STSCEntry(1, 1, 1), 0);
    stbl.setContent(stsc, child++);

    MP4::STCO stco(0);
    stco.setChunkOffset(100, 0);
    stco.setChunkOffset(104, 1);
    stbl.setContent(stco, child++);

    minf.setContent(stbl, 0);
    mdia.setContent(minf, 2);
    trak.setContent(mdia, 1);

    result.read(trak);
  }

  bool zeroTimescaleIsRejected() {
    MP4::TrackHeader track;
    readTrack(track, 9, 0, -80, 40);
    if (track.compatible()) {
      std::cerr << "zero-timescale track was accepted" << std::endl;
      return false;
    }
    return true;
  }

  bool negativeOffsetsAreRebasedUniformly() {
    std::deque<MP4::TrackHeader> tracks;
    tracks.push_back(MP4::TrackHeader());
    readTrack(tracks.back(), 1, 1000, -80, 40);
    tracks.push_back(MP4::TrackHeader());
    readTrack(tracks.back(), 2, 1000, -120, 0);
    if (!tracks[0].compatible() || !tracks[1].compatible()) {
      std::cerr << "test tracks are unexpectedly incompatible" << std::endl;
      return false;
    }
    const int64_t firstMinimum = tracks[0].getMinCTSOffsetMs();
    const int64_t secondMinimum = tracks[1].getMinCTSOffsetMs();
    if (firstMinimum != -80 || secondMinimum != -120) {
      std::cerr << "negative CTTS minima were " << firstMinimum << " and " << secondMinimum << ", expected -80 and -120"
                << std::endl;
      return false;
    }
    if (MP4::TrackHeader::normalizeCompositionOffsets(tracks) != 120) {
      std::cerr << "wrong global presentation shift" << std::endl;
      return false;
    }
    if (tracks[0].timeShift != 40 || tracks[0].offsetShift != 80 || tracks[1].timeShift != 0 || tracks[1].offsetShift != 120) {
      std::cerr << "per-track decode/composition shifts are wrong" << std::endl;
      return false;
    }

    const int64_t originalPresentation[] = {-80, -120};
    for (size_t i = 0; i < tracks.size(); ++i) {
      uint64_t decodeTime = 0;
      int32_t compositionOffset = 0;
      tracks[i].getPart(0, 0, 0, &decodeTime, &compositionOffset, 0, 0);
      const int64_t presentationTime = decodeTime + compositionOffset;
      if (compositionOffset < 0 || presentationTime - originalPresentation[i] != 120) {
        std::cerr << "first sample did not preserve a uniform +120ms presentation shift" << std::endl;
        return false;
      }
    }
    return true;
  }

  bool fragmentedOffsetsRespectTrunVersion() {
    MP4::trunSampleInformation sample = {};
    sample.sampleDuration = 40;
    sample.sampleSize = 4;
    sample.sampleFlags = MP4::isKeySample;
    sample.sampleOffset = -80;

    const uint32_t sampleFields = MP4::trunsampleDuration | MP4::trunsampleSize | MP4::trunsampleFlags | MP4::trunsampleOffsets;

    MP4::TRUN signedOffsets;
    signedOffsets.setVersion(1);
    signedOffsets.setFlags(sampleFields);
    signedOffsets.setSampleInformation(sample, 0);
    if (signedOffsets.getSampleInformation(0).sampleOffset != -80) {
      std::cerr << "version-1 TRUN composition offset lost its sign" << std::endl;
      return false;
    }

    MP4::TRUN unsignedOffsets;
    unsignedOffsets.setVersion(0);
    unsignedOffsets.setFlags(sampleFields);
    sample.sampleOffset = 0x80000000ull;
    unsignedOffsets.setSampleInformation(sample, 0);
    if (unsignedOffsets.getSampleInformation(0).sampleOffset != 0x80000000ll) {
      std::cerr << "version-0 TRUN composition offset was incorrectly sign-extended" << std::endl;
      return false;
    }
    return true;
  }
} // namespace

int main() {
  if (!zeroTimescaleIsRejected()) { return 1; }
  if (!negativeOffsetsAreRebasedUniformly()) { return 1; }
  if (!fragmentedOffsetsRespectTrunVersion()) { return 1; }
  return 0;
}

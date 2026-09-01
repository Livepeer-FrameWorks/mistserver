#include <mist/bitfields.h>
#include <mist/cmaf.h>

#include <algorithm>
#include <iostream>
#include <vector>

int main() {
  DTSC::Meta meta;
  meta.reInit("", true);
  meta.setMinimumFragmentDuration(100);

  const size_t video = meta.addTrack(8, 8, 32, 2, true);
  meta.setType(video, "video");
  meta.setCodec(video, "H264");
  meta.update(1000, 0, video, 100, 0, true, 100);
  meta.update(1040, 80, video, 100, 0, false, 100);
  meta.update(1080, 0, video, 100, 0, false, 100);
  meta.update(1120, 0, video, 100, 0, true, 100);
  meta.update(1240, 0, video, 100, 0, true, 100);

  const size_t audio = meta.addTrack(8, 8, 32, 2, true);
  meta.setType(audio, "audio");
  meta.setCodec(audio, "AAC");
  for (uint64_t time = 1000; time <= 1240; time += 20) {
    meta.update(time, 0, audio, 20, 0, time == 1000 || time == 1120 || time == 1240, 20);
  }

  std::map<size_t, Comms::Users> selected;
  Comms::Users videoSelection;
  Comms::Users audioSelection;
  selected.insert(std::make_pair(video, videoSelection));
  selected.insert(std::make_pair(audio, audioSelection));

  Util::ResizeablePointer init;
  if (!CMAF::header(init, meta, selected) || init.size() < 16) {
    std::cerr << "muxed init builder rejected two selected tracks" << std::endl;
    return 1;
  }
  char *initData = init;
  MP4::Box ftyp(initData, false);
  if (!ftyp.isType("ftyp")) {
    std::cerr << "muxed init does not begin with ftyp" << std::endl;
    return 1;
  }
  MP4::Box moovBox(initData + ftyp.boxedSize(), false);
  if (!moovBox.isType("moov")) {
    std::cerr << "muxed init does not contain moov after ftyp" << std::endl;
    return 1;
  }
  MP4::MOOV &moov = (MP4::MOOV &)moovBox;
  if (moov.getChildren("trak").size() != 2) {
    std::cerr << "muxed init does not contain one trak per selected track" << std::endl;
    return 1;
  }

  CMAF::MuxedFragment fragment;
  if (!CMAF::muxedFragment(fragment, meta, selected, 1000, 1120, 7)) {
    std::cerr << "muxed fragment builder rejected aligned two-track range; video parts "
              << meta.getPartIndex(1000, video) << "-" << meta.getPartIndex(1120, video)
              << " valid " << DTSC::Parts(meta.parts(video)).getFirstValid() << "-"
              << DTSC::Parts(meta.parts(video)).getEndValid() << ", audio parts "
              << meta.getPartIndex(1000, audio) << "-" << meta.getPartIndex(1120, audio)
              << " valid " << DTSC::Parts(meta.parts(audio)).getFirstValid() << "-"
              << DTSC::Parts(meta.parts(audio)).getEndValid() << std::endl;
    return 1;
  }
  if (fragment.header.size() < 16 || fragment.payloadSize != 420 || fragment.samples.size() != 9) {
    std::cerr << "muxed fragment header is truncated" << std::endl;
    return 1;
  }

  char *data = &fragment.header[0];
  MP4::Box moofBox(data, false);
  if (!moofBox.isType("moof")) {
    std::cerr << "first box is not moof" << std::endl;
    return 1;
  }
  const uint64_t moofSize = moofBox.boxedSize();
  if (moofSize + 8 != fragment.header.size()) {
    std::cerr << "fragment builder emitted unexpected bytes around moof/mdat header" << std::endl;
    return 1;
  }
  MP4::Box mdatBox(data + moofSize, false);
  if (!mdatBox.isType("mdat") || mdatBox.boxedSize() != 428) {
    std::cerr << "mdat size is " << mdatBox.boxedSize() << ", expected 428" << std::endl;
    return 1;
  }

  MP4::MOOF &moof = (MP4::MOOF &)moofBox;
  std::deque<MP4::Box> trafs = moof.getChildren("traf");
  if (trafs.size() != 2) {
    std::cerr << "muxed moof contains " << trafs.size() << " traf boxes, expected 2" << std::endl;
    return 1;
  }

  std::vector<uint32_t> offsets;
  size_t trunCount = 0;
  size_t sampleCount = 0;
  for (std::deque<MP4::Box>::iterator trafBox = trafs.begin(); trafBox != trafs.end(); ++trafBox) {
    MP4::TRAF &traf = (MP4::TRAF &)*trafBox;
    std::deque<MP4::Box> truns = traf.getChildren("trun");
    trunCount += truns.size();
    for (std::deque<MP4::Box>::iterator trunBox = truns.begin(); trunBox != truns.end(); ++trunBox) {
      MP4::TRUN &trun = (MP4::TRUN &)*trunBox;
      sampleCount += trun.getSampleInformationCount();
      offsets.push_back(trun.getDataOffset());
    }
  }
  if (trunCount != 6 || sampleCount != 9) {
    std::cerr << "muxed moof contains " << trunCount << " runs and " << sampleCount
              << " samples, expected 6 runs and 9 samples" << std::endl;
    return 1;
  }

  std::sort(offsets.begin(), offsets.end());
  const uint32_t mediaStart = moofSize + 8;
  const uint32_t relativeOffsets[] = {0, 100, 140, 240, 280, 380};
  for (size_t i = 0; i < offsets.size(); ++i) {
    if (offsets[i] != mediaStart + relativeOffsets[i]) {
      std::cerr << "trun data offset " << offsets[i] << " at index " << i << ", expected "
                << mediaStart + relativeOffsets[i] << std::endl;
      return 1;
    }
  }

  // The payload contract is explicit and matches Output's timestamp/track packet ordering.
  const uint64_t expectedTimes[] = {1000, 1000, 1020, 1040, 1040, 1060, 1080, 1080, 1100};
  const size_t expectedTracks[] = {video, audio, audio, video, audio, audio, video, audio, audio};
  uint64_t describedPayload = 0;
  for (size_t i = 0; i < fragment.samples.size(); ++i) {
    if (fragment.samples[i].time != expectedTimes[i] || fragment.samples[i].track != expectedTracks[i]) {
      std::cerr << "sample emission order mismatch at index " << i << std::endl;
      return 1;
    }
    describedPayload += fragment.samples[i].size;
  }
  if (describedPayload != fragment.payloadSize || mdatBox.boxedSize() != describedPayload + 8) {
    std::cerr << "sample plan, payload size and mdat declaration disagree" << std::endl;
    return 1;
  }
  std::string completeSegment = fragment.header;
  for (size_t i = 0; i < fragment.samples.size(); ++i) {
    completeSegment.append(fragment.samples[i].size,
                           (char)((fragment.samples[i].track + fragment.samples[i].part) & 0xFF));
  }
  if (completeSegment.size() != moofSize + mdatBox.boxedSize()) {
    std::cerr << "complete muxed segment length does not match its finalized boxes" << std::endl;
    return 1;
  }

  // Preserve the DRM branch's fragment-index API while routing it through the same explicit-range
  // implementation.
  Util::ResizeablePointer compatibilityOutput;
  const bool compatibilityBuilt = CMAF::fragmentHeader(compatibilityOutput, meta, selected, 0);
  if (!compatibilityBuilt || compatibilityOutput.size() != fragment.header.size()) {
    std::cerr << "fragment-index compatibility builder diverges from explicit-range builder: built="
              << compatibilityBuilt << ", compatibility size=" << compatibilityOutput.size()
              << ", explicit size=" << fragment.header.size() << std::endl;
    return 1;
  }

  const size_t lateAudio = meta.addTrack(8, 8, 16, 2, true);
  meta.setType(lateAudio, "audio");
  meta.setCodec(lateAudio, "AAC");
  meta.update(1200, 0, lateAudio, 20, 0, true, 20);
  meta.update(1220, 0, lateAudio, 20, 0, false, 20);
  std::map<size_t, Comms::Users> sparseSelection;
  sparseSelection[video];
  sparseSelection[lateAudio];
  CMAF::MuxedFragment sparse;
  if (!CMAF::muxedFragment(sparse, meta, sparseSelection, 1000, 1120, 8)) {
    std::cerr << "an empty optional track rejected an otherwise servable muxed fragment" << std::endl;
    return 1;
  }
  for (size_t i = 0; i < sparse.samples.size(); ++i) {
    if (sparse.samples[i].track != video) {
      std::cerr << "empty optional track unexpectedly contributed a sample" << std::endl;
      return 1;
    }
  }

  return 0;
}

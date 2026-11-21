#include <mist/ac3.h>
#include <mist/bitfields.h>
#include <mist/cmaf.h>

#include <algorithm>
#include <iostream>
#include <vector>

int main(int argc, char **argv) {
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

  // A newly announced EAC-3 track cannot suppress ready video before its first audio PES.
  meta.setCodec(audio, "EAC-3");
  meta.setInit(audio, "");
  Util::ResizeablePointer videoOnlyInit;
  if (!CMAF::header(videoOnlyInit, meta, selected)) {
    std::cerr << "missing EAC-3 setup suppressed the video initialization" << std::endl;
    return 1;
  }
  MP4::Box videoFtyp((char *)videoOnlyInit, false);
  MP4::Box videoMoov((char *)videoOnlyInit + videoFtyp.boxedSize(), false);
  if (((MP4::MOOV &)videoMoov).getChildren("trak").size() != 1) {
    std::cerr << "missing EAC-3 setup emitted an unusable audio track" << std::endl;
    return 1;
  }
  if (!CMAF::keyHeader(meta, audio, 1000, 1120, 1).empty() || CMAF::payloadSize(meta, audio, 1000, 1120)) {
    std::cerr << "unready EAC-3 emitted a single-track fragment without a decodable init" << std::endl;
    return 1;
  }
  if (!CMAF::trackHeader(meta, audio).empty()) { return 1; }
  CMAF::MuxedFragment unreadyFragment;
  if (!CMAF::muxedFragment(unreadyFragment, meta, selected, 1000, 1120, 1)) { return 1; }
  for (const auto & sample : unreadyFragment.samples) {
    if (sample.track == audio) {
      std::cerr << "muxed CMAF emitted samples excluded from its init" << std::endl;
      return 1;
    }
  }
  if (argc > 1 && std::string(argv[1]) == "unready-single") {
    if (!CMAF::trackHeader(meta, audio, false).empty() || !CMAF::keyHeader(meta, audio, 1000, 1120, 1).empty() ||
        CMAF::keyHeaderSize(meta, audio, uint64_t(1000), uint64_t(1120)) || CMAF::payloadSize(meta, audio, 1000, 1120)) {
      std::cerr << "unready EAC-3 emitted a non-muxed CMAF fragment without its decoder configuration\n";
      return 1;
    }
    CMAF::MuxedFragment unready;
    if (!CMAF::muxedFragment(unready, meta, selected, 1000, 1120, 1) ||
        std::any_of(unready.samples.begin(), unready.samples.end(),
                    [audio](const CMAF::MuxedSample & sample) { return sample.track == audio; })) {
      std::cerr << "muxed media did not match its video-only initialization while EAC-3 was unready\n";
      return 1;
    }
    meta.setRate(audio, 48000);
    meta.setChannels(audio, 6);
    const std::string decoderConfig("\x02\x00\x20\x3f\x00", 5);
    meta.setInit(audio, decoderConfig);
    const std::string ready = CMAF::trackHeader(meta, audio, false);
    if (ready.empty() || CMAF::keyHeader(meta, audio, 1000, 1120, 1).empty()) {
      std::cerr << "configured EAC-3 did not become available for CMAF\n";
      return 1;
    }
    MP4::Box ftyp((char *)ready.data(), false),
      moov((char *)ready.data() + MP4::Box((char *)ready.data(), false).boxedSize(), false);
    MP4::STSD description =
      ((MP4::MOOV &)moov).getChild<MP4::TRAK>().getChild<MP4::MDIA>().getChild<MP4::MINF>().getChild<MP4::STBL>().getChild<MP4::STSD>();
    MP4::AudioSampleEntry entry = (MP4::AudioSampleEntry &)description.getEntry(0);
    MP4::Box codec = entry.getCodecBox();
    const AC3::Header decoded = AC3::parseDEC3(codec.payload(), codec.payloadSize());
    if (!ftyp.isType("ftyp") || !moov.isType("moov") || !entry.isType("ec-3") || entry.getChannelCount() != 2 ||
        entry.getSampleSize() != 16 || entry.getSampleRate() != 48000 || !codec.isType("dec3") || !decoded.valid ||
        decoded.channels != 6 || decoded.rate != 48000 || codec.boxedSize() != 8 + decoderConfig.size() ||
        std::string(codec.asBox() + 8, decoderConfig.size()) != decoderConfig) {
      std::cerr << "CMAF EAC-3 initialization lost sample entry, rate, channels or decoder bytes: prefixChannels="
                << entry.getChannelCount() << " sampleSize=" << entry.getSampleSize() << " decodedChannels=" << decoded.channels
                << " rate=" << entry.getSampleRate() << " codec=" << codec.getType() << "\n";
      return 1;
    }
    std::cout << "PASS unready CMAF EAC-3 emits no media, then exposes its complete configured header\n";
    return 0;
  }
  meta.setInit(audio, std::string("\x02\x00\x20\x3f\x00", 5));
  if (CMAF::keyHeader(meta, audio, 1000, 1120, 1).empty() || CMAF::trackHeader(meta, audio).empty()) { return 1; }
  meta.setCodec(audio, "AAC");

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

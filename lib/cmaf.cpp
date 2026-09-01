#include "cmaf.h"

#include "bitfields.h"
#include "mp4_dash.h"
#include "mp4_generic.h"
#include "stream.h"
#include "timing.h"

#include <algorithm>
#include <climits>
#include <sstream>
#include <vector>

static uint64_t unixBootDiff = Util::unixMS();

namespace CMAF{
  /// Function to determine the payload size of a CMAF fragment.
  size_t payloadSize(const DTSC::Meta &M, size_t track, uint64_t startTime, uint64_t endTime){
    DTSC::Parts parts(M.parts(track));
    size_t firstValidPart = parts.getFirstValid();
    size_t endValidPart = parts.getEndValid();
    if (firstValidPart >= endValidPart) { return 0; }
    if (startTime < M.getPartTime(firstValidPart, track)) { return 0; }

    size_t firstPart = M.getPartIndex(startTime, track);
    size_t endPart = M.getPartIndex(endTime, track);
    if (firstPart < firstValidPart || firstPart >= endPart || firstPart >= endValidPart || endPart > endValidPart) {
      return 0;
    }
    size_t payloadSize = 0;
    for (size_t i = firstPart; i < endPart; i++){payloadSize += parts.getSize(i);}
    return payloadSize;
  }

  std::string trackHeader(const DTSC::Meta &M, size_t track, bool simplifyTrackIds){
    std::string tType = M.getType(track);

    std::stringstream header;

    MP4::FTYP ftypBox;
    ftypBox.setMajorBrand("isom");
    ftypBox.setCompatibleBrands("cmfc", 0);
    ftypBox.setCompatibleBrands("isom", 1);
    ftypBox.setCompatibleBrands("dash", 2);
    ftypBox.setCompatibleBrands("iso9", 3);
    header.write(ftypBox.asBox(), ftypBox.boxedSize());

    MP4::MOOV moovBox;

    MP4::MVHD mvhdBox(0);
    mvhdBox.setTrackID(0xFFFFFFFF); // This value needs to point to an unused trackid
    moovBox.setContent(mvhdBox, 0);

    MP4::TRAK trakBox;

    MP4::TKHD tkhdBox(M, track);
    tkhdBox.setDuration(0);
    trakBox.setContent(tkhdBox, 0);

    MP4::MDIA mdiaBox;

    MP4::MDHD mdhdBox(0, M.getLang(track));
    mdiaBox.setContent(mdhdBox, 0);

    MP4::HDLR hdlrBox(tType, M.getType(track));
    mdiaBox.setContent(hdlrBox, 1);

    MP4::MINF minfBox;

    if (tType == "video"){
      MP4::VMHD vmhdBox;
      vmhdBox.setFlags(1);
      minfBox.setContent(vmhdBox, 0);
    }else if (tType == "audio"){
      MP4::SMHD smhdBox;
      minfBox.setContent(smhdBox, 0);
    }else{
      MP4::NMHD nmhdBox;
      minfBox.setContent(nmhdBox, 0);
    }

    MP4::DINF dinfBox;
    MP4::DREF drefBox;
    dinfBox.setContent(drefBox, 0);
    minfBox.setContent(dinfBox, 1);

    MP4::STBL stblBox;

    // Add STSD box
    MP4::STSD stsdBox(0);
    if (tType == "video"){
      MP4::VisualSampleEntry sampleEntry(M, track);
      MP4::BTRT btrtBox;
      btrtBox.setDecodingBufferSize(0xFFFFFFFFull);
      btrtBox.setAverageBitrate(M.getBps(track));
      btrtBox.setMaxBitrate(M.getMaxBps(track));

      sampleEntry.setBoxEntry(sampleEntry.getBoxEntryCount(), btrtBox);
      stsdBox.setEntry(sampleEntry, 0);
    }else if (tType == "audio"){
      MP4::AudioSampleEntry sampleEntry(M, track);
      MP4::BTRT btrtBox;
      btrtBox.setDecodingBufferSize(0xFFFFFFFFull);
      btrtBox.setAverageBitrate(M.getBps(track));
      btrtBox.setMaxBitrate(M.getMaxBps(track));

      sampleEntry.setBoxEntry(sampleEntry.getBoxEntryCount(), btrtBox);
      stsdBox.setEntry(sampleEntry, 0);
    }else if (tType == "meta"){
      MP4::TextSampleEntry sampleEntry(M, track);

      MP4::FontTableBox ftab;
      sampleEntry.setFontTableBox(ftab);
      stsdBox.setEntry(sampleEntry, 0);
    }

    stblBox.setContent(stsdBox, 0);

    MP4::STTS sttsBox(0);
    stblBox.setContent(sttsBox, 1);
    MP4::STSC stscBox(0);
    stblBox.setContent(stscBox, 2);
    MP4::STSZ stszBox(0);
    stblBox.setContent(stszBox, 3);
    MP4::STCO stcoBox(0);
    stblBox.setContent(stcoBox, 4);

    minfBox.setContent(stblBox, 2);
    mdiaBox.setContent(minfBox, 2);
    trakBox.setContent(mdiaBox, 1);
    moovBox.setContent(trakBox, 1);

    MP4::MVEX mvexBox;

    if (M.getVod()){
      MP4::MEHD mehdBox;
      mehdBox.setFragmentDuration(M.getDuration(track));
      mvexBox.setContent(mehdBox, 0);
    }

    MP4::TREX trexBox(track + 1);
    trexBox.setDefaultSampleDuration(1000);
    mvexBox.setContent(trexBox, M.getVod() ? 1 : 0);

    moovBox.setContent(mvexBox, 2);
    header.write(moovBox.asBox(), moovBox.boxedSize());

    if (M.getVod()){
      DTSC::Fragments fragments(M.fragments(track));
      DTSC::Keys keys(M.keys(track));
      DTSC::Parts parts(M.parts(track));

      MP4::SIDX sidxBox;
      sidxBox.setReferenceID(track + 1);
      sidxBox.setTimescale(1000);
      sidxBox.setEarliestPresentationTime(keys.getTime(0) + parts.getOffset(0) -
                                          M.getFirstms(track));

      for (size_t i = 0; i < fragments.getEndValid(); i++){
        size_t firstKey = fragments.getFirstKey(i);
        size_t endKey =
            ((i + 1 < fragments.getEndValid()) ? fragments.getFirstKey(i + 1) : keys.getEndValid());
        uint64_t endTime = (endKey == keys.getEndValid() ? M.getLastms(track) : keys.getTime(endKey));

        MP4::sidxReference refItem;
        refItem.referencedSize = payloadSize(M, track, keys.getTime(firstKey), endTime) + keyHeaderSize(M, track, i) + 8;
        refItem.subSegmentDuration = endTime - keys.getTime(firstKey);
        refItem.sapStart = true;
        refItem.sapType = 16;
        refItem.sapDeltaTime = 0;
        refItem.referenceType = 0;

        sidxBox.setReference(refItem, i);
      }
      header.write(sidxBox.asBox(), sidxBox.boxedSize());
    }

    return header.str();
  }

  bool header(Util::ResizeablePointer & headOut, const DTSC::Meta & M, const std::map<size_t, Comms::Users> & userSelect) {
    // MP4 Files always start with an FTYP box. Constructor sets default values
    MP4::FTYP ftypBox;
    ftypBox.setMajorBrand("isom");
    ftypBox.setCompatibleBrands("cmfc", 0);
    ftypBox.setCompatibleBrands("isom", 1);
    ftypBox.setCompatibleBrands("dash", 2);
    ftypBox.setCompatibleBrands("iso9", 3);
    headOut.append(ftypBox.asBox(), ftypBox.boxedSize());

    // Start building the moov box. This is the metadata box for an mp4 file, and will contain all
    // metadata.
    MP4::MOOV moovBox;
    // Keep track of the current index within the moovBox
    unsigned int moovOffset = 0;

    // Construct with duration of -1, as this is the default for fragmented
    MP4::MVHD mvhdBox(0);
    // Set the trackid for the first "empty" track within the file.
    mvhdBox.setTrackID(userSelect.size() + 1);
    moovBox.setContent(mvhdBox, moovOffset++);

    for (std::map<size_t, Comms::Users>::const_iterator it = userSelect.begin(); it != userSelect.end(); it++) {
      DTSC::Parts parts(M.parts(it->first));
      DTSC::Keys keys = M.getKeys(it->first);
      std::string tType = M.getType(it->first);

      MP4::TRAK trakBox;
      // Keep track of the current index within the moovBox
      size_t trakOffset = 0;

      MP4::TKHD tkhdBox(M, it->first);
      tkhdBox.setDuration(0);
      trakBox.setContent(tkhdBox, trakOffset++);

      MP4::MDIA mdiaBox;
      size_t mdiaOffset = 0;

      // Add the mandatory MDHD and HDLR boxes to the MDIA
      MP4::MDHD mdhdBox(0);
      mdhdBox.setLanguage(M.getLang(it->first));
      mdiaBox.setContent(mdhdBox, mdiaOffset++);
      MP4::HDLR hdlrBox(tType, M.getTrackIdentifier(it->first));
      mdiaBox.setContent(hdlrBox, mdiaOffset++);

      MP4::MINF minfBox;
      size_t minfOffset = 0;

      // Add a track-type specific box to the MINF box
      if (tType == "video") {
        MP4::VMHD vmhdBox(0, 1);
        minfBox.setContent(vmhdBox, minfOffset++);
      } else if (tType == "audio") {
        MP4::SMHD smhdBox;
        minfBox.setContent(smhdBox, minfOffset++);
      } else {
        // create nmhd box
        MP4::NMHD nmhdBox;
        minfBox.setContent(nmhdBox, minfOffset++);
      }

      // Add the mandatory DREF (dataReference) box
      MP4::DINF dinfBox;
      MP4::DREF drefBox;
      dinfBox.setContent(drefBox, 0);
      minfBox.setContent(dinfBox, minfOffset++);

      // Add STSD box
      MP4::STSD stsdBox(0);
      if (tType == "video") {
        MP4::VisualSampleEntry sampleEntry(M, it->first);
        stsdBox.setEntry(sampleEntry, 0);
      } else if (tType == "audio") {
        MP4::AudioSampleEntry sampleEntry(M, it->first);
        stsdBox.setEntry(sampleEntry, 0);
      } else if (tType == "meta") {
        MP4::TextSampleEntry sampleEntry(M, it->first);

        MP4::FontTableBox ftab;
        sampleEntry.setFontTableBox(ftab);
        stsdBox.setEntry(sampleEntry, 0);
      }

      MP4::STBL stblBox;
      size_t stblOffset = 0;
      stblBox.setContent(stsdBox, stblOffset++);

      // Add STTS Box
      // note: STTS is empty when fragmented
      MP4::STTS sttsBox(0);
      // Add STSZ Box
      // note: STSZ is empty when fragmented
      MP4::STSZ stszBox(0);
      stblBox.setContent(sttsBox, stblOffset++);
      stblBox.setContent(stszBox, stblOffset++);

      // Add STSC Box
      // note: STSC is empty when fragmented
      MP4::STSC stscBox(0);
      stblBox.setContent(stscBox, stblOffset++);

      // Create STCO Box (either stco or co64)
      // note: 64bit boxes will never be used in fragmented
      // note: Inserting empty values on purpose here, will be fixed later.
      MP4::STCO stcoBox(0);
      stcoBox.setEntryCount(0);
      stblBox.setContent(stcoBox, stblOffset++);

      minfBox.setContent(stblBox, minfOffset++);

      mdiaBox.setContent(minfBox, mdiaOffset++);

      trakBox.setContent(mdiaBox, trakOffset++);

      moovBox.setContent(trakBox, moovOffset++);
    }

    MP4::MVEX mvexBox;
    size_t curBox = 0;
    MP4::MEHD mehdBox;
    mehdBox.setFragmentDuration(-1);

    mvexBox.setContent(mehdBox, curBox++);
    for (std::map<size_t, Comms::Users>::const_iterator it = userSelect.begin(); it != userSelect.end(); it++) {
      MP4::TREX trexBox(it->first + 1);
      trexBox.setDefaultSampleDuration(1000);
      mvexBox.setContent(trexBox, curBox++);
    }
    moovBox.setContent(mvexBox, moovOffset++);
    headOut.append(moovBox.asBox(), moovBox.boxedSize());
    return true;
  }

  class sortPart{
  public:
    uint64_t time;
    size_t partIndex;
    size_t bytePos;
    bool operator<(const sortPart & rhs) const {
      if (time < rhs.time) { return true; }
      if (time > rhs.time) { return false; }
      return partIndex < rhs.partIndex;
    }
  };

  size_t keyHeaderSize(const DTSC::Meta &M, size_t track, size_t fragment){
    uint64_t tmpRes = 8 + 16 + 32 + 20;

    DTSC::Fragments fragments(M.fragments(track));
    DTSC::Keys keys(M.keys(track));
    DTSC::Parts parts(M.parts(track));

    size_t firstKey = fragments.getFirstKey(fragment);
    size_t firstPart = keys.getFirstPart(firstKey);
    size_t endPart = parts.getEndValid();
    if (fragment + 1 < fragments.getEndValid()){
      endPart = keys.getFirstPart(fragments.getFirstKey(fragment + 1));
    }

    tmpRes += 24 + ((endPart - firstPart) * 12);
    return tmpRes;
  }

  /// Calculates the full size of a 'moof' box for a DTSC::Key based fragment.
  /// Used when building the 'moof' box to calculate the relative data offsets.
  size_t keyHeaderSize(const DTSC::Meta &M, size_t track, uint64_t startTime, uint64_t endTime){
    uint64_t tmpRes = 8 + 16 + 32 + 20;
    size_t firstPart = M.getPartIndex(startTime, track);
    size_t endPart = M.getPartIndex(endTime, track);
    tmpRes += 24 + ((endPart - firstPart) * 12);
    return tmpRes;
  }

  /// Generates the 'moof' box for a DTSC::Key based CMAF fragment.
  std::string keyHeader(const DTSC::Meta &M, size_t track, uint64_t startTime, uint64_t endTime,
                        uint64_t segmentNum, bool simplifyTrackIds, bool UTCTime){

    size_t firstPart = M.getPartIndex(startTime, track);
    size_t endPart = M.getPartIndex(endTime, track);
    std::stringstream header;
    MP4::MOOF moofBox;
    MP4::MFHD mfhdBox(segmentNum);
    moofBox.setContent(mfhdBox, 0);

    std::set<sortPart> trunOrder;
    DTSC::Parts parts(M.parts(track));
    DTSC::Keys keys(M.keys(track));
    uint64_t firstSampleTime = startTime;
    if (firstPart < parts.getEndValid()) { firstSampleTime = M.getPartTime(firstPart, track); }

    // We use keyHeaderSize here to determine the relative offsets of the data in the 'mdat' box.
    uint64_t relativeOffset = keyHeaderSize(M, track, startTime, endTime) + 8;

    sortPart temp;
    temp.time = firstSampleTime;
    temp.partIndex = firstPart;
    temp.bytePos = relativeOffset;

    for (size_t p = firstPart; p < endPart; p++){
      trunOrder.insert(temp);
      temp.time += parts.getDuration(p);
      temp.partIndex++;
      temp.bytePos += parts.getSize(p);
    }

    DEBUG_MSG(5, "CMAF header track=%zu start=%" PRIu64 " mediaStart=%" PRIu64 " end=%" PRIu64 " firstPart=%zu endPart=%zu samples=%zu payload=%zu",
              track, startTime, firstSampleTime, endTime, firstPart, endPart, trunOrder.size(),
              payloadSize(M, track, startTime, endTime));

    MP4::TRAF trafBox;
    MP4::TFHD tfhdBox;

    tfhdBox.setFlags(MP4::tfhdSampleFlag | MP4::tfhdBaseIsMoof | MP4::tfhdSampleDesc);
    tfhdBox.setTrackID(track + 1);
    tfhdBox.setDefaultSampleDuration(444);
    tfhdBox.setDefaultSampleSize(444);
    tfhdBox.setDefaultSampleFlags((M.getType(track) == "video")
                                      ? (MP4::noIPicture | MP4::noKeySample)
                                      : (MP4::isIPicture | MP4::isKeySample));
    tfhdBox.setSampleDescriptionIndex(1);
    trafBox.setContent(tfhdBox, 0);

    MP4::TFDT tfdtBox;
    if (M.getVod()){
      tfdtBox.setBaseMediaDecodeTime(firstSampleTime - M.getFirstms(track));
    }else{
      tfdtBox.setBaseMediaDecodeTime(UTCTime ? firstSampleTime + M.getBootMsOffset() + unixBootDiff : firstSampleTime);
    }
    trafBox.setContent(tfdtBox, 1);

    MP4::TRUN trunBox;
    trunBox.setFlags(MP4::trundataOffset | MP4::trunfirstSampleFlags | MP4::trunsampleSize |
                     MP4::trunsampleDuration | MP4::trunsampleOffsets);

    trunBox.setDataOffset(trunOrder.size() ? trunOrder.begin()->bytePos : relativeOffset);

    bool firstSampleIsKey = M.getType(track) != "video";
    if (!firstSampleIsKey && trunOrder.size()) {
      size_t keyIdx = M.getKeyIndexForTime(track, firstSampleTime);
      firstSampleIsKey = keyIdx < keys.getEndValid() && keys.getTime(keyIdx) == firstSampleTime;
    }
    trunBox.setFirstSampleFlags(firstSampleIsKey ? (MP4::isIPicture | MP4::isKeySample) : (MP4::noIPicture | MP4::noKeySample));

    size_t trunOffset = 0;

    if (trunOrder.size()) {
      for (std::set<sortPart>::iterator it = trunOrder.begin(); it != trunOrder.end(); it++){
        MP4::trunSampleInformation sampleInfo;
        sampleInfo.sampleSize = parts.getSize(it->partIndex);
        sampleInfo.sampleDuration = parts.getDuration(it->partIndex);
        sampleInfo.sampleOffset = parts.getOffset(it->partIndex);
        trunBox.setSampleInformation(sampleInfo, trunOffset++);
      }
    } else {
      WARN_MSG("Empty CMAF header for track %zu: %" PRIu64 "-%" PRIu64
               " contains no packets (first: %" PRIu64 ", last: %" PRIu64
               "), firstPart=%zu, lastPart=%zu",
               track, startTime, endTime, M.getFirstms(track), M.getLastms(track), firstPart,
               endPart);
    }
    trafBox.setContent(trunBox, 2);

    moofBox.setContent(trafBox, 1);

    header.write(moofBox.asBox(), moofBox.boxedSize());

    return header.str();
  }

  bool muxedFragment(MuxedFragment &out, const DTSC::Meta &M,
                     const std::map<size_t, Comms::Users> &userSelect, uint64_t startTime,
                     uint64_t endTime, uint64_t sequenceNumber) {
    out.header.clear();
    out.samples.clear();
    out.payloadSize = 0;
    if (endTime <= startTime || userSelect.empty()) { return false; }

    struct SampleInfo {
      MuxedSample sample;
      uint32_t duration;
      uint32_t offset;
      uint32_t flags;
      uint64_t mediaOffset;
    };
    std::vector<SampleInfo> samples;

    for (std::map<size_t, Comms::Users>::const_iterator selected = userSelect.begin();
         selected != userSelect.end(); ++selected) {
      const size_t track = selected->first;
      if (!M.trackValid(track)) { return false; }
      DTSC::Parts parts(M.parts(track));
      DTSC::Keys keys(M.getKeys(track));
      const size_t firstPart = M.getPartIndex(startTime, track);
      const size_t endPart = M.getPartIndex(endTime, track);
      if (firstPart < parts.getFirstValid() || endPart > parts.getEndValid()) { return false; }
      if (firstPart >= endPart) { continue; } // Sparse optional track in this interval.

      uint64_t sampleTime = M.getPartTime(firstPart, track);
      if (!sampleTime && firstPart != parts.getFirstValid()) { return false; }
      size_t keyIndex = keys.getFirstValid();
      while (keyIndex + 1 < keys.getEndValid() && keys.getTime(keyIndex + 1) <= sampleTime) {
        ++keyIndex;
      }
      for (size_t part = firstPart; part < endPart; ++part) {
        if (sampleTime >= endTime) { break; }
        while (keyIndex + 1 < keys.getEndValid() && keys.getTime(keyIndex + 1) <= sampleTime) {
          ++keyIndex;
        }
        const bool isKey = M.getType(track) != "video" ||
                           (keyIndex < keys.getEndValid() && keys.getTime(keyIndex) == sampleTime);
        SampleInfo info = {};
        info.sample.track = track;
        info.sample.part = part;
        info.sample.time = sampleTime;
        info.sample.size = parts.getSize(part);
        info.duration = parts.getDuration(part);
        info.offset = parts.getOffset(part);
        info.flags = isKey ? (MP4::isIPicture | MP4::isKeySample)
                           : (MP4::noIPicture | MP4::noKeySample);
        samples.push_back(info);
        sampleTime += info.duration;
      }
    }
    if (samples.empty()) { return false; }

    std::sort(samples.begin(), samples.end(), [](const SampleInfo &a, const SampleInfo &b) {
      if (a.sample.time != b.sample.time) { return a.sample.time < b.sample.time; }
      if (a.sample.track != b.sample.track) { return a.sample.track < b.sample.track; }
      return a.sample.part < b.sample.part;
    });

    uint64_t totalData = 0;
    for (std::vector<SampleInfo>::iterator sample = samples.begin(); sample != samples.end(); ++sample) {
      sample->mediaOffset = totalData;
      totalData += sample->sample.size;
      if (totalData > 0xFFFFFFFFull - 8) { return false; }
      out.samples.push_back(sample->sample);
    }

    typedef std::pair<size_t, size_t> SampleRun; // half-open indices into samples
    std::map<size_t, std::vector<SampleRun> > trackRuns;
    for (size_t begin = 0; begin < samples.size();) {
      size_t end = begin + 1;
      while (end < samples.size() && samples[end].sample.track == samples[begin].sample.track) { ++end; }
      trackRuns[samples[begin].sample.track].push_back(SampleRun(begin, end));
      begin = end;
    }
    if (M.getVod()) {
      for (std::map<size_t, std::vector<SampleRun> >::const_iterator track = trackRuns.begin();
           track != trackRuns.end(); ++track) {
        if (samples[track->second.front().first].sample.time < M.getFirstms(track->first)) {
          return false;
        }
      }
    }

    const auto buildMoof = [&](uint64_t mediaDataStart) {
      MP4::MOOF moof;
      MP4::MFHD mfhd(sequenceNumber);
      moof.setContent(mfhd, 0);
      size_t moofEntry = 1;

      for (std::map<size_t, std::vector<SampleRun> >::const_iterator track = trackRuns.begin();
           track != trackRuns.end(); ++track) {
        const size_t trackId = track->first;
        const std::vector<SampleRun> &runs = track->second;
        if (runs.empty()) { continue; }

        MP4::TRAF traf;
        MP4::TFHD tfhd;
        tfhd.setFlags(MP4::tfhdSampleFlag | MP4::tfhdBaseIsMoof | MP4::tfhdSampleDesc);
        tfhd.setTrackID(trackId + 1);
        tfhd.setDefaultSampleFlags(M.getType(trackId) == "video"
                                       ? (MP4::noIPicture | MP4::noKeySample)
                                       : (MP4::isIPicture | MP4::isKeySample));
        tfhd.setSampleDescriptionIndex(1);
        traf.setContent(tfhd, 0);

        MP4::TFDT tfdt;
        uint64_t decodeTime = samples[runs.front().first].sample.time;
        if (M.getVod()) { decodeTime -= M.getFirstms(trackId); }
        tfdt.setBaseMediaDecodeTime(decodeTime);
        traf.setContent(tfdt, 1);

        size_t trafEntry = 2;
        for (std::vector<SampleRun>::const_iterator run = runs.begin(); run != runs.end(); ++run) {
          MP4::TRUN trun;
          trun.setFlags(MP4::trundataOffset | MP4::trunsampleSize | MP4::trunsampleDuration |
                        MP4::trunsampleFlags | MP4::trunsampleOffsets);
          const uint64_t dataOffset = mediaDataStart + samples[run->first].mediaOffset;
          trun.setDataOffset(dataOffset);
          size_t trunSample = 0;
          for (size_t index = run->first; index < run->second; ++index) {
            MP4::trunSampleInformation info = {};
            info.sampleSize = samples[index].sample.size;
            info.sampleDuration = samples[index].duration;
            info.sampleFlags = samples[index].flags;
            info.sampleOffset = samples[index].offset;
            trun.setSampleInformation(info, trunSample++);
          }
          traf.setContent(trun, trafEntry++);
        }
        // Future CENC senc/saiz/saio boxes belong in this traf before the sizing pass below so
        // their bytes are automatically included in every trun data_offset.
        moof.setContent(traf, moofEntry++);
      }
      return moof;
    };

    MP4::MOOF sizingMoof = buildMoof(0);
    const uint64_t mediaDataStart = sizingMoof.boxedSize() + 8;
    if (mediaDataStart + totalData > INT32_MAX) { return false; }
    MP4::MOOF moof = buildMoof(mediaDataStart);
    if (!moof.boxedSize()) { return false; }
    out.header.assign(moof.asBox(), moof.boxedSize());

    char mdatHeader[] = {0x00, 0x00, 0x00, 0x00, 'm', 'd', 'a', 't'};
    Bit::htobl(mdatHeader, totalData + 8);
    out.header.append(mdatHeader, 8);
    out.payloadSize = totalData;
    return true;
  }

  bool fragmentHeader(Util::ResizeablePointer &headOut, const DTSC::Meta &M,
                      const std::map<size_t, Comms::Users> &userSelect, uint64_t startTime,
                      uint64_t endTime, uint64_t sequenceNumber) {
    MuxedFragment fragment;
    if (!muxedFragment(fragment, M, userSelect, startTime, endTime, sequenceNumber)) { return false; }
    headOut.append(fragment.header.data(), fragment.header.size());
    return true;
  }

  bool fragmentHeader(Util::ResizeablePointer &headOut, const DTSC::Meta &M,
                      const std::map<size_t, Comms::Users> &userSelect, size_t fragmentIndex) {
    if (userSelect.empty()) { return false; }
    size_t timingTrack = M.mainTrack();
    if (!userSelect.count(timingTrack)) { timingTrack = userSelect.begin()->first; }
    if (!M.trackValid(timingTrack)) { return false; }
    DTSC::Fragments fragments(M.fragments(timingTrack));
    DTSC::Keys keys(M.getKeys(timingTrack));
    if (fragmentIndex < fragments.getFirstValid() || fragmentIndex >= fragments.getEndValid()) {
      return false;
    }
    const uint64_t duration = fragments.getDuration(fragmentIndex);
    if (!duration) { return false; }
    const uint64_t startTime = keys.getTime(fragments.getFirstKey(fragmentIndex));
    return fragmentHeader(headOut, M, userSelect, startTime, startTime + duration, fragmentIndex);
  }

}// namespace CMAF

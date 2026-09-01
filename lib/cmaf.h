#pragma once

#include "comms.h"
#include "dtsc.h"
#include "mp4_dash.h"
#include "mp4_generic.h"

#include <set>
#include <string>
#include <vector>

namespace CMAF{
  size_t payloadSize(const DTSC::Meta &M, size_t track, uint64_t startTime, uint64_t endTime);
  std::string trackHeader(const DTSC::Meta & M, size_t trackIndex, bool simplifyTrackIds = false);
  size_t keyHeaderSize(const DTSC::Meta &M, size_t track, size_t fragment);
  size_t keyHeaderSize(const DTSC::Meta &M, size_t track, uint64_t startTime, uint64_t endTime);
  std::string keyHeader(const DTSC::Meta &M, size_t track, uint64_t startTime, uint64_t endTime, uint64_t segmentNum, bool simplifyTrackIds = false, bool UTCTime = false);

  bool header(Util::ResizeablePointer & headOut, const DTSC::Meta & M, const std::map<size_t, Comms::Users> & userSelect);

  struct MuxedSample {
    size_t track;
    size_t part;
    uint64_t time;
    uint32_t size;
  };

  /// Complete description of one multiplexed fMP4 fragment. The caller MUST emit media bytes in
  /// samples order immediately after header; payloadSize and every trun data_offset describe that
  /// exact order.
  struct MuxedFragment {
    std::string header;
    std::vector<MuxedSample> samples;
    uint64_t payloadSize;
  };

  bool muxedFragment(MuxedFragment &out, const DTSC::Meta &M,
                     const std::map<size_t, Comms::Users> &userSelect, uint64_t startTime,
                     uint64_t endTime, uint64_t sequenceNumber);

  bool fragmentHeader(Util::ResizeablePointer & headOut, const DTSC::Meta & M,
                      const std::map<size_t, Comms::Users> & userSelect, uint64_t startTime,
                      uint64_t endTime, uint64_t sequenceNumber);

  /// Compatibility entry point for callers that address complete fragments by index. The selected
  /// main track defines the fragment range; incomplete fragments are rejected.
  bool fragmentHeader(Util::ResizeablePointer & headOut, const DTSC::Meta & M,
                      const std::map<size_t, Comms::Users> & userSelect, size_t fragmentIndex);
}// namespace CMAF

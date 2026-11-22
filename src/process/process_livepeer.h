#include "../output/output_ts_base.h"

#include <mist/defines.h>
#include <mist/json.h>
#include <mist/stream.h>
#include <mist/ts_stream.h>

#include <atomic>

namespace Mist{
  bool getFirst = false;
  bool sendFirst = false;

  uint64_t packetTimeDiff;
  uint64_t sendPacketTime;
  uint64_t keyCount = 0;
  JSON::Value opt; /// Options

  size_t nextFreeID = 0;

  class readySegment{
    public:
      uint64_t time;
      uint64_t lastPacket;
      int64_t timeOffset;
      int64_t sourceOffset = 0;
      int64_t compositionAdjustment = 0;
      uint64_t byteOffset;
      bool offsetCalcd;
      size_t ID;
      bool fullyRead;
      bool fullyWritten;
      TS::Stream S;
      Util::ResizeablePointer data;
      readySegment() {
        ID = nextFreeID++;
        time = 0;
        timeOffset = 0;
        byteOffset = 0;
        fullyRead = true;
        fullyWritten = false;
        offsetCalcd = false;
        lastPacket = 0;
      }
      void set(uint64_t t, void *ptr, size_t len, int64_t offset = 0) {
        time = t;
        sourceOffset = offset;
        compositionAdjustment = 0;
        data.assign(ptr, len);
        fullyRead = false;
        offsetCalcd = false;
        byteOffset = 0;
        fullyWritten = true;
      }
  };
  std::map<std::string, readySegment> segs;

#define PRESEG_COUNT 2
  class preparedSegment{
    public:
      uint64_t time;
      uint64_t videoTime = 0;
      int64_t videoOffset = 0;
      bool hasVideo = false;
      uint64_t segDuration;
      uint64_t keyNo;
      uint64_t width;
      uint64_t height;
      std::atomic<bool> fullyRead;
      std::atomic<bool> fullyWritten;
      Util::ResizeablePointer data;
      preparedSegment(){
        time = 0;
        keyNo = 0;
        segDuration = 0;
        fullyRead = true;
        fullyWritten = false;
      };
  };
  preparedSegment presegs[PRESEG_COUNT];

  JSON::Value lpEnc;
  JSON::Value lpBroad;
  std::string currBroadAddr;
  std::string lpID;

} // namespace Mist

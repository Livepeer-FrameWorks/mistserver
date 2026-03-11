#pragma once

#include "../input/input.h"
#include "../output/output.h"

#include <mist/defines.h>
#include <mist/json.h>
#include <mist/socket.h>
#include <mist/util.h>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace Mist {
  // Forward declarations
  class ProcessSink;
  class ProcessSource;

  // Global state needed by threads
  extern bool getFirst;
  extern bool sendFirst;
  extern uint64_t packetTimeDiff;
  extern uint64_t sendPacketTime;
  extern JSON::Value opt;
  extern ProcessSink *sinkClass;

  class ProcAV {
    private:
      std::unique_ptr<ProcessSink> sink;
      std::unique_ptr<ProcessSource> source;
      std::thread sinkThread;
      std::thread sourceThread;

      // Thread synchronization
      std::mutex threadMutex;
      std::condition_variable threadCV;
      bool isActive;

      // Thread management methods
      void runSinkThread();
      void runSourceThread();
      void runProcessThread();
      void updateStats(uint64_t startTime);

    public:
      ProcAV();
      ~ProcAV();

      void Run();
      void Stop();
      bool CheckConfig();
  };

  class ProcessSink : public Input {
    private:
      // SPS/PPS storage for H.264 parsing
      Util::ResizeablePointer ppsInfo;
      Util::ResizeablePointer spsInfo;

      // VPS/SPS/PPS storage for HEVC parsing
      Util::ResizeablePointer vpsInfo;

      // Track state - simple mapping from encoder node ID to track index
      std::unordered_map<size_t, size_t> encoderToTrackMap;

      // Private methods
      void parseH264(bool isKey, const char *bufIt, uint64_t bufSize);
      void setVideoInit(size_t & trackIdx, std::string codecOut, uint64_t outWidth, uint64_t outHeight,
                        const std::string & extradata, uint64_t outFpks);
      void setRawVideoInit(size_t & trackIdx, std::string codecOut, uint64_t outWidth, uint64_t outHeight, uint64_t outFpks);
      void setAudioInit(size_t & trackIdx, std::string codecOut, uint64_t outSampleRate, uint64_t outChannels,
                        uint64_t outBitDepth, const std::string & extraData);

      // Frame metrics
      uint64_t getProcessingTime() const;
      uint64_t getQueueTime() const;
      uint64_t getDecodeTime() const;
      uint64_t getEncodeTime() const;
      uint64_t getTransformTime() const;
      bool isHWAccelerated() const;
      std::string getHWDeviceName() const;

      std::mutex sinkMutex;

    public:
      explicit ProcessSink(Util::Config *cfg);
      ~ProcessSink() override; // Fixed destructor declaration

      void setNowMS(uint64_t t);

      // Required Input class overrides
      void streamMainLoop() override;
      void onData(void * data);
      bool checkArguments() override { return true; }
      bool needHeader() override { return false; }
      bool readHeader() override { return true; }
      bool openStreamSource() override { return true; }
      void parseStreamHeader() override {}
      bool needsLock() override { return false; }
      bool isSingular() override { return false; }
      bool publishesTracks() override { return false; }
      void connStats(Comms::Connections & statComm) override;
  };

  class ProcessSource : public Output {
    protected:
      inline virtual bool keepGoing() override {return config->is_active;}
    private:
      uint64_t lastJPEGSent{0};
      bool sendFirst{false}; //< Whether first packet has been sent

    public:
      explicit ProcessSource(Socket::Connection &c, Util::Config & _cfg, JSON::Value & _capa);
      virtual ~ProcessSource();

      virtual bool isRecording() override;
      static void init(Util::Config *cfg, JSON::Value &);
      virtual bool onFinish() override;
      virtual bool isReadyForPlay() override { return true; }
      virtual void dropTrack(size_t trackId, const std::string & reason, bool probablyBad = true) override;
      virtual void sendHeader() override;
      virtual void connStats(uint64_t now, Comms::Connections & statComm) override;
      virtual void sendNext() override;
  };

} // namespace Mist

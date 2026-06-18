#pragma once
#include "output.h"

#include <mist/device_ndi.h>
#include <mist/dtsc.h>

namespace Mist {
  class OutNDI : public Output {
    public:
      OutNDI(Socket::Connection & conn, Util::Config & cfg, JSON::Value & capa);
      ~OutNDI();
      static void init(Util::Config *cfg, JSON::Value & capa);
      void sendNext();
      void sendHeader();
      void syncVideoFrames();
      inline virtual bool keepGoing() { return config->is_active; }
      static bool listenMode(Util::Config *config) { return false; }

    protected:
      std::string streamName;
      NDI::Device dev;

      // Playback state
      bool isPlaying;
      bool useAsyncVideo;
      uint64_t asyncFramesSent;
      uint64_t asyncFramesSynced;

      // Current track configuration
      uint32_t currentWidth;
      uint32_t currentHeight;
      uint32_t currentFpks;
      uint32_t currentAudioRate;
      uint32_t currentAudioChannels;
      uint32_t currentAudioDepth;
      bool currentHasMetadata;

      // Metrics tracking
      uint64_t videoFramesSent;
      uint64_t audioFramesSent;
      uint64_t metadataFramesSent;
      uint64_t videoFramesDropped;
      uint64_t audioFramesDropped;
      uint64_t metadataFramesDropped;
      uint64_t videoFramesSkipped;
      uint64_t audioFramesSkipped;
      uint64_t lastMetricsUpdate;
      uint64_t lastVideoFrame;
      uint64_t lastAudioFrame;
  };
} // namespace Mist

typedef Mist::OutNDI mistOut;

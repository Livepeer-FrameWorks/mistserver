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
      inline virtual bool keepGoing() { return config->is_active; }
      static bool listenMode(Util::Config *config) { return false; }

    protected:
      std::string streamName;
      NDI::Device dev;

      // Playback state. In-class defaults so the destructor (which reads isPlaying) is safe even
      // when the constructor bails out early because the NDI runtime failed to initialize.
      bool isPlaying = false;

      // Current track configuration
      uint32_t currentWidth = 0;
      uint32_t currentHeight = 0;
      uint32_t currentFpks = 0;
      uint32_t currentAudioRate = 0;
      uint32_t currentAudioChannels = 0;
      uint32_t currentAudioDepth = 0;
      bool currentHasMetadata = false;

      // Metrics tracking
      uint64_t videoFramesSent = 0;
      uint64_t audioFramesSent = 0;
      uint64_t metadataFramesSent = 0;
      uint64_t videoFramesDropped = 0;
      uint64_t audioFramesDropped = 0;
      uint64_t metadataFramesDropped = 0;
      uint64_t videoFramesSkipped = 0;
      uint64_t audioFramesSkipped = 0;
      uint64_t lastMetricsUpdate = 0;
      uint64_t lastVideoFrame = 0;
      uint64_t lastAudioFrame = 0;
  };
} // namespace Mist

typedef Mist::OutNDI mistOut;

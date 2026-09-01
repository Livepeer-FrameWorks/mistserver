#pragma once

#include "input.h"

#include <mist/config.h>
#include <mist/defines.h>
#include <mist/device_ndi.h>
#include <mist/dtsc.h>
#include <mist/stream.h>

#include <memory>
#include <string>

namespace Mist {

  class inNDI : public Input {
    public:
      inNDI(Util::Config *cfg);
      ~inNDI();

      // Dynamic source switching methods
      bool switchSource(const std::string & sourceName);
      std::string getCurrentSourceName() const;

      /// Gets the web control URL for the NDI source, if available
      std::string getWebControlUrl() const { return webControlUrl; }

    protected:
      bool checkArguments();
      virtual bool needHeader() { return false; }
      virtual bool isSingular() { return true; }
      bool needsLock() { return false; }
      JSON::Value enumerateSources(const std::string & device);
      JSON::Value getSourceCapa(const std::string & device);

      void parseStreamHeader() {}
      bool openStreamSource();
      void closeStreamSource();
      void streamMainLoop();

    private:
      std::unique_ptr<NDI::Device> dev;
      std::unique_ptr<NDI::Discovery> sourceFinder;
      std::string ndiColorFormat;
      std::string ndiBandwidth;
      uint32_t audioSampleRate;
      uint32_t audioSampleDepth;
      uint32_t audioChannels;
      size_t videoTrackIdx;
      size_t audioTrackIdx;
      size_t metadataTrackIdx;
      bool force8Bit;
      bool hasFrames;
      uint32_t lastWidth;
      uint32_t lastHeight;
      uint32_t lastFpks;
      uint32_t lastFourCC; // last seen NDI video FourCC, to detect pixel-format changes

      // Performance monitoring
      uint64_t lastMetricsUpdate;
      float lastVideoFps;
      float lastDroppedFps;
      float lastAudioFps;
      int lastVideoQueue;
      int lastAudioQueue;
      int lastMetadataQueue;

      std::string webControlUrl;
      void updateWebControlUrl();

      // Convert an NDI frame's timing to milliseconds, tolerating undefined/synthesize sentinels
      // (INT64_MAX): prefer the receiver timestamp, fall back to the source timecode, then the
      // local clock.
      uint64_t ndiTimeToMs(int64_t timestamp, int64_t timecode) const;

      void bufferVideo(const NDIlib_video_frame_v2_t & frame);
      void bufferAudio(const NDIlib_audio_frame_v3_t & frame);
      void bufferMetadata(const NDIlib_metadata_frame_t & frame);
  };
} // namespace Mist

typedef Mist::inNDI mistIn;

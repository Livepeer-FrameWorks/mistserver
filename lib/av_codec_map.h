#pragma once

#include <cstdint>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/codec_id.h>
}

namespace AVCodecMap {
  const std::vector<std::string> & videoInputs();
  const std::vector<std::string> & audioInputs();
  bool isVideoInput(const std::string & codec);
  bool isAudioInput(const std::string & codec);
  AVCodecID codecID(const std::string & codec, unsigned int pcmDepth = 16);
  const char *mistCodec(AVCodecID codec);
} // namespace AVCodecMap

#pragma once

#include <cstdint>
#include <string>

extern "C" {
#include <libavcodec/codec_id.h>
}

namespace AVCodecMap {
  AVCodecID codecID(const std::string & codec, unsigned int pcmDepth = 16);
  const char *mistCodec(AVCodecID codec);
} // namespace AVCodecMap

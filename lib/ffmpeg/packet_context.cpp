#include "packet_context.h"

#include "../defines.h"
#include "utils.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/time.h>
}

#include <stdexcept>

namespace FFmpeg {

  PacketContext::PacketContext()
    : packet(av_packet_alloc()), codecParams(avcodec_parameters_alloc()), sourceNodeId(0), encoderNodeId(0) {
    if (!packet || !codecParams) {
      ERROR_MSG("PacketContext: Failed to allocate packet or codec parameters");
      throw std::runtime_error("PacketContext: Failed to allocate packet or codec parameters");
    }
  }

  PacketContext::PacketContext(AVPacket *pkt)
    : packet(pkt), codecParams(avcodec_parameters_alloc()), sourceNodeId(0), encoderNodeId(0) {
    if (!packet || !codecParams) {
      ERROR_MSG("PacketContext: Failed to allocate packet or codec parameters");
      throw std::runtime_error("PacketContext: Failed to allocate packet or codec parameters");
    }
  }

  PacketContext::PacketContext(const uint8_t *data, size_t size, int64_t pts, int64_t dts, bool isKey)
    : packet(av_packet_alloc()), codecParams(avcodec_parameters_alloc()), sourceNodeId(0), encoderNodeId(0) {
    if (!packet || !codecParams) {
      ERROR_MSG("PacketContext: Failed to allocate packet or codec parameters");
      throw std::runtime_error("PacketContext: Failed to allocate packet or codec parameters");
    }

    // Allocate and copy packet data
    uint8_t *buf = (uint8_t *)av_malloc(size);
    if (!buf) {
      ERROR_MSG("PacketContext: Failed to allocate packet buffer");
      throw std::runtime_error("PacketContext: Failed to allocate packet buffer");
    }
    memcpy(buf, data, size);

    // Set packet fields
    packet->data = buf;
    packet->size = size;
    packet->pts = pts;
    packet->dts = dts;
    if (isKey) { packet->flags |= AV_PKT_FLAG_KEY; }
  }

  PacketContext::~PacketContext() {
    if (codecParams) { avcodec_parameters_free(&codecParams); }
  }

  AVPacket *PacketContext::getPacket() const {
    return packet;
  }

  const uint8_t *PacketContext::getData() const {
    return packet ? packet->data : nullptr;
  }

  int PacketContext::getSize() const {
    return packet ? packet->size : 0;
  }

  int64_t PacketContext::getPts() const {
    return packet ? packet->pts : AV_NOPTS_VALUE;
  }

  int64_t PacketContext::getDts() const {
    return packet ? packet->dts : AV_NOPTS_VALUE;
  }

  int64_t PacketContext::getDuration() const {
    return packet ? packet->duration : 0;
  }

  bool PacketContext::isKeyframe() const {
    return packet ? (packet->flags & AV_PKT_FLAG_KEY) != 0 : false;
  }

  void PacketContext::setCodecParameters(const AVCodecParameters *params) {
    if (!params) {
      ERROR_MSG("PacketContext: Invalid codec parameters");
      return;
    }

    // Free existing parameters if any
    if (codecParams) { avcodec_parameters_free(&codecParams); }

    // Allocate and copy new parameters
    codecParams = avcodec_parameters_alloc();
    if (!codecParams) {
      ERROR_MSG("PacketContext: Failed to allocate codec parameters");
      return;
    }

    int ret = avcodec_parameters_copy(codecParams, params);
    if (ret < 0) {
      FFmpegUtils::printAvError("PacketContext: Failed to copy codec parameters", ret);
      avcodec_parameters_free(&codecParams);
      codecParams = nullptr;
      return;
    }

    // Validate parameters
    if (codecParams->codec_type == AVMEDIA_TYPE_VIDEO) {
      if (codecParams->width <= 0 || codecParams->height <= 0) {
        ERROR_MSG("PacketContext: Invalid video dimensions: %dx%d", codecParams->width, codecParams->height);
        avcodec_parameters_free(&codecParams);
        codecParams = nullptr;
        return;
      }
    } else if (codecParams->codec_type == AVMEDIA_TYPE_AUDIO) {
      if (codecParams->sample_rate <= 0 || codecParams->ch_layout.nb_channels <= 0) {
        ERROR_MSG("PacketContext: Invalid audio parameters: rate=%d, channels=%d", codecParams->sample_rate,
                  codecParams->ch_layout.nb_channels);
        avcodec_parameters_free(&codecParams);
        codecParams = nullptr;
        return;
      }
    }
  }

  void PacketContext::setSideData(AVPacketSideDataType type, const uint8_t *data, size_t size) {
    if (!packet || !data || !size) {
      ERROR_MSG("PacketContext: Invalid side data parameters");
      return;
    }

    // Validate side data type and size
    bool isValid = false;
    switch (type) {
      case AV_PKT_DATA_STRINGS_METADATA:
      case AV_PKT_DATA_PARAM_CHANGE:
      case AV_PKT_DATA_NEW_EXTRADATA:
        isValid = size > 0 && size <= 16384; // Max 16KB for metadata/params
        break;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(62, 0, 0)
      case AV_PKT_DATA_QUALITY_STATS: isValid = size > 0; break;
#else
      case AV_PKT_DATA_QUALITY_FACTOR: isValid = size == sizeof(int32_t); break;
#endif
      default:
        isValid = size > 0 && size <= 1048576; // 1MB general limit
        break;
    }

    if (!isValid) {
      ERROR_MSG("PacketContext: Invalid side data size %zu for type %d", size, type);
      return;
    }

    // Allocate new side data buffer
    uint8_t *sideData = (uint8_t *)av_malloc(size);
    if (!sideData) {
      ERROR_MSG("PacketContext: Failed to allocate side data buffer");
      return;
    }
    memcpy(sideData, data, size);

    // Remove existing side data of same type if present
    uint8_t *oldData = av_packet_get_side_data(packet, type, nullptr);
    if (oldData) { av_freep(&oldData); }

    // Add new side data
    if (av_packet_add_side_data(packet, type, sideData, size) < 0) {
      ERROR_MSG("PacketContext: Failed to add side data");
      av_freep(&sideData);
      return;
    }

    MEDIUM_MSG("PacketContext: Added side data: type=%d, size=%zu bytes", type, size);
  }

  const uint8_t *PacketContext::getSideData(AVPacketSideDataType type, size_t *size) const {
    if (!packet || !size) {
      ERROR_MSG("PacketContext: Invalid parameters for getSideData");
      return nullptr;
    }

    const uint8_t *data = av_packet_get_side_data(packet, type, size);
    if (!data) {
      MEDIUM_MSG("PacketContext: No side data found for type %d", type);
      return nullptr;
    }

    MEDIUM_MSG("PacketContext: Retrieved side data: type=%d, size=%zu bytes", type, *size);
    return data;
  }

  std::unique_ptr<PacketContext> PacketContext::clone() const {
    if (!packet) { return nullptr; }

    AVPacket *newPacket = av_packet_clone(packet);
    if (!newPacket) {
      ERROR_MSG("PacketContext: Failed to clone packet");
      return nullptr;
    }

    try {
      auto ctx = std::unique_ptr<PacketContext>(new PacketContext(newPacket));
      if (codecParams) { ctx->setCodecParameters(codecParams); }
      ctx->setSourceNodeId(sourceNodeId);
      ctx->setEncoderNodeId(encoderNodeId);
      return ctx;
    } catch (...) {
      av_packet_free(&newPacket);
      return nullptr;
    }
  }

  void PacketContext::reset() {
    if (packet) { av_packet_unref(packet); }
    if (codecParams) {
      avcodec_parameters_free(&codecParams);
      codecParams = avcodec_parameters_alloc();
      if (!codecParams) {
        ERROR_MSG("PacketContext: Failed to allocate codec parameters during reset");
        throw std::runtime_error("PacketContext: Failed to allocate codec parameters");
      }
    }
    sourceNodeId = 0; // Reset source node ID
    encoderNodeId = 0; // Reset encoder node ID
  }

  void PacketContext::setCodecParameters(int width, int height, AVCodecID codecId, const std::string & extradata) {
    // Free existing parameters if any
    if (codecParams) { avcodec_parameters_free(&codecParams); }

    // Allocate new parameters
    codecParams = avcodec_parameters_alloc();
    if (!codecParams) {
      ERROR_MSG("PacketContext: Failed to allocate codec parameters");
      return;
    }

    // Set basic parameters
    codecParams->codec_type = AVMEDIA_TYPE_VIDEO;
    codecParams->codec_id = codecId;
    codecParams->width = width;
    codecParams->height = height;

    // Set extradata if provided
    if (extradata.size()) {
      codecParams->extradata = (uint8_t *)av_mallocz(extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE);
      if (!codecParams->extradata) {
        ERROR_MSG("PacketContext: Failed to allocate extradata buffer");
        return;
      }
      memcpy(codecParams->extradata, extradata.data(), extradata.size());
      codecParams->extradata_size = extradata.size();
    }

    VERYHIGH_MSG("PacketContext: Set codec parameters: id=%d, type=%d, width=%d, height=%d, extradata=%d bytes",
               codecParams->codec_id, codecParams->codec_type, codecParams->width, codecParams->height, codecParams->extradata_size);
  }

  size_t PacketContext::getSourceNodeId() const {
    return sourceNodeId;
  }

  void PacketContext::setSourceNodeId(size_t id) {
    sourceNodeId = id;
  }

  size_t PacketContext::getEncoderNodeId() const {
    return encoderNodeId;
  }

  void PacketContext::setEncoderNodeId(size_t id) {
    encoderNodeId = id;
  }

  const std::string & PacketContext::getCodecData() const {
    return codecData;
  }

  void PacketContext::setPts(int64_t pts) {
    if (packet) { packet->pts = pts; }
  }

  void PacketContext::setDts(int64_t dts) {
    if (packet) { packet->dts = dts; }
  }

  void PacketContext::setKeyframe(bool isKey) {
    if (packet) {
      if (isKey) {
        packet->flags |= AV_PKT_FLAG_KEY;
      } else {
        packet->flags &= ~AV_PKT_FLAG_KEY;
      }
    }
  }

  void PacketContext::setCodecData(const uint8_t *data, size_t size) {
    codecData.assign((char *)data, size);
  }

  void PacketContext::setFormatInfo(const FormatInfo & info) {
    formatInfo = info;
  }

  const PacketContext::FormatInfo & PacketContext::getFormatInfo() const {
    return formatInfo;
  }

  uint64_t PacketContext::getFrameRate() const {
    return formatInfo.fpks;
  }

} // namespace FFmpeg

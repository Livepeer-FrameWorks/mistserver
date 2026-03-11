#include "audio_encoder_node.h"

#include "../timing.h"
#include "packet_context.h"
#include "utils.h"

#include <algorithm>
extern "C" {
#include <libavcodec/codec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
}

namespace FFmpeg {

  AudioEncoderNode::AudioEncoderNode(const char *_codecName, uint64_t _sampleRate, uint64_t _channels,
                                     uint64_t _bitrate, uint64_t _bitDepth) {
    // Initialize base class members
    codecName = _codecName ? _codecName : "";
    sampleRate = _sampleRate;
    channels = _channels;
    bitrate = _bitrate;
    bitDepth = _bitDepth;
    targetBitDepth = _bitDepth;
    init();
  }

  AudioEncoderNode::~AudioEncoderNode() {
    if (codecCtx) { avcodec_free_context(&codecCtx); }
    if (audioBuffer) {
      av_audio_fifo_free(audioBuffer);
      audioBuffer = nullptr;
    }
  }

  bool AudioEncoderNode::init() {
    // Find encoder - map codec names to correct libav codec IDs
    if (strcmp(codecName.c_str(), "PCM") == 0) {
      codec = avcodec_find_encoder(AV_CODEC_ID_PCM_S32BE);
    } else if (strcmp(codecName.c_str(), "opus") == 0) {
      codec = avcodec_find_encoder(AV_CODEC_ID_OPUS);
    } else if (strcmp(codecName.c_str(), "AAC") == 0) {
      codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    } else if (strcmp(codecName.c_str(), "MP3") == 0) {
      codec = avcodec_find_encoder(AV_CODEC_ID_MP3);
    } else if (strcmp(codecName.c_str(), "FLAC") == 0) {
      codec = avcodec_find_encoder(AV_CODEC_ID_FLAC);
    } else if (strcmp(codecName.c_str(), "vorbis") == 0) {
      codec = avcodec_find_encoder(AV_CODEC_ID_VORBIS);
    } else {
      // Fall back to find by name for other codecs
      codec = avcodec_find_encoder_by_name(codecName.c_str());
    }

    if (!codec) {
      ERROR_MSG("Audio Encoder: Could not find encoder '%s'", codecName.c_str());
      return false;
    }
    INFO_MSG("Audio Encoder: Found encoder: %s", codec->long_name);

    // Initialize format requirements
    initFormatRequirements();

    // Allocate codec context
    codecCtx = avcodec_alloc_context3(codec);
    if (!codecCtx) {
      ERROR_MSG("Audio Encoder: Could not allocate codec context");
      return false;
    }

    // Set basic parameters
    codecCtx->sample_rate = sampleRate;
    codecCtx->ch_layout.nb_channels = channels;
    codecCtx->bit_rate = bitrate;

    // Set time base
    codecCtx->time_base = (AVRational){1, (int)sampleRate};
    codecCtx->pkt_timebase = codecCtx->time_base;

    // Set sample format (will be negotiated with encoder)
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 19, 100)
    const enum AVSampleFormat *supported_formats = nullptr;
    // Use new API for FFmpeg 7.x
    int ret_config = avcodec_get_supported_config(codecCtx, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0,
                                                  (const void **)&supported_formats, nullptr);
    if (ret_config >= 0 && supported_formats && supported_formats[0] != AV_SAMPLE_FMT_NONE) {
      codecCtx->sample_fmt = supported_formats[0];
    } else {
      codecCtx->sample_fmt = AV_SAMPLE_FMT_FLTP; // Default
    }
#else
    // Use older API for FFmpeg 6.x and earlier
    if (codec->sample_fmts && codec->sample_fmts[0] != AV_SAMPLE_FMT_NONE) {
      codecCtx->sample_fmt = codec->sample_fmts[0];
    } else {
      codecCtx->sample_fmt = AV_SAMPLE_FMT_FLTP; // Default
    }
#endif

    // Set channel layout
    if (channels == 1) {
      codecCtx->ch_layout = AV_CHANNEL_LAYOUT_MONO;
    } else if (channels == 2) {
      codecCtx->ch_layout = AV_CHANNEL_LAYOUT_STEREO;
    } else {
      av_channel_layout_default(&codecCtx->ch_layout, static_cast<int>(channels));
      codecCtx->ch_layout.nb_channels = static_cast<unsigned int>(channels);
    }

    // Set strict compliance for all audio codecs
    codecCtx->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;

    // Codec-specific initialization
    if (strcmp(codecName.c_str(), "AAC") == 0) {
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(62, 0, 0)
      codecCtx->profile = AV_PROFILE_AAC_LOW; // LOW profile - the default and most compatible
#else
      codecCtx->profile = FF_PROFILE_AAC_LOW; // LOW profile - the default and most compatible
#endif
      codecCtx->sample_fmt = AV_SAMPLE_FMT_FLTP;
    } else if (strcmp(codecName.c_str(), "opus") == 0) {
      // Opus requires specific configuration
      codecCtx->sample_fmt = AV_SAMPLE_FMT_S16;
      codecCtx->sample_rate = 48000; // Opus requires 48000 Hz
      sampleRate = 48000;
      codecCtx->time_base = (AVRational){1, 48000};
      codecCtx->compression_level = 10;

      // Validate bitrate range for Opus
      if (codecCtx->bit_rate < 500) {
        WARN_MSG("Audio Encoder: Opus does not support bitrate %" PRIu64 ", setting to 500", codecCtx->bit_rate);
        codecCtx->bit_rate = 500;
      } else if (codecCtx->bit_rate > 512000) {
        WARN_MSG("Audio Encoder: Opus does not support bitrate %" PRIu64 ", setting to 512000", codecCtx->bit_rate);
        codecCtx->bit_rate = 512000;
      }
    } else if (strcmp(codecName.c_str(), "MP3") == 0) {
      codecCtx->compression_level = 2;
    } else if (strcmp(codecName.c_str(), "FLAC") == 0) {
      codecCtx->compression_level = 5; // FLAC compression level 0-12, 5 is good balance
      codecCtx->sample_fmt = AV_SAMPLE_FMT_S32; // FLAC prefers 32-bit samples
    } else if (strcmp(codecName.c_str(), "vorbis") == 0) {
      codecCtx->compression_level = 3; // Vorbis quality level -1 to 10, 3 is good quality
      codecCtx->sample_fmt = AV_SAMPLE_FMT_FLTP; // Vorbis uses floating point
    } else if (strcmp(codecName.c_str(), "PCM") == 0) {
      codecCtx->sample_fmt = AV_SAMPLE_FMT_S32;
      codecCtx->sample_rate = 48000;
      sampleRate = 48000;
      codecCtx->time_base = (AVRational){1, 48000};
    }

    // Verify format is supported
    if (!formatRequirements.isFormatSupported(codecCtx->sample_fmt)) {
      // Fall back to first supported format
      if (!formatRequirements.supportedFormats.empty()) {
        codecCtx->sample_fmt = formatRequirements.supportedFormats[0];
        WARN_MSG("Audio Encoder: Sample format not supported, falling back to %s", av_get_sample_fmt_name(codecCtx->sample_fmt));
      } else {
        ERROR_MSG("Audio Encoder: No supported sample formats found for codec %s", codec->name);
        return false;
      }
    }

    // Store the final sample format in targetFormat for getSampleFormat() method
    targetFormat = codecCtx->sample_fmt;
    INFO_MSG("Audio Encoder: Set target format to %s", av_get_sample_fmt_name(targetFormat));

    // Set quality or bitrate
    AVDictionary *opts = nullptr;

    // Add codec-specific dictionary options before quality/bitrate settings
    if (strcmp(codecName.c_str(), "opus") == 0) {
      // Opus-specific options
      if (channels == 16) { av_dict_set_int(&opts, "mapping_family", 255, 0); }
      // Set application type for Opus
      av_dict_set(&opts, "application", "audio", 0);
    }

    if (targetQuality >= 0) {
      // Quality-based encoding (VBR)
      if (!formatRequirements.supportsVBR) {
        WARN_MSG("Audio Encoder: VBR not supported by codec %s, using CBR instead", codec->name);
        targetQuality = -1;
      } else if (!formatRequirements.isQualitySupported(targetQuality)) {
        WARN_MSG("Audio Encoder: Quality value %d not supported, clamping to range [%d, %d]", targetQuality,
                 formatRequirements.minQuality, formatRequirements.maxQuality);
        targetQuality = std::max<int>(formatRequirements.minQuality, std::min<int>(targetQuality, formatRequirements.maxQuality));
      }

      if (targetQuality >= 0) {
        // Configure VBR settings
        if (strcmp(codec->name, "opus") == 0) {
          av_dict_set(&opts, "vbr", "on", 0);
          av_dict_set_int(&opts, "compression_level", targetQuality, 0);
        } else if (strcmp(codec->name, "aac") == 0) {
          av_dict_set(&opts, "q", std::to_string(targetQuality).c_str(), 0);
        }
        codecCtx->flags |= AV_CODEC_FLAG_QSCALE;
        codecCtx->global_quality = targetQuality * FF_QP2LAMBDA;
      }
    }

    if (targetQuality < 0) {
      // Bitrate-based encoding (CBR)
      if (!formatRequirements.isBitrateSupported(bitrate)) {
        WARN_MSG("Audio Encoder: Bitrate %" PRIu64 " not supported, clamping to range [%d, %d]", bitrate,
                 formatRequirements.minBitrate, formatRequirements.maxBitrate);
        bitrate = std::max<int64_t>(formatRequirements.minBitrate, std::min<int64_t>(bitrate, formatRequirements.maxBitrate));
      }
      codecCtx->bit_rate = bitrate;

      // Apply codec-specific CBR settings
      if (strcmp(codec->name, "opus") == 0) {
        av_dict_set(&opts, "vbr", "off", 0);
        av_dict_set(&opts, "application", "audio", 0);
      } else if (strcmp(codec->name, "aac") == 0) {
        av_dict_set(&opts, "q", "0", 0); // Disable VBR
      }
    }

    // Open codec
    int ret = avcodec_open2(codecCtx, codec, &opts);
    if (opts) { av_dict_free(&opts); }
    if (ret < 0) {
      FFmpegUtils::printAvError("Audio Encoder: Could not open codec", ret);
      return false;
    }

    updateLongName();
    INFO_MSG("Successfully (re)initialized %s", longName.c_str());
    return true;
  }

  void AudioEncoderNode::updateLongName() {
    std::string lower = codecName;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    if (lower.rfind("pcm_", 0) == 0) {
      longName = "AudioRawPacketizer(";
    } else {
      longName = "AudioEncoder(";
    }
    longName += codecName;
    if (targetQuality) {
      longName += ", quality=" + std::to_string(targetQuality);
    } else if (bitrate) {
      longName += ", bitrate=" + std::to_string(bitrate);
    }
    if (targetBitDepth) { longName += ", " + std::to_string(targetBitDepth) + "bit"; }
    if (channels == 1) {
      longName += ", mono";
    } else if (channels == 2) {
      longName += ", stereo";
    } else {
      longName += ", " + std::to_string(channels) + "ch";
    }
    if (!(sampleRate % 1000)) {
      longName += ", " + std::to_string(sampleRate / 1000) + "kHz";
    } else if (!(sampleRate % 100)) {
      longName += ", " + std::to_string(sampleRate / 1000) + "." + std::to_string((sampleRate / 100) % 10) + "kHz";
    } else {
      longName += ", " + std::to_string(sampleRate) + "Hz";
    }
    longName += ")";
  }

  bool AudioEncoderNode::processDirectly(AVFrame *frame) {
    uint64_t startTime = Util::getMicros();

    // Use internal sample counting for FFmpeg operations
    // This ensures proper encoder behavior and frame ordering
    frame->pts = totalSamplesProcessed;

    VERYHIGH_MSG("Audio Encoder: Direct processing frame with %d samples at internal PTS %" PRIu64 "",
                 frame->nb_samples, frame->pts);

    // Send frame for encoding
    int ret = avcodec_send_frame(codecCtx, frame);
    if (ret < 0) {
      FFmpegUtils::printAvError("Audio Encoder: Error sending frame to encoder", ret);
      droppedFrames++;
      return false;
    }

    // Get encoded packet
    AVPacket *packet = av_packet_alloc();
    ret = avcodec_receive_packet(codecCtx, packet);
    if (ret < 0) {
      if (ret != AVERROR(EAGAIN)) {
        FFmpegUtils::printAvError("Audio Encoder: Error receiving packet from encoder", ret);
        droppedFrames++;
      }
      av_packet_free(&packet);
      return false;
    }

    // Create output packet context
    PacketContext outputPacket(packet);
    outputPacket.setEncoderNodeId(getNodeId());

    // Calculate timestamp based on current sample position relative to baseline
    uint64_t currentSamplePosition = totalSamplesProcessed;
    uint64_t samplesSinceBaseline = currentSamplePosition - baselineSamplePosition;
    int64_t timeSinceBaselineMs = (samplesSinceBaseline * 1000) / codecCtx->sample_rate;

    int64_t outputTimestampMs = baselineTimestampMs + timeSinceBaselineMs;
    outputPacket.setPts(outputTimestampMs);
    lastOutputTimestampMs = outputTimestampMs;

    MEDIUM_MSG("Audio Encoder: Continuous PTS: baseline=%" PRIu64 " ms, samples_since_baseline=%" PRIu64 ", "
               "time_offset=%" PRIu64 " ms, output=%" PRIu64 " ms",
               baselineTimestampMs, samplesSinceBaseline, timeSinceBaselineMs, outputTimestampMs);

    // Set format info with codec name and audio parameters
    PacketContext::FormatInfo formatInfo;
    formatInfo.codecName = codecName;
    formatInfo.codecId = codecCtx->codec_id;
    formatInfo.channels = codecCtx->ch_layout.nb_channels;
    formatInfo.sampleRate = codecCtx->sample_rate;
    formatInfo.bitDepth = av_get_bytes_per_sample(codecCtx->sample_fmt) * 8;
    formatInfo.isKeyframe = true; // Audio packets are always considered keyframes
    formatInfo.width = 0; // Not applicable for audio
    formatInfo.height = 0; // Not applicable for audio
    formatInfo.fpks = 0; // Not applicable for audio

    outputPacket.setFormatInfo(formatInfo);

    // Copy codec extradata if available (critical for AAC, Opus, etc.)
    if (codecCtx->extradata && codecCtx->extradata_size > 0) {
      outputPacket.setCodecData(codecCtx->extradata, codecCtx->extradata_size);
      INSANE_MSG("Audio Encoder: Set codec extradata: %d bytes for codec %s", codecCtx->extradata_size, codecName.c_str());
    }

    // Update metrics
    totalProcessingTime.fetch_add(Util::getMicros(startTime));
    totalSamplesProcessed += frame->nb_samples;

    // Call outputs
    for (auto & cb : callbacks) { cb(&outputPacket); }

    VERYHIGH_MSG("Audio Encoder: Successfully encoded frame, total samples: %" PRIu64 "", totalSamplesProcessed);
    return true;
  }

  bool AudioEncoderNode::processWithFrameBuffering(AVFrame *inputFrame) {
    uint64_t startTime = Util::getMicros();
    // Initialize audio buffer if needed
    if (!audioBuffer) {
      int expectedFrameSize = getExpectedFrameSize();
      int bufferSize = std::max(expectedFrameSize * 10, 4096);

      audioBuffer = av_audio_fifo_alloc(codecCtx->sample_fmt, codecCtx->ch_layout.nb_channels, bufferSize);
      if (!audioBuffer) {
        ERROR_MSG("Audio Encoder: Failed to allocate audio buffer");
        return false;
      }

      MEDIUM_MSG("Audio Encoder: Initialized frame buffer for frame size mismatch (input: %d, "
                 "expected: %d, buffer: %d)",
                 inputFrame->nb_samples, expectedFrameSize, bufferSize);
    }

    // Check for buffer overflow and expand if needed
    int currentBufferSize = av_audio_fifo_size(audioBuffer);
    int bufferSpace = av_audio_fifo_space(audioBuffer);

    if (inputFrame->nb_samples > bufferSpace) {
      // Calculate new buffer size needed
      int totalNeeded = currentBufferSize + inputFrame->nb_samples;
      int newBufferSize = std::max(totalNeeded * 2, 4096); // Double the needed size for future frames

      MEDIUM_MSG("Audio Encoder: Expanding buffer from %d to %d samples to accommodate input frame "
                 "of %d samples",
                 currentBufferSize + bufferSpace, newBufferSize, inputFrame->nb_samples);

      // Reallocate buffer with larger size
      int ret = av_audio_fifo_realloc(audioBuffer, newBufferSize);
      if (ret < 0) {
        ERROR_MSG("Audio Encoder: Failed to expand audio buffer to %d samples", newBufferSize);
        return false;
      }

      bufferSpace = av_audio_fifo_space(audioBuffer);
    }

    // Add input frame to buffer
    int ret = av_audio_fifo_write(audioBuffer, (void **)inputFrame->data, inputFrame->nb_samples);
    if (ret < inputFrame->nb_samples) {
      ERROR_MSG("Audio Encoder: Failed to write samples to audio buffer (wrote %d of %d)", ret, inputFrame->nb_samples);
      return false;
    }

    VERYHIGH_MSG("Audio Encoder: Buffered %d samples, total buffered: %d", inputFrame->nb_samples, av_audio_fifo_size(audioBuffer));

    // Process buffered frames if we have enough samples
    int expectedFrameSize = getExpectedFrameSize();
    bool processedAny = false;
    int framesProcessed = 0;

    // Allocate frame buffer if needed
    if (!encoderFrame || encoderFrame->format != codecCtx->sample_fmt || encoderFrame->sample_rate != codecCtx->sample_rate ||
        encoderFrame->ch_layout.nb_channels != codecCtx->ch_layout.nb_channels || encoderFrame->nb_samples != expectedFrameSize) {
      INFO_MSG("(Re)allocating frame buffer for encoder");
      if (encoderFrame) { av_frame_free(&encoderFrame); }
      encoderFrame = av_frame_alloc();
      if (!encoderFrame) {
        ERROR_MSG("Audio Encoder: Failed to allocate encoder frame");
        return processedAny;
      }

      // Set frame properties to match encoder requirements
      encoderFrame->format = codecCtx->sample_fmt;
      encoderFrame->sample_rate = codecCtx->sample_rate;
      encoderFrame->ch_layout = codecCtx->ch_layout;
      encoderFrame->nb_samples = expectedFrameSize;

      ret = av_frame_get_buffer(encoderFrame, 0);
      if (ret < 0) {
        ERROR_MSG("Audio Encoder: Failed to allocate frame buffer");
        av_frame_free(&encoderFrame);
        return processedAny;
      }
    }

    while (av_audio_fifo_size(audioBuffer) >= expectedFrameSize) {
      // Allocate frame for encoder
      // Read samples from buffer
      ret = av_audio_fifo_read(audioBuffer, (void **)encoderFrame->data, expectedFrameSize);
      if (ret < expectedFrameSize) {
        ERROR_MSG("Audio Encoder: Failed to read samples from audio buffer (read %d of %d)", ret, expectedFrameSize);
        return processedAny;
      }

      // Send frame for encoding (using internal sample counting for FFmpeg)
      encoderFrame->pts = totalSamplesProcessed;

      VERYHIGH_MSG("Audio Encoder: Buffered frame encoding: PTS=%" PRIu64 ", samples=%d", encoderFrame->pts, expectedFrameSize);

      int encodeRet = avcodec_send_frame(codecCtx, encoderFrame);
      if (encodeRet < 0) {
        FFmpegUtils::printAvError("Audio Encoder: Error sending buffered frame to encoder", encodeRet);
        return processedAny;
      }

      // Get encoded packet
      AVPacket *packet = av_packet_alloc();
      encodeRet = avcodec_receive_packet(codecCtx, packet);
      if (encodeRet < 0) {
        if (encodeRet != AVERROR(EAGAIN)) {
          FFmpegUtils::printAvError("Audio Encoder: Error receiving packet from buffered frame", encodeRet);
          av_packet_free(&packet);
          return processedAny;
        }
        // AVERROR(EAGAIN) is normal - encoder needs more input frames
        av_packet_free(&packet);

        // Update sample counter and continue processing
        totalSamplesProcessed += expectedFrameSize;
        framesProcessed++;
        processedAny = true;
        continue;
      }

      // Create output packet with smart timestamp
      PacketContext outputPacket(packet);
      outputPacket.setEncoderNodeId(getNodeId());

      // Calculate timestamp based on current sample position relative to baseline
      uint64_t currentSamplePosition = totalSamplesProcessed;
      uint64_t samplesSinceBaseline = currentSamplePosition - baselineSamplePosition;
      int64_t timeSinceBaselineMs = (samplesSinceBaseline * 1000) / codecCtx->sample_rate;

      int64_t outputTimestampMs = baselineTimestampMs + timeSinceBaselineMs;
      outputPacket.setPts(outputTimestampMs);
      lastOutputTimestampMs = outputTimestampMs;

      HIGH_MSG("Audio Encoder: Continuous PTS: baseline=%" PRIu64 " ms, samples_since_baseline=%" PRIu64 ", "
               "time_offset=%" PRIu64 " ms, output=%" PRIu64 " ms",
               baselineTimestampMs, samplesSinceBaseline, timeSinceBaselineMs, outputTimestampMs);

      // Set format info
      PacketContext::FormatInfo formatInfo;
      formatInfo.codecName = codecName;
      formatInfo.codecId = codecCtx->codec_id;
      formatInfo.channels = codecCtx->ch_layout.nb_channels;
      formatInfo.sampleRate = codecCtx->sample_rate;
      formatInfo.bitDepth = av_get_bytes_per_sample(codecCtx->sample_fmt) * 8;
      formatInfo.isKeyframe = true;
      outputPacket.setFormatInfo(formatInfo);

      // Copy codec extradata if available (critical for AAC, Opus, etc.)
      if (codecCtx->extradata && codecCtx->extradata_size > 0) {
        outputPacket.setCodecData(codecCtx->extradata, codecCtx->extradata_size);
        DONTEVEN_MSG("Audio Encoder: Set codec extradata: %d bytes for codec %s", codecCtx->extradata_size, codecName.c_str());
      }

      // Update sample counter
      totalSamplesProcessed += expectedFrameSize;
      framesProcessed++;
      totalProcessingTime.fetch_add(Util::getMicros(startTime));

      // Call outputs
      for (auto & cb : callbacks) { cb(&outputPacket); }

      processedAny = true;

    }

    if (framesProcessed > 0) {
      VERYHIGH_MSG("Audio Encoder: Processed %d buffered frames, %d samples remaining in buffer", framesProcessed,
                   av_audio_fifo_size(audioBuffer));
    }

    return processedAny;
  }

  bool AudioEncoderNode::receivePacket(AVPacket *packet) {
    if (!codecCtx || !packet) {
      WARN_MSG("Audio Encoder: Invalid encoder state - context: %p, packet: %p", codecCtx, packet);
      return false;
    }

    int ret = avcodec_receive_packet(codecCtx, packet);
    if (ret < 0 && ret != AVERROR(EAGAIN)) {
      FFmpegUtils::printAvError("Audio Encoder: Error receiving packet from encoder", ret);
      return false;
    }

    if (ret >= 0) {
      VERYHIGH_MSG("Audio Encoder: Received encoded packet of size %d at pts %" PRIu64 "", packet->size, packet->pts);
    }
    return ret >= 0;
  }

  bool AudioEncoderNode::validateFormat(const AudioFrameContext *frame) const {
    if (!frame || !frame->getAVFrame()) {
      ERROR_MSG("Audio Encoder: validateFormat - frame is null or has null AVFrame");
      return false;
    }

    AVFrame *avFrame = frame->getAVFrame();

    // Check sample rate
    if (avFrame->sample_rate != codecCtx->sample_rate) {
      ERROR_MSG("Audio Encoder: Sample rate mismatch: got %d, expected %d", avFrame->sample_rate, codecCtx->sample_rate);
      return false;
    }

    // Check channel count
    if (avFrame->ch_layout.nb_channels != codecCtx->ch_layout.nb_channels) {
      ERROR_MSG("Audio Encoder: Channel count mismatch: got %d, expected %d", avFrame->ch_layout.nb_channels,
                codecCtx->ch_layout.nb_channels);
      return false;
    }

    // Check sample format
    if (avFrame->format != codecCtx->sample_fmt) {
      ERROR_MSG("Audio Encoder: Sample format mismatch: got %s, expected %s",
                av_get_sample_fmt_name(static_cast<AVSampleFormat>(avFrame->format)),
                av_get_sample_fmt_name(codecCtx->sample_fmt));
      return false;
    }

    return true;
  }

  bool AudioEncoderNode::setInput(void * inFrame, size_t idx) {
    AudioFrameContext * frame = (AudioFrameContext*)inFrame;
    if (!frame) {
      ERROR_MSG("Audio Encoder: Attempted to set null input frame at index %zu", idx);
      return false;
    }
    if (!codecCtx) {
      WARN_MSG("Audio Encoder: Invalid encoder state - context: %p", codecCtx);
      droppedFrames++;
      return false;
    }

    std::lock_guard<std::mutex> lock(mutex);

    // Track input timestamp for continuous timeline
    int64_t currentInputTimestamp = frame->getPts();
    int inputSamples = frame->getAVFrame()->nb_samples;

    // Calculate expected timestamp based on samples received so far
    int64_t expectedTimestamp = -1;
    if (baselineTimestampMs != -1 && totalInputSamples > 0) {
      // Expected timestamp = baseline + time for all samples received
      int64_t timeSinceBaselineMs = (totalInputSamples * 1000) / codecCtx->sample_rate;
      expectedTimestamp = baselineTimestampMs + timeSinceBaselineMs;
    }

    // Detect timestamp discontinuity
    bool isDiscontinuity = false;
    if (baselineTimestampMs == -1) {
      // First frame - establish baseline
      isDiscontinuity = true;
      MEDIUM_MSG("Audio Encoder: First frame - establishing baseline");
    } else if (expectedTimestamp != -1) {
      // Check if actual timestamp matches expected (within 1ms tolerance)
      int64_t timestampDiff = abs(currentInputTimestamp - expectedTimestamp);
      if (timestampDiff > 1) {
        isDiscontinuity = true;
        MEDIUM_MSG("Audio Encoder: Timestamp discontinuity detected - expected: %" PRIu64 " ms, actual: "
                   "%" PRIu64 " ms, diff: %" PRIu64 " ms",
                   expectedTimestamp, currentInputTimestamp, timestampDiff);
      }
    }

    // Adjust baseline if discontinuity detected
    if (isDiscontinuity) {
      if (baselineTimestampMs == -1) {
        // First frame - establish initial baseline
        MEDIUM_MSG("Audio Encoder: Establishing initial baseline - timestamp: %" PRIu64 " ms, "
                   "total_input_samples: %" PRIu64 "",
                   currentInputTimestamp, totalInputSamples);
        baselineTimestampMs = currentInputTimestamp;
        baselineSamplePosition = totalInputSamples;
      } else {
        // Discontinuity - shift baseline to maintain timing of previously buffered samples
        int64_t timestampShift = currentInputTimestamp - expectedTimestamp;
        baselineTimestampMs += timestampShift;

        MEDIUM_MSG("Audio Encoder: Shifting baseline due to discontinuity - shift: %" PRIu64 " ms, "
                   "new_baseline: %" PRIu64 " ms",
                   timestampShift, baselineTimestampMs);

        // Keep baselineSamplePosition unchanged to preserve timing of buffered samples
      }
    }

    // Update tracking
    lastInputTimestampMs = currentInputTimestamp;
    totalInputSamples += inputSamples;

    DONTEVEN_MSG("Audio Encoder: Input frame @ pts=%" PRIu64 ", samples=%d, total_input=%" PRIu64 ", baseline=%" PRIu64 "",
                 currentInputTimestamp, inputSamples, totalInputSamples, baselineTimestampMs);

    AVFrame *inputFrame = frame->getAVFrame();

    // Check if we need frame buffering based on frame size mismatch
    int expectedFrameSize = getExpectedFrameSize();
    int inputFrameSize = inputFrame->nb_samples;

    if (expectedFrameSize > 0 && inputFrameSize != expectedFrameSize) {
      // Frame sizes don't match - use buffering
      return processWithFrameBuffering(inputFrame);
    } else {
      // Frame sizes match or encoder doesn't specify frame size - process directly
      return processDirectly(inputFrame);
    }
  }

  void AudioEncoderNode::initFormatRequirements() {
    if (!codec) { return; }

    // Get supported sample formats
    formatRequirements.supportedFormats = getSupportedSampleFormats();

    // Get supported channel layouts
    formatRequirements.supportedLayouts = getSupportedChannelLayouts();

    // Get supported sample rates
    formatRequirements.supportedSampleRates = getSupportedSampleRates();

    // Initialize default quality/bitrate limits
    formatRequirements.minBitrate = 8000; // 8 kbps
    formatRequirements.maxBitrate = 512000; // 512 kbps
    formatRequirements.minQuality = 0;
    formatRequirements.maxQuality = 10;
    formatRequirements.supportsVBR = false;

    // Codec-specific adjustments
    if (strcmp(codec->name, "aac") == 0) {
      // AAC codec requirements
      formatRequirements.minBitrate = 8000; // 8 kbps
      formatRequirements.maxBitrate = 512000; // 512 kbps
      formatRequirements.supportsVBR = true;
      formatRequirements.minQuality = 1; // VBR quality scale 1-5
      formatRequirements.maxQuality = 5;
    } else if (strcmp(codec->name, "opus") == 0) {
      // Opus codec requirements
      formatRequirements.minBitrate = 6000; // 6 kbps
      formatRequirements.maxBitrate = 510000; // 510 kbps
      formatRequirements.supportsVBR = true;
      formatRequirements.minQuality = 0; // VBR quality scale 0-10
      formatRequirements.maxQuality = 10;
    } else if (strcmp(codec->name, "pcm_s16le") == 0 || strcmp(codec->name, "pcm_s32le") == 0) {
      // PCM codec requirements
      formatRequirements.minBitrate = 0; // No minimum
      formatRequirements.maxBitrate = 0; // No maximum
      formatRequirements.supportsVBR = false;
      formatRequirements.minQuality = 0; // No quality settings
      formatRequirements.maxQuality = 0;
    }
  }

  std::vector<AVSampleFormat> AudioEncoderNode::getSupportedSampleFormats() const {
    std::vector<AVSampleFormat> formats;
    if (!codec) { return formats; }

#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 19, 100)
    // Use new API for FFmpeg 7.x
    const enum AVSampleFormat *fmts = nullptr;
    int ret = avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, (const void **)&fmts, nullptr);
    if (ret < 0 || !fmts) { return formats; }

    const enum AVSampleFormat *p = fmts;
    while (*p != AV_SAMPLE_FMT_NONE) {
      formats.push_back(*p);
      p++;
    }
#else
    // Use older API for FFmpeg 6.x and earlier
    const enum AVSampleFormat *p = codec->sample_fmts;
    if (p) {
      while (*p != AV_SAMPLE_FMT_NONE) {
        formats.push_back(*p);
        p++;
      }
    }
#endif
    return formats;
  }

  std::vector<AVChannelLayout> AudioEncoderNode::getSupportedChannelLayouts() const {
    std::vector<AVChannelLayout> layouts;
    if (!codec) { return layouts; }

#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 19, 100)
    // Use new API for FFmpeg 7.x
    const AVChannelLayout *ch_layouts = nullptr;
    int ret = avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_CHANNEL_LAYOUT, 0, (const void **)&ch_layouts, nullptr);
    if (ret < 0 || !ch_layouts) { return layouts; }

    const AVChannelLayout *p = ch_layouts;
    while (p->nb_channels > 0) {
      layouts.push_back(*p);
      p++;
    }
#else
    // Use older API for FFmpeg 6.x and earlier
    const AVChannelLayout *p = codec->ch_layouts;
    if (p) {
      while (p->nb_channels > 0) {
        layouts.push_back(*p);
        p++;
      }
    }
#endif
    return layouts;
  }

  std::vector<int> AudioEncoderNode::getSupportedSampleRates() const {
    std::vector<int> rates;
    if (!codec) { return rates; }

#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 19, 100)
    // Use new API for FFmpeg 7.x
    const int *samplerates = nullptr;
    int ret = avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_SAMPLE_RATE, 0, (const void **)&samplerates, nullptr);
    if (ret < 0 || !samplerates) { return rates; }

    const int *p = samplerates;
    while (*p != 0) {
      rates.push_back(*p);
      p++;
    }
#else
    // Use older API for FFmpeg 6.x and earlier
    const int *p = codec->supported_samplerates;
    if (p) {
      while (*p != 0) {
        rates.push_back(*p);
        p++;
      }
    }
#endif
    return rates;
  }

  uint64_t AudioEncoderNode::getCurrentBitrate() const {
    return codecCtx ? codecCtx->bit_rate : bitrate;
  }

  int AudioEncoderNode::getExpectedFrameSize() const {
    if (!codecCtx) { return 0; }

    // Use codec's frame_size if specified
    if (codecCtx->frame_size > 0) { return codecCtx->frame_size; }

    // Calculate default frame size (20ms worth of samples)
    return codecCtx->sample_rate / 50;
  }

} // namespace FFmpeg

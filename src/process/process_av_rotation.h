#pragma once
#include "process_av_cuda.h"

#include <mist/orientation.h>
#include <mist/util.h>

#include <string>
extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
}

namespace Mist {
  class VideoTransform {
      AVFilterGraph *graph = 0;
      AVFilterContext *source = 0, *sink = 0;
      AVFrame *result = av_frame_alloc();
      std::string signature;
      const void *hardware = 0;
      int inputWidth = 0, inputHeight = 0, inputFormat = -1;

      bool build(const AVFrame *input, const std::string & chain) {
        avfilter_graph_free(&graph);
        source = sink = 0;
        graph = avfilter_graph_alloc();
        if (!graph) {
          error = AVERROR(ENOMEM);
          return 0;
        }
        graph->nb_threads = 1;
        graph->scale_sws_opts = av_strdup("flags=bilinear");
        const AVRational aspect =
          input->sample_aspect_ratio.num && input->sample_aspect_ratio.den ? input->sample_aspect_ratio : AVRational{1, 1};
        // Hardware frame context must be installed before the buffer filter is initialized.
        source = avfilter_graph_alloc_filter(graph, avfilter_get_by_name("buffer"), "source");
        if (!source) {
          error = AVERROR(ENOMEM);
          return 0;
        }
        AVBufferSrcParameters *params = av_buffersrc_parameters_alloc();
        if (!params) {
          error = AVERROR(ENOMEM);
          return 0;
        }
        params->format = input->format;
        params->width = input->width;
        params->height = input->height;
        params->time_base = AVRational{1, 1000};
        params->sample_aspect_ratio = aspect;
        params->hw_frames_ctx = input->hw_frames_ctx;
        error = av_buffersrc_parameters_set(source, params);
        av_free(params);
        if (error < 0) { return 0; }
        error = avfilter_init_str(source, 0);
        if (error < 0) { return 0; }
        error = avfilter_graph_create_filter(&sink, avfilter_get_by_name("buffersink"), "sink", 0, 0, graph);
        if (error < 0) { return 0; }
        AVFilterInOut *outputs = avfilter_inout_alloc(), *inputs = avfilter_inout_alloc();
        if (!outputs || !inputs) {
          avfilter_inout_free(&outputs);
          avfilter_inout_free(&inputs);
          error = AVERROR(ENOMEM);
          return 0;
        }
        outputs->name = av_strdup("in");
        outputs->filter_ctx = source;
        outputs->pad_idx = 0;
        inputs->name = av_strdup("out");
        inputs->filter_ctx = sink;
        inputs->pad_idx = 0;
        error = avfilter_graph_parse_ptr(graph, chain.c_str(), &inputs, &outputs, 0);
        avfilter_inout_free(&inputs);
        avfilter_inout_free(&outputs);
        if (error < 0) { return 0; }
        error = avfilter_graph_config(graph, 0);
        if (error < 0) { return 0; }
        return true;
      }
      static std::string download(const AVHWFramesContext *pool) {
        const char *name = pool ? av_get_pix_fmt_name(pool->sw_format) : 0;
        return name ? std::string("hwdownload,format=") + name : "";
      }
      static void append(std::string & chain, const std::string & filter) {
        if (filter.empty()) { return; }
        if (!chain.empty()) { chain += ","; }
        chain += filter;
      }

    public:
      int error = 0;
      VideoTransform() = default;
      VideoTransform(const VideoTransform &) = delete;
      VideoTransform & operator=(const VideoTransform &) = delete;
      ~VideoTransform() {
        av_frame_free(&result);
        avfilter_graph_free(&graph);
      }
      void reset() {
        av_frame_unref(result);
        avfilter_graph_free(&graph);
        source = sink = 0;
        signature.clear();
        hardware = 0;
      }
      AVFrame *apply(const AVFrame *input, AVPixelFormat output, int width, int height, Orientation orientation) {
        if (!input || !result) {
          error = AVERROR(ENOMEM);
          return 0;
        }
        const AVHWFramesContext *hw = input->hw_frames_ctx ? (const AVHWFramesContext *)input->hw_frames_ctx->data : 0;
        const bool onDevice = av_pix_fmt_desc_get((AVPixelFormat)input->format) &&
          (av_pix_fmt_desc_get((AVPixelFormat)input->format)->flags & AV_PIX_FMT_FLAG_HWACCEL);
        const char *name = av_get_pix_fmt_name(output);
        if (!name || (onDevice && download(hw).empty())) {
          error = AVERROR(EINVAL);
          return 0;
        }
        std::string cpu;
        if (onDevice) { append(cpu, download(hw)); }
        append(cpu, orientation.filters("cpu"));
        append(cpu, "scale=w=" + std::to_string(width) + ":h=" + std::to_string(height) + ":flags=bilinear");
        append(cpu, std::string("format=") + name);

        std::string chain, backend = "cpu";
        if (input->format == AV_PIX_FMT_QSV && avfilter_get_by_name("vpp_qsv")) {
          backend = "qsv";
          chain = "vpp_qsv=w=" + std::to_string(orientation.rotation % 180 ? height : width) +
            ":h=" + std::to_string(orientation.rotation % 180 ? width : height) + ":async_depth=0";
          const std::string transform = orientation.filters("qsv");
          if (!transform.empty()) {
            std::string direction = transform.substr(transform.find("dir=") + 4);
            // CPU transpose flips vertically; QSV expresses the equivalent horizontal flip.
            if (direction == "clock_flip") { direction = "cclock_hflip"; }
            if (direction == "cclock_flip") { direction = "clock_hflip"; }
            chain += ":transpose=" + direction;
          }
        } else {
          const char *device = input->format == AV_PIX_FMT_CUDA ? "cuda"
            : input->format == AV_PIX_FMT_VAAPI                 ? "vaapi"
            : input->format == AV_PIX_FMT_VIDEOTOOLBOX          ? "vt"
                                                                : 0;
          if (device && avfilter_get_by_name((std::string("scale_") + device).c_str())) {
            backend = device;
            const std::string orient = orientation.filters(device);
            if (orient.empty() || avfilter_get_by_name((std::string("transpose_") + device).c_str())) {
              append(chain, orient);
              append(chain, std::string("scale_") + device + "=w=" + std::to_string(width) + ":h=" + std::to_string(height));
            }
          }
        }
        if (!chain.empty() && output != input->format) {
          append(chain, download(hw));
          append(chain, std::string("scale=flags=bilinear,format=") + name);
        }
        if (chain.empty()) {
          chain = cpu;
          backend = "cpu";
        }
        const void *frames = input->hw_frames_ctx ? input->hw_frames_ctx->data : 0;
        if (!graph || signature != chain || inputWidth != input->width || inputHeight != input->height ||
            inputFormat != input->format || hardware != frames) {
          reset();
          if (!build(input, chain)) {
            if (chain == cpu || output == input->format) { return 0; }
            WARN_MSG("Hardware video filters rejected the frame; trying system-memory filters");
            if (!build(input, cpu)) { return 0; }
            backend = "cpu";
          }
          INFO_MSG("Video transform backend=%s filters=%s", backend.c_str(), chain.c_str());
          signature = chain;
          hardware = frames;
          inputWidth = input->width;
          inputHeight = input->height;
          inputFormat = input->format;
        }
        av_frame_unref(result);
        error = av_buffersrc_add_frame_flags(source, const_cast<AVFrame *>(input), AV_BUFFERSRC_FLAG_KEEP_REF);
        if (error < 0) { return 0; }
        error = av_buffersink_get_frame(sink, result);
        if (error < 0) { return 0; }
        av_frame_remove_side_data(result, AV_FRAME_DATA_DISPLAYMATRIX);
        return result;
      }
  };
} // namespace Mist

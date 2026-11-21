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

    public:
      int error = 0;
      VideoTransform() = default;
      VideoTransform(const VideoTransform &) = delete;
      VideoTransform & operator=(const VideoTransform &) = delete;
      ~VideoTransform() {
        av_frame_free(&result);
        avfilter_graph_free(&graph);
      }
      AVFrame *apply(const AVFrame *input, AVPixelFormat output, int width, int height, Orientation orientation) {
        if (!input || !result) {
          error = AVERROR(ENOMEM);
          return 0;
        }
        const AVHWFramesContext *hw = input->hw_frames_ctx ? (const AVHWFramesContext *)input->hw_frames_ctx->data : 0;
        const bool vt = input->format == AV_PIX_FMT_VIDEOTOOLBOX;
        std::string chain;
        const auto append = [&chain](const std::string & filter) {
          if (!filter.empty()) {
            if (!chain.empty()) { chain += ","; }
            chain += filter;
          }
        };
        std::string download;
        if (!cudaDownloadFilters(input, download)) {
          error = AVERROR(EINVAL);
          return 0;
        }
        append(download);
        append(orientation.filters(vt ? "vt" : "cpu"));
        if (vt && output == input->format) {
          append(std::string("scale_vt") + "=w=" + std::to_string(width) + ":h=" + std::to_string(height));
        } else {
          if (vt) {
            if (!hw) {
              error = AVERROR(EINVAL);
              return 0;
            }
            append("hwdownload");
            append(std::string("format=") + av_get_pix_fmt_name(hw->sw_format));
          }
          // Orientation is applied at source resolution, before scale/format.
          append("scale=w=" + std::to_string(width) + ":h=" + std::to_string(height) + ":flags=bilinear");
          const char *name = av_get_pix_fmt_name(output);
          if (!name) {
            error = AVERROR(EINVAL);
            return 0;
          }
          append(std::string("format=") + name);
        }
        const void *frames = input->hw_frames_ctx ? input->hw_frames_ctx->data : 0;
        if (!graph || signature != chain || inputWidth != input->width || inputHeight != input->height ||
            inputFormat != input->format || hardware != frames) {
          signature.clear();
          hardware = 0;
          inputWidth = inputHeight = 0;
          inputFormat = -1;
          avfilter_graph_free(&graph);
          source = sink = 0;
          graph = avfilter_graph_alloc();
          if (!graph) {
            error = AVERROR(ENOMEM);
            return 0;
          }
          graph->nb_threads = 1;
          graph->scale_sws_opts = av_strdup("flags=bilinear");
          const AVRational aspect = input->sample_aspect_ratio.num && input->sample_aspect_ratio.den ? input->sample_aspect_ratio
                                                                                                     : AVRational{1, 1};
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
          INFO_MSG("Video transform backend=%s filters=%s", vt ? "vt" : "cpu", chain.c_str());
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
        // The upright pixels must not acquire a second display transform downstream.
        av_frame_remove_side_data(result, AV_FRAME_DATA_DISPLAYMATRIX);
        return result;
      }
  };
} // namespace Mist

#pragma once

#include <mist/dtsc.h>

namespace Mist {
  /// Describes the live track a VOD input feeds in realtime (target in live) as the source track
  /// (source in M) its header describes, including the bitrate the header measured over the whole
  /// file: the processing graph decides rendition retention before the live track measures one.
  inline void describeRealtimeTrack(DTSC::Meta & live, size_t target, const DTSC::Meta & M, size_t source) {
    live.setType(target, M.getType(source));
    live.setCodec(target, M.getCodec(source));
    live.setFpks(target, M.getFpks(source));
    live.setInit(target, M.getInit(source));
    live.setLang(target, M.getLang(source));
    live.setRate(target, M.getRate(source));
    live.setChannels(target, M.getChannels(source));
    live.setSize(target, M.getSize(source));
    live.setWidth(target, M.getWidth(source));
    live.setRotation(target, M.getRotation(source));
    live.setMirror(target, M.getMirror(source));
    live.setHeight(target, M.getHeight(source));
    live.setBps(target, M.getBps(source));
    live.setMaxBps(target, M.getMaxBps(source));
  }
} // namespace Mist

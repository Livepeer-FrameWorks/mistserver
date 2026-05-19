#include "processing_profile.h"

#include <algorithm>

namespace Mist {

  bool isProcessingStreamName(const std::string & streamName) {
    if (streamName == "processing") { return true; }
    // "processing+<hash>" wildcard variant
    return streamName.rfind("processing+", 0) == 0;
  }

  ProcessingProfile defaultProcessingProfile() {
    return {1, 1, PP_KIND_UNKNOWN, "default"};
  }

  // Per-kind default caps. Starting points; the runtime controller adapts
  // from here based on measured pressure.
  namespace {
    struct KindDefaults {
        uint64_t startSpeed;
        uint64_t maxSpeed;
        const char *name;
    };

    KindDefaults defaultsFor(ProcessingProfileKind k) {
      switch (k) {
        case PP_KIND_THUMBS: return {8, 24, "thumbs"};
        case PP_KIND_AUDIO: return {6, 16, "audio"};
        case PP_KIND_AV_SW_VIDEO: return {1, 4, "av-sw-video"};
        case PP_KIND_AV_HW_VIDEO: return {4, 12, "av-hw-video"};
        case PP_KIND_LIVEPEER: return {8, 24, "livepeer"};
        case PP_KIND_MIXED: // never used directly as a "kind"; falls through
        case PP_KIND_UNKNOWN:
        default: return {1, 1, "unknown"};
      }
    }

    bool equalsIgnoreCase(const std::string & a, const char *b) {
      size_t n = a.size();
      for (size_t i = 0; i < n; ++i) {
        if (!b[i]) { return false; }
        char ca = a[i];
        if (ca >= 'A' && ca <= 'Z') { ca = (char)(ca + 32); }
        char cb = b[i];
        if (cb >= 'A' && cb <= 'Z') { cb = (char)(cb + 32); }
        if (ca != cb) { return false; }
      }
      return b[n] == 0;
    }

    /// True if the codec name is an audio codec we recognize. Match list
    /// matches the real Foghorn AV process configs (opus, AAC, MP3, vorbis,
    /// FLAC, AC3, EAC3, PCM, ALAW, ULAW). Anything else (H264, AV1, HEVC,
    /// VP9, VP8, JPEG, raw pixel formats) is treated as video.
    bool isAudioCodec(const std::string & codec) {
      return equalsIgnoreCase(codec, "aac") || equalsIgnoreCase(codec, "opus") || equalsIgnoreCase(codec, "mp3") ||
        equalsIgnoreCase(codec, "vorbis") || equalsIgnoreCase(codec, "flac") || equalsIgnoreCase(codec, "ac3") ||
        equalsIgnoreCase(codec, "eac3") || equalsIgnoreCase(codec, "pcm") || equalsIgnoreCase(codec, "alaw") ||
        equalsIgnoreCase(codec, "ulaw");
    }

    /// Classify a single process entry from its static config. HW vs SW is
    /// not visible here (libav negotiates at runtime); we conservatively
    /// assume SW-video for AV-video. The proc later publishes its real kind
    /// via ProcState.negotiatedKind and the controller re-classifies.
    ///
    /// AV is audio if any of these hold (any one is sufficient):
    ///   - x-LSP-kind == "audio"
    ///   - track_select selects no video (contains "video=none")
    ///   - codec is a known audio codec name (AAC, opus, mp3, ...)
    /// Otherwise AV is video.
    ProcessingProfileKind classifyOne(const JSON::Value & p) {
      if (!p.isObject() || !p.isMember("process")) { return PP_KIND_UNKNOWN; }
      std::string proc = p["process"].asString();
      if (proc == "Thumbs") { return PP_KIND_THUMBS; }
      if (proc == "Livepeer") { return PP_KIND_LIVEPEER; }
      if (proc == "AV") {
        if (p.isMember("x-LSP-kind") && p["x-LSP-kind"].asString() == "audio") { return PP_KIND_AUDIO; }
        if (p.isMember("track_select") && p["track_select"].isString() &&
            p["track_select"].asStringRef().find("video=none") != std::string::npos) {
          return PP_KIND_AUDIO;
        }
        if (p.isMember("codec") && p["codec"].isString() && isAudioCodec(p["codec"].asString())) {
          return PP_KIND_AUDIO;
        }
        return PP_KIND_AV_SW_VIDEO;
      }
      return PP_KIND_UNKNOWN;
    }

    /// Combine N kinds into a profile by tightest-of-present-kinds:
    /// pick the smallest maxSpeed (and matching startSpeed) across all kinds
    /// seen. Label is the single kind's name if homogeneous, "mixed" if not.
    ProcessingProfile combineKinds(const std::vector<ProcessingProfileKind> & kinds) {
      uint64_t minMax = 0, minStart = 0;
      ProcessingProfileKind tightest = PP_KIND_UNKNOWN;
      bool seen[8] = {false};
      int distinct = 0;
      for (auto k : kinds) {
        if (k == PP_KIND_UNKNOWN || k == PP_KIND_MIXED) { continue; }
        KindDefaults d = defaultsFor(k);
        if (k < 8 && !seen[k]) {
          seen[k] = true;
          ++distinct;
        }
        if (!minMax || d.maxSpeed < minMax) {
          minMax = d.maxSpeed;
          minStart = d.startSpeed;
          tightest = k;
        }
      }
      if (tightest == PP_KIND_UNKNOWN) { return defaultProcessingProfile(); }
      if (minStart < 1) { minStart = 1; }
      if (minMax < minStart) { minMax = minStart; }
      if (distinct == 1) {
        KindDefaults d = defaultsFor(tightest);
        return {d.startSpeed, d.maxSpeed, tightest, d.name};
      }
      // Heterogeneous: keep the tightest kind's numeric caps; label "mixed".
      return {minStart, minMax, PP_KIND_MIXED, "mixed"};
    }
  } // namespace

  ProcessingProfile classifyProcessingProfile(const JSON::Value & processesArray) {
    if (!processesArray.isArray() || !processesArray.size()) { return defaultProcessingProfile(); }
    std::vector<ProcessingProfileKind> kinds;
    kinds.reserve(processesArray.size());
    jsonForEachConst (processesArray, it) {
      ProcessingProfileKind k = classifyOne(*it);
      if (k == PP_KIND_UNKNOWN) {
        // Unknown process kind in the mix. If Foghorn flagged it as
        // best-effort with "inconsequential":true, skip it for cap purposes
        // (its failure doesn't break the job). Otherwise we have no idea
        // what it can sustain. Tighten to 1x rather than let a peer kind's
        // generous cap take over and overrun an unobservable proc.
        bool inconsequential = it->isMember("inconsequential") && (*it)["inconsequential"].asBool();
        if (inconsequential) { continue; }
        return {1, 1, PP_KIND_UNKNOWN, "unknown-strict"};
      }
      kinds.push_back(k);
    }
    if (kinds.empty()) {
      // All entries were inconsequential unknowns -> no observable load.
      // Treat like an empty config (default 1x).
      return defaultProcessingProfile();
    }
    return combineKinds(kinds);
  }

  ProcessingProfile classifyFromNegotiatedKinds(const std::vector<ProcessingProfileKind> & kinds) {
    if (kinds.empty()) { return defaultProcessingProfile(); }
    return combineKinds(kinds);
  }
} // namespace Mist

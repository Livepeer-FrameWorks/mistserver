#include "hls_support.h"

#include "encode.h"
#include "stream.h"
#include "timing.h"

#include <cstdlib>
#include <iomanip>
#include <sstream>

namespace HLS {

  namespace {

    struct PartRange {
        uint64_t mediaStart;
        uint64_t mediaEnd;
    };

    std::string getParam(const std::map<std::string, std::string> & params, const std::string & name) {
      std::map<std::string, std::string>::const_iterator value = params.find(name);
      return value == params.end() ? "" : value->second;
    }

    void appendParam(std::stringstream & result, bool & hasQuery, const std::string & name, const std::string & value) {
      if (!value.size()) { return; }
      result << (hasQuery ? "&" : "?") << name << "=" << value;
      hasQuery = true;
    }

    // Rendition group prefix for video tracks with equal resolutions.
    const std::string groupIdPrefix = "vid-";

    // Requests beyond the Advance Part Limit are rejected before blocking.
    uint32_t advancePartLimit(uint32_t partTargetMs) {
      if (!partTargetMs) { return 3; }
      // RFC 8216bis 6.2.5.2 defines this as three divided by PART-TARGET when
      // PART-TARGET is below one second, or three otherwise. A requested part
      // index is integral, so integer division implements the "exceeds" test:
      // at 500 ms the last advertised part may be advanced by six, not three.
      return partTargetMs < 1000 ? 3000 / partTargetMs : 3;
    }

#ifdef NOLLHLS
    const bool lowLatencySupported = false;
#else
    const bool lowLatencySupported = true;
#endif

    /// Calculate HLS media playlist version compatibility
    /// \return version number
    uint16_t playlistVersion(const std::string & hlsSkip) {
      // Server and Client support skipping media segments
      if (lowLatencySupported && hlsSkip == "YES") { return 9; }
      // Default, lowest version supported
      return 6;
    }

    /// returns the main track id provided in master manifest if valid
    /// else returns the current valid main track id
    size_t selectTimingTrack(const DTSC::Meta & M, const std::string & mTrack, const size_t mSelTrack) {
      return (mTrack.size() && (M.getValidTracks().count(atoll(mTrack.c_str())))) ? atoll(mTrack.c_str()) : mSelTrack;
    }

    bool getPartRange(const DTSC::Meta & M, size_t track, uint64_t startTime, uint64_t endTime,
                      PartRange & range) {
      range = PartRange();
      if (endTime <= startTime || !M.getValidTracks().count(track)) { return false; }

      DTSC::Parts parts(M.parts(track));
      DTSC::Keys keys(M.getKeys(track));
      if (!parts.getValidCount() || !keys.getValidCount()) { return false; }

      keys.applyLimiter(startTime, endTime, parts);
      if (!keys.getValidCount()) { return false; }

      const size_t firstKey = keys.getFirstValid();
      const size_t lastKey = keys.getEndValid() - 1;
      const size_t firstPart = keys.getFirstPart(firstKey);
      const size_t endPart = keys.getFirstPart(lastKey) + keys.getParts(lastKey);
      if (firstPart < parts.getFirstValid() || firstPart >= endPart ||
          endPart > parts.getEndValid()) {
        return false;
      }

      range.mediaStart = keys.getTime(firstKey);
      range.mediaEnd = keys.getTime(lastKey) + keys.getDuration(lastKey);
      for (size_t part = firstPart; part < endPart; ++part) {
        if (parts.getSize(part)) { return true; }
      }
      return false;
    }

    bool hasMediaPayload(const DTSC::Meta & M, bool isTs, size_t track, uint64_t startTime,
                         uint64_t endTime) {
      if (isTs) { return true; }
      PartRange range;
      return getPartRange(M, track, startTime, endTime, range);
    }

    bool hasFragmentPayload(const DTSC::Meta & M, bool isTs, bool isLive, size_t requestTrack,
                            size_t timingTrack, const DTSC::Fragments & fragments,
                            const DTSC::Keys & keys, uint64_t fragment) {
      if (fragment < fragments.getFirstValid() || fragment >= fragments.getEndValid()) { return false; }
      const uint64_t duration = fragments.getDuration(fragment);
      if (!duration) { return false; }
      uint64_t startTime = keys.getTime(fragments.getFirstKey(fragment));
      if (!isLive) {
        startTime -= M.getFirstms(timingTrack);
        startTime += M.getFirstms(requestTrack);
      }
      return hasMediaPayload(M, isTs, requestTrack, startTime, startTime + duration);
    }

    /// Return live edge fragment duration
    uint64_t liveFragmentDuration(const DTSC::Meta & M, size_t requestTrack, size_t timingTrack,
                                  uint64_t requestedMsn,
                                  const DTSC::Fragments & fragments, const DTSC::Keys & keys) {
      const uint64_t liveEdge = std::min(M.getLastms(requestTrack), M.getLastms(timingTrack));
      const uint64_t fragmentStart = keys.getTime(fragments.getFirstKey(requestedMsn));
      return liveEdge > fragmentStart ? liveEdge - fragmentStart : 0;
    }

    /// Waits until the requested fragment & partial fragment are available
    /// Returns 400 if specific part is requested without a specific MSN
    /// Returns 400 if requested MSN > the real live edge MSN plus two
    /// Returns 503 if the blocking reload exceeds its availability timeout.
    uint32_t blockPlaylistReload(const DTSC::Meta & M, bool lowLatencyDisabled,
                                 size_t requestTrack, size_t timingTrack,
                                 uint32_t targetDuration, uint32_t partTarget,
                                 uint32_t advertisedPartTarget, const std::string & hlsMsn,
                                 const std::string & hlsPart, const DTSC::Fragments & fragments,
                                 const DTSC::Keys & keys) {
      // Standard HLS playlists do not use blocking reload.
      if (lowLatencyDisabled) { return 0; }

      // Check BPR request validity
      if (hlsMsn.empty() && hlsPart.size()) { return 400; }
      if (fragments.getEndValid() && atol(hlsMsn.c_str()) > fragments.getEndValid() + 1) { return 400; }

      // BPR logic only if live & _HLS_msn requested
      if (M.getLive() && hlsMsn.size()) {
        DEBUG_MSG(5, "Requesting media playlist: Track %zu, MSN %s, part: %s", timingTrack,
                  hlsMsn.c_str(), hlsPart.c_str());

        uint64_t requestedMsn = atol(hlsMsn.c_str());
        uint64_t requestedPart = atol(hlsPart.c_str()) + 1; // base 1
        int64_t bprTimeLimit = (4ll * targetDuration * 1000) +
          std::max(M.getMinKeepAway(timingTrack), M.getMinKeepAway(requestTrack));

        // if hlsPart empty (HLS spec) OR if fragment hlsMsn is complete
        // THEN request part 1 of MSN++
        if (hlsPart.empty()) { requestedPart = 1; }
        if (requestedMsn < fragments.getFirstValid()) { return 0; }
        while (requestedMsn >= fragments.getEndValid()) {
          if (bprTimeLimit < 1) { return 503; }
          Util::wait(partTarget + 25);
          bprTimeLimit -= (partTarget + 25);
        }
        if (fragments.getDuration(requestedMsn)) {
          requestedMsn++;
          requestedPart = 1;
        }
        while (requestedMsn >= fragments.getEndValid()) {
          if (bprTimeLimit < 1) { return 503; }
          Util::wait(partTarget + 25);
          bprTimeLimit -= (partTarget + 25);
        }

        uint64_t lastFragmentDur =
            liveFragmentDuration(M, requestTrack, timingTrack, requestedMsn, fragments, keys);
        std::ldiv_t res = std::ldiv(lastFragmentDur, partTarget);
        size_t finalMsn = fragments.getEndValid() > 1 ? fragments.getEndValid() - 2 : 0;
        DEBUG_MSG(5, "req MSN %" PRIu64 " fin MSN %zu, req Part %" PRIu64 " fin Part %ld", requestedMsn, finalMsn,
                  requestedPart, res.quot);

        // RFC 8216bis 6.2.5.2: if the requested part is beyond the last available part by more than
        // the Advance Part Limit, reject immediately (400) instead of blocking until timeout (503).
        const uint32_t advertisedTarget = advertisedPartTarget ? advertisedPartTarget : partTarget;
        if (requestedPart > (uint64_t)res.quot + advancePartLimit(advertisedTarget)) { return 400; }

        while (requestedPart > res.quot) {
          if (bprTimeLimit < 1) { return 503; }
          DEBUG_MSG(5, "Part Block: req %" PRIu64 " fin %ld", requestedPart, res.quot);
          Util::wait(partTarget - res.rem + 25);
          bprTimeLimit -= (partTarget - res.rem + 25);
          lastFragmentDur =
              liveFragmentDuration(M, requestTrack, timingTrack, requestedMsn, fragments, keys);
          res = std::ldiv(lastFragmentDur, partTarget);
        }
      }
      return 0;
    }

    uint64_t liveWindowStart(uint64_t firstFrag, uint64_t lastFrag, uint64_t listLimit, uint64_t requestedStart) {
      // lastFrag is one past the forming fragment. Only fragments before that forming fragment
      // produce complete EXTINF entries in a live playlist.
      if (lastFrag <= firstFrag + 1) { return firstFrag; }
      const uint64_t completeEnd = lastFrag - 1;
      const uint64_t completeCount = completeEnd - firstFrag;
      uint64_t result = firstFrag;

      // Preserve the historical two-fragment safety trim only when doing so leaves the Apple
      // authoring profile's minimum six complete segments available.
      if (completeCount >= 8) { result += 2; }

      if (listLimit) {
        const uint64_t effectiveLimit = std::max<uint64_t>(listLimit, 6);
        if (completeEnd - result > effectiveLimit) { result = completeEnd - effectiveLimit; }
      }

      // An explicit internal start request may move within the retained full window, but it may not
      // reduce a normal live playlist below six complete segments. Delta-update omission is handled
      // separately through EXT-X-SKIP.
      if (requestedStart) {
        const uint64_t latestStart = completeEnd - std::min<uint64_t>(completeCount, 6);
        result = std::min(std::max(requestedStart, result), latestStart);
      }
      return result;
    }

    uint32_t skipBoundary(uint32_t targetDuration) {
      return targetDuration * 6;
    }

    void writeMediaHeader(std::stringstream & result, const DTSC::Meta & M,
                          uint64_t mediaSequence, uint64_t skippedFragments,
                          bool lowLatencyDisabled,
                          const std::string & mediaFormat, const std::string & urlPrefix,
                          const std::string & sessionId, size_t requestTrack,
                          uint32_t targetDuration, uint32_t partTarget,
                          uint32_t advertisedPartTarget, const std::string & hlsSkip) {
      uint16_t version = playlistVersion(hlsSkip);
      if (M.getLive() && !lowLatencyDisabled && lowLatencySupported) {
        version = std::max<uint16_t>(version, 10);
      }
      result << "#EXTM3U\r\n#EXT-X-VERSION:" << version << "\r\n";

      if (M.getLive() && !lowLatencyDisabled && lowLatencySupported) {
        const uint32_t advertisedTarget = advertisedPartTarget ? advertisedPartTarget : partTarget;
        const float partTargetSeconds = advertisedTarget / 1000.0;
        result << "#EXT-X-SERVER-CONTROL:CAN-BLOCK-RELOAD=YES,CAN-SKIP-UNTIL="
               << skipBoundary(targetDuration) << ",HOLD-BACK=" << targetDuration * 3
               << ",PART-HOLD-BACK=" << partTargetSeconds * 4
               << "\r\n#EXT-X-PART-INF:PART-TARGET=" << partTargetSeconds << "\r\n";
      }

      if (mediaFormat != ".ts") {
        result << "#EXT-X-MAP:URI=\"" << urlPrefix << "init" << mediaFormat;
        if (sessionId.size()) { result << "?tkn=" << sessionId; }
        result << "\"\r\n";
      }
      const std::string encryptionMethod = M.getEncryption(requestTrack);
      if (encryptionMethod.size()) {
        const JSON::Value encryption = JSON::fromString(encryptionMethod);
        if (encryption.isArray() && encryption.size()) {
          std::set<std::string> tags;
          jsonForEachConst (encryption, entry) {
            if ((*entry)["hls-media"].asStringRef().size()) {
              tags.insert(Encodings::Base64::decode((*entry)["hls-media"].asStringRef()));
            }
          }
          for (std::set<std::string>::const_iterator tag = tags.begin(); tag != tags.end(); ++tag) {
            result << *tag << "\r\n";
          }
        } else {
          // Compatibility with the legacy simple encryption-method metadata.
          result << "#EXT-X-KEY:METHOD=" << encryptionMethod << ",URI=\"asd\"\r\n";
        }
      }
      result << "#EXT-X-TARGETDURATION:" << targetDuration << "\r\n"
             << "#EXT-X-MEDIA-SEQUENCE:" << mediaSequence << "\r\n";

      // EXT-X-SKIP must follow EXT-X-MEDIA-SEQUENCE.
      if (skippedFragments) {
        result << "#EXT-X-SKIP:SKIPPED-SEGMENTS=" << skippedFragments << "\r\n";
      }
    }

    void writeSegment(std::stringstream & result, const DTSC::Meta & M,
                      const std::string & mediaFormat, const std::string & urlPrefix,
                      const std::string & sessionId, size_t timingTrack, uint64_t fragment,
                      uint64_t startTime, uint64_t duration) {
      result << "#EXTINF:" << std::fixed << std::setprecision(3) << duration / 1000.0
             << ",\r\n";

      // NOTE: HLS spec says it isn't mandatory to add date time tag for every fragment.
      // Tests show that there is definitely an influence on consistency for live streams.
      // Printing the tag for every fragment tag was the best.
      if (M.getLive()) {
        const uint64_t unixMs = M.packetTimeToUnixMs(startTime);
        if (unixMs) { result << "#EXT-X-PROGRAM-DATE-TIME:" << Util::getUTCStringMillis(unixMs) << "\r\n"; }
      }

      result << urlPrefix << "chunk_" << startTime << mediaFormat;
      result << "?msn=" << fragment;
      result << "&mTrack=" << timingTrack;
      result << "&dur=" << duration;
      if (sessionId.size()) { result << "&tkn=" << sessionId; }
      result << "\r\n";
    }

    bool writePart(std::stringstream & result, const DTSC::Meta & M, bool muxed,
                   const std::string & mediaFormat, const std::string & urlPrefix,
                   const std::string & sessionId, size_t requestTrack, size_t timingTrack,
                   uint32_t partTarget, uint64_t fragment, uint64_t fragmentStart,
                   uint32_t partNumber, uint64_t rangeStart, uint64_t rangeEnd) {
      PartRange range;
      if (!getPartRange(M, requestTrack, rangeStart, rangeEnd, range)) { return false; }

      // A muxed part is defined by the shared grid interval: another selected track can have a
      // sample before the primary track's first sample. Per-track playlists retain their historical
      // snap-to-first-sample duration.
      const uint64_t duration = muxed ? rangeEnd - rangeStart : range.mediaEnd - range.mediaStart;
      const uint64_t requestDuration = rangeEnd - rangeStart;
      result << "#EXT-X-PART:DURATION=" << duration / 1000.0;
      result << ",URI=\"" << urlPrefix;
      result << "chunk_" << fragmentStart << "." << partNumber << mediaFormat;
      result << "?msn=" << fragment;
      result << "&mTrack=" << timingTrack;
      // Keep the resource identity tied to the production grid. This guarantees
      // that an EXT-X-PRELOAD-HINT URI remains identical when the part is later
      // advertised with its exact sample-aligned duration.
      result << "&dur=" << requestDuration;
      if (sessionId.size()) { result << "&tkn=" << sessionId; }
      result << "\"";

      // NOTE: INDEPENDENT tags, specified ONLY for VIDEO tracks, indicate the first partial fragment
      // closest to the before (live edge - PART-HOLD-BACK) time that a client starts playback from.
      if (M.getType(requestTrack) == "video") {
        const uint64_t partStartTime = fragmentStart + uint64_t(partNumber) * partTarget;
        const uint32_t partKeyIdx = M.getKeyIndexForTime(timingTrack, partStartTime);
        const uint64_t partKeyIdxTime = M.getTimeForKeyIndex(timingTrack, partKeyIdx);
        if (partKeyIdxTime == partStartTime) { result << ",INDEPENDENT=YES"; }
      }
      result << "\r\n";
      return true;
    }

    uint32_t writeParts(std::stringstream & result, const DTSC::Meta & M,
                        bool lowLatencyDisabled, bool muxed,
                        const std::string & mediaFormat, const std::string & urlPrefix,
                        const std::string & sessionId, size_t requestTrack, size_t timingTrack,
                        uint32_t targetDuration, uint32_t partTarget,
                        uint64_t endFragment, uint64_t liveEdge, uint64_t fragment,
                        uint64_t startTime, uint64_t duration) {
      if (lowLatencyDisabled || !M.getLive() || !lowLatencySupported) {
        return 0;
      }

      // if fragment is last-but-4th or later
      // OR if fragment is 3 target durations from the end
      const uint64_t liveEdgeDistance = liveEdge > startTime ? liveEdge - startTime : 0;
      uint64_t availableDuration = duration;
      if (fragment == endFragment - 1) {
        availableDuration = std::min(availableDuration, liveEdgeDistance);
      }

      uint32_t partNumber = 0;
      if ((endFragment - fragment < 5) || (liveEdgeDistance <= 3ull * targetDuration * 1000)) {
        const std::ldiv_t durationData = std::ldiv(availableDuration, partTarget);

        // General case: complete cells in the configured part grid.
        for (; partNumber < durationData.quot; ++partNumber) {
          const uint64_t rangeStart = startTime + uint64_t(partNumber) * partTarget;
          const uint64_t rangeEnd = startTime + (uint64_t(partNumber) + 1) * partTarget;
          if (!writePart(result, M, muxed, mediaFormat, urlPrefix, sessionId, requestTrack,
                         timingTrack, partTarget, fragment, startTime, partNumber, rangeStart,
                         rangeEnd)) {
            break;
          }
        }

        // Special case: last partial segment (duration < partTargetMs) in any fragment not at
        // live edge
        if (durationData.rem && (endFragment - fragment > 1)) {
          const uint64_t rangeStart = startTime + uint64_t(partNumber) * partTarget;
          const uint64_t rangeEnd = rangeStart + durationData.rem;
          writePart(result, M, muxed, mediaFormat, urlPrefix, sessionId, requestTrack,
                    timingTrack, partTarget, fragment, startTime, partNumber, rangeStart, rangeEnd);
        }
      }
      return partNumber;
    }

    void writeMediaFooter(std::stringstream & result, const DTSC::Meta & M,
                          const std::map<size_t, Comms::Users> & userSelect,
                          bool lowLatencyDisabled, bool muxed,
                          const std::string & mediaFormat, const std::string & urlPrefix,
                          const std::string & sessionId, size_t requestTrack, size_t timingTrack,
                          uint32_t partTarget, uint64_t firstFragment, uint64_t endFragment,
                          uint64_t lastFragmentStart, uint32_t lastPartCount) {
      if (!M.getLive()) {
        result << "#EXT-X-ENDLIST\r\n";
        return;
      }
      if (lowLatencyDisabled || !lowLatencySupported) { return; }

      if (endFragment > firstFragment && endFragment) {
        result << "#EXT-X-PRELOAD-HINT:TYPE=PART,URI=\"" << urlPrefix << "chunk_"
               << lastFragmentStart << "." << lastPartCount << mediaFormat << "?msn="
               << endFragment - 1 << "&mTrack=" << timingTrack << "&dur=" << partTarget;
        if (sessionId.size()) { result << "&tkn=" << sessionId; }
        result << "\"\r\n";
      }

      if (muxed) { return; }
      DTSC::Fragments fragments(M.fragments(timingTrack));
      if (endFragment < fragments.getFirstValid() + 2) { return; }
      const std::ldiv_t previousPart =
          std::ldiv(fragments.getDuration(endFragment - 2), partTarget);
      for (std::map<size_t, Comms::Users>::const_iterator rendition = userSelect.begin(); rendition != userSelect.end(); ++rendition) {
        if (rendition->first == requestTrack) { continue; }
        result << "#EXT-X-RENDITION-REPORT:URI=\"" << rendition->first
               << "/index.m3u8?mTrack=" << timingTrack;
        if (sessionId.size()) { result << "&tkn=" << sessionId; }
        result << "\"";
        if (lastPartCount) {
          result << ",LAST-MSN=" << endFragment - 1 << ",LAST-PART=" << lastPartCount - 1
                 << "\r\n";
        } else {
          result << ",LAST-MSN=" << endFragment - 2
                 << ",LAST-PART=" << previousPart.quot - 1 + (previousPart.rem ? 1 : 0) << "\r\n";
        }
      }
    }

    std::string groupId(const DTSC::Meta & M, size_t track) {
      std::stringstream result;
      result << groupIdPrefix << M.getWidth(track) << "x" << M.getHeight(track);
      return result.str();
    }

    bool framesAligned(std::stringstream & result, const DTSC::Meta & M, size_t mainTrack,
                       size_t track) {
      if (track == mainTrack || M.keyTimingsMatch(mainTrack, track)) { return true; }
      result << "## NOTE: Track " << track << " is available, but ignored because it is not aligned with track "
             << mainTrack << ".\r\n";
      return false;
    }

    std::string trackPath(const DTSC::Meta & M, size_t track, bool typed) {
      if (typed) {
        if (M.getType(track) == "video") { return "v" + std::to_string(track); }
        if (M.getType(track) == "audio") { return "a" + std::to_string(track); }
      }
      return std::to_string(track);
    }

    void writeMediaRendition(std::stringstream & result, const DTSC::Meta & M,
                             size_t mainTrack, bool lowLatencyDisabled, bool typedPaths,
                             const std::string & sessionId, size_t track,
                             const std::string & type, const std::string & group,
                             bool defaultRendition = false, bool autoSelect = false) {
      const std::string lang = M.getLang(track).empty() ? "und" : M.getLang(track);
      result << "#EXT-X-MEDIA:TYPE=" << type << ",GROUP-ID=\"" << group << "\",LANGUAGE=\"" << lang;
      if (lang == "und") { result << "-" << track; }
      result << "\",NAME=\"" << M.getCodec(track) << "-" << (lang == "und" ? std::to_string(track) : lang)
             << "\"";
      if (defaultRendition || autoSelect) {
        result << ",DEFAULT=" << (defaultRendition ? "YES" : "NO")
               << ",AUTOSELECT=" << (autoSelect ? "YES" : "NO");
      }
      if (type == "AUDIO" && M.getChannels(track)) {
        result << ",CHANNELS=\"" << M.getChannels(track) << "\"";
      }
      result << ",URI=\"" << trackPath(M, track, typedPaths) << "/index.m3u8?mTrack=" << mainTrack;
      if (sessionId.size()) { result << "&tkn=" << sessionId; }
      if (lowLatencyDisabled) { result << "&llhls=0"; }
      result << "\"\r\n";
    }

    void writeVariantPath(std::stringstream & result, const DTSC::Meta & M, size_t mainTrack,
                          bool lowLatencyDisabled, bool isTs, const std::string & sessionId,
                          const std::set<size_t> & audioTracks, size_t track, bool isVideo) {
      result << trackPath(M, track, !isTs);
      if (isVideo && isTs && audioTracks.size() == 1) { result << "_" << *audioTracks.begin(); }
      result << "/index.m3u8?mTrack=" << mainTrack;
      if (sessionId.size()) { result << "&tkn=" << sessionId; }
      if (lowLatencyDisabled) { result << "&llhls=0"; }
      result << "\r\n";
    }

    void writeMasterEncryption(std::stringstream & result, const DTSC::Meta & M, const std::map<size_t, Comms::Users> & userSelect) {
      std::set<std::string> tags;
      for (std::map<size_t, Comms::Users>::const_iterator track = userSelect.begin(); track != userSelect.end(); ++track) {
        const JSON::Value encryption = JSON::fromString(M.getEncryption(track->first));
        jsonForEachConst (encryption, entry) {
          if (entry->isMember("hls-master")) { tags.insert((*entry)["hls-master"].asStringRef()); }
        }
      }
      for (std::set<std::string>::const_iterator tag = tags.begin(); tag != tags.end(); ++tag) {
        result << Encodings::Base64::decode(*tag) << "\r\n";
      }
    }

    void writeMasterPlaylist(std::stringstream & result, const DTSC::Meta & M,
                             const std::map<size_t, Comms::Users> & userSelect,
                             size_t mainTrack, bool lowLatencyDisabled, bool isTs,
                             const std::string & sessionId) {
      std::set<size_t> videoTracks;
      std::set<size_t> audioTracks;
      std::set<size_t> subtitleTracks;
      std::multimap<std::string, size_t> videoGroups;
      for (std::map<size_t, Comms::Users>::const_iterator track = userSelect.begin(); track != userSelect.end(); ++track) {
        if (M.getType(track->first) == "video") {
          videoTracks.insert(track->first);
          videoGroups.insert(std::make_pair(groupId(M, track->first), track->first));
        }
        if (M.getType(track->first) == "audio") { audioTracks.insert(track->first); }
        if (M.getCodec(track->first) == "subtitle") { subtitleTracks.insert(track->first); }
      }

      result << "#EXTM3U\r\n#EXT-X-VERSION:7\r\n#EXT-X-INDEPENDENT-SEGMENTS\r\n";
      writeMasterEncryption(result, M, userSelect);

      if (!audioTracks.size()) {
        for (std::set<size_t>::const_iterator track = videoTracks.begin(); track != videoTracks.end(); ++track) {
          const std::string group = groupId(M, *track);
          if (videoGroups.count(group) == 1) { continue; }
          if (framesAligned(result, M, mainTrack, *track)) {
            writeMediaRendition(result, M, mainTrack, lowLatencyDisabled, !isTs, sessionId, *track,
                                "VIDEO", group);
          }
        }
      }

      std::set<std::string> audioCodecs;
      uint64_t audioBandwidth = 0;
      if (videoTracks.size()) {
        for (std::set<size_t>::const_iterator track = audioTracks.begin(); track != audioTracks.end(); ++track) {
          if (!isTs || audioTracks.size() > 1) {
            writeMediaRendition(result, M, mainTrack, lowLatencyDisabled, !isTs, sessionId, *track,
                                "AUDIO", "aud", track == audioTracks.begin(), true);
          }
          audioCodecs.insert(Util::codecString(M.getCodec(*track), M.getInit(*track)));
          audioBandwidth = std::max(audioBandwidth, M.getBps(*track));
        }
      }

      uint64_t subtitleBandwidth = 0;
      for (std::set<size_t>::const_iterator track = subtitleTracks.begin(); track != subtitleTracks.end(); ++track) {
        writeMediaRendition(result, M, mainTrack, lowLatencyDisabled, !isTs, sessionId, *track,
                            "SUBTITLES", "sub");
        subtitleBandwidth = std::max(subtitleBandwidth, M.getBps(*track));
      }

      if (!videoTracks.size()) {
        for (std::set<size_t>::const_iterator track = audioTracks.begin(); track != audioTracks.end(); ++track) {
          const uint64_t bandwidth = std::max<uint64_t>(M.getBps(*track), 5) * 8;
          result << "#EXT-X-STREAM-INF:CODECS=\"" << Util::codecString(M.getCodec(*track), M.getInit(*track)) << "\""
                 << std::fixed << std::setprecision(0) << ",BANDWIDTH=" << bandwidth * 1.3
                 << ",AVERAGE-BANDWIDTH=" << bandwidth * 1.1 << "\r\n";
          writeVariantPath(result, M, mainTrack, lowLatencyDisabled, isTs, sessionId, audioTracks,
                           *track, false);
        }
        return;
      }

      std::string audioCodecList;
      for (std::set<std::string>::const_iterator codec = audioCodecs.begin(); codec != audioCodecs.end(); ++codec) {
        audioCodecList += "," + *codec;
      }
      std::string associatedGroups;
      if ((!isTs && audioTracks.size()) || (isTs && audioTracks.size() > 1)) {
        associatedGroups += "AUDIO=\"aud\",";
      }
      if (subtitleTracks.size()) { associatedGroups += "SUBTITLES=\"sub\","; }

      for (std::set<size_t>::const_iterator track = videoTracks.begin(); track != videoTracks.end(); ++track) {
        if (!framesAligned(result, M, mainTrack, *track)) { continue; }
        const uint64_t bandwidth = (std::max<uint64_t>(M.getBps(*track), 5) + audioBandwidth + subtitleBandwidth) * 8;
        result << "#EXT-X-STREAM-INF:" << associatedGroups << "CODECS=\""
               << Util::codecString(M.getCodec(*track), M.getInit(*track)) << audioCodecList
               << "\",RESOLUTION=" << M.getWidth(*track) << "x" << M.getHeight(*track);
        if (M.getFpks(*track)) { result << ",FRAME-RATE=" << (float)M.getFpks(*track) / 1000; }
        result << std::fixed << std::setprecision(0) << ",BANDWIDTH=" << bandwidth * 1.3
               << ",AVERAGE-BANDWIDTH=" << bandwidth * 1.1 << "\r\n";
        writeVariantPath(result, M, mainTrack, lowLatencyDisabled, isTs, sessionId, audioTracks,
                         *track, true);
      }
    }

  } // namespace

  Generator::Generator() : ext(".ts"), listLimit(0), partTargetMs(500), muxed(false) {}

  void Generator::setParam(const std::string & name, const std::string & value) {
    params[name] = value;
  }

  void Generator::setExt(const std::string & value) {
    if (!value.size()) {
      ext = ".ts";
    } else {
      ext = value[0] == '.' ? value : "." + value;
    }
  }

  void Generator::setListLimit(uint64_t value) {
    listLimit = value;
  }

  void Generator::setPartTarget(uint32_t value) {
    if (value) { partTargetMs = value; }
  }

  void Generator::setUrlPrefix(const std::string & value) {
    urlPrefix = value;
  }

  void Generator::setMuxed(bool value) {
    muxed = value;
  }

  void Generator::setMediaPath(const std::string & value) {
    mediaPath = value;
  }

  std::string Generator::masterPlaylist(const DTSC::Meta & M, const std::map<size_t, Comms::Users> & userSelect, size_t mainTrack) const {
    if (muxed) {
      std::stringstream result;
      result << "#EXTM3U\r\n#EXT-X-VERSION:7\r\n#EXT-X-INDEPENDENT-SEGMENTS\r\n";
      writeMasterEncryption(result, M, userSelect);
      result << "#EXT-X-STREAM-INF:";
      uint64_t bandwidth = 0;
      std::string codecs;
      size_t video = INVALID_TRACK_ID;
      for (std::map<size_t, Comms::Users>::const_iterator track = userSelect.begin(); track != userSelect.end(); ++track) {
        bandwidth += M.getBps(track->first) * 8;
        if (codecs.size()) { codecs += ","; }
        codecs += Util::codecString(M.getCodec(track->first), M.getInit(track->first));
        if (M.getType(track->first) == "video") { video = track->first; }
      }
      result << "BANDWIDTH=" << (uint64_t)(bandwidth * 1.3) << ",AVERAGE-BANDWIDTH=" << (uint64_t)(bandwidth * 1.1)
             << ",CODECS=\"" << codecs << "\"";
      if (video != INVALID_TRACK_ID) {
        result << ",RESOLUTION=" << M.getWidth(video) << "x" << M.getHeight(video);
        if (M.getFpks(video)) { result << ",FRAME-RATE=" << M.getFpks(video) / 1000.0; }
      }
      result << "\r\n" << mediaPath << "/index.m3u8";
      bool hasQuery = false;
      appendParam(result, hasQuery, "tkn", getParam(params, "tkn"));
      if (getParam(params, "llhls") == "0") { appendParam(result, hasQuery, "llhls", "0"); }
      result << "\r\n";
      return result.str();
    }

    std::stringstream result;
    writeMasterPlaylist(result, M, userSelect, mainTrack, getParam(params, "llhls") == "0",
                        ext == ".ts", getParam(params, "tkn"));
    return result.str();
  }

  Playlist Generator::mediaPlaylist(const DTSC::Meta & M, const std::map<size_t, Comms::Users> & userSelect,
                                    size_t requestTrack, size_t mainTrack) const {
    if (!M.getValidTracks().count(requestTrack)) { return Playlist(404); }

    const size_t timingTrack = selectTimingTrack(M, getParam(params, "mTrack"), mainTrack);
    const bool isTs = ext == ".ts";
    const bool isLive = M.getLive();
    const std::string sessionId = getParam(params, "tkn");
    const std::string hlsSkip = getParam(params, "_HLS_skip");
    const std::string hlsMsn = getParam(params, "_HLS_msn");
    const std::string hlsPart = getParam(params, "_HLS_part");
    bool lowLatencyDisabled = getParam(params, "llhls") == "0";
    uint32_t targetDuration = (M.biggestFragment(timingTrack) + 500) / 1000;
    if (!targetDuration) { targetDuration = 1; }

    uint64_t maxSampleDuration = 0;
    for (std::map<size_t, Comms::Users>::const_iterator selected = userSelect.begin(); selected != userSelect.end(); ++selected) {
      if (!M.getValidTracks().count(selected->first)) { continue; }
      DTSC::Parts parts(M.parts(selected->first));
      for (size_t part = parts.getFirstValid(); part < parts.getEndValid(); ++part) {
        maxSampleDuration = std::max<uint64_t>(maxSampleDuration, parts.getDuration(part));
      }
    }
    const uint64_t advertised64 = uint64_t(partTargetMs) + maxSampleDuration;
    const uint32_t advertisedPartTarget = advertised64 > UINT32_MAX ? UINT32_MAX : (uint32_t)advertised64;
    if (maxSampleDuration && 3ull * partTargetMs < 37ull * maxSampleDuration) {
      lowLatencyDisabled = true;
      DEBUG_MSG(5, "Disabling LL-HLS for track %zu: %ums part grid is too short for %" PRIu64 "ms media samples",
                requestTrack, partTargetMs, maxSampleDuration);
    }

    DTSC::Fragments fragments(M.fragments(timingTrack));
    DTSC::Keys keys(M.getKeys(timingTrack));
    const uint32_t responseCode =
        blockPlaylistReload(M, lowLatencyDisabled, requestTrack, timingTrack, targetDuration,
                            partTargetMs, advertisedPartTarget, hlsMsn, hlsPart, fragments, keys);
    if (responseCode) { return Playlist(responseCode); }

    const uint64_t availableFirstFragment = fragments.getFirstValid();
    const uint64_t liveEdge = std::min(M.getLastms(requestTrack), M.getLastms(timingTrack));
    uint64_t endFragment = fragments.getEndValid();
    if (isLive) {
      const uint64_t edgeFragment = M.getFragmentIndexForTime(timingTrack, liveEdge);
      endFragment = std::min<uint64_t>(endFragment, edgeFragment + 1);
      if (endFragment < availableFirstFragment) { endFragment = availableFirstFragment; }
    }

    uint64_t firstFragment = availableFirstFragment;
    if (isLive) {
      firstFragment = liveWindowStart(availableFirstFragment, endFragment, listLimit,
                                      (uint64_t)atol(getParam(params, "iMsn").c_str()));

      // Retain six complete, servable CMAF segments when that many still have payload data.
      if (!isTs && endFragment > availableFirstFragment + 1) {
        uint64_t candidate = endFragment - 1;
        uint32_t servable = 0;
        while (candidate > availableFirstFragment && servable < 6) {
          --candidate;
          if (hasFragmentPayload(M, false, true, requestTrack, timingTrack, fragments, keys,
                                 candidate)) {
            ++servable;
          }
        }
        const uint64_t floorStart =
            servable >= 6 ? candidate : availableFirstFragment;
        if (firstFragment > floorStart) { firstFragment = floorStart; }
      }
    }
    while (!isTs && firstFragment + 1 < endFragment &&
           !hasFragmentPayload(M, false, isLive, requestTrack, timingTrack, fragments, keys,
                               firstFragment)) {
      ++firstFragment;
    }

    const uint64_t mediaSequence = firstFragment;
    uint64_t skippedFragments = 0;
    if (playlistVersion(hlsSkip) >= 9) {
      const uint32_t retained = skipBoundary(targetDuration) / targetDuration + 2;
      const uint64_t available = endFragment - firstFragment;
      if (available > retained) {
        skippedFragments = available - retained;
        firstFragment += skippedFragments;
      }
    }

    std::stringstream result;
    writeMediaHeader(result, M, mediaSequence, skippedFragments, lowLatencyDisabled, ext,
                     urlPrefix, sessionId, requestTrack, targetDuration, partTargetMs,
                     advertisedPartTarget, hlsSkip);

    uint64_t lastFragmentStart = liveEdge;
    uint32_t lastPartCount = 0;
    for (uint64_t fragment = firstFragment; fragment < endFragment; ++fragment) {
      uint64_t startTime = keys.getTime(fragments.getFirstKey(fragment));
      if (!isLive) { startTime -= M.getFirstms(timingTrack); }

      uint64_t duration = fragments.getDuration(fragment);
      if (!duration) {
        duration = liveEdge > startTime ? liveEdge - startTime : 0;
      }
      lastFragmentStart = startTime;
      lastPartCount =
          writeParts(result, M, lowLatencyDisabled, muxed, ext, urlPrefix, sessionId,
                     requestTrack, timingTrack, targetDuration, partTargetMs, endFragment,
                     liveEdge, fragment, startTime, duration);

      // The final live fragment is still forming and has no EXTINF entry yet.
      if (isLive && fragment == endFragment - 1) { continue; }

      uint64_t payloadStart = startTime;
      if (!isLive) { payloadStart += M.getFirstms(requestTrack); }
      if (!hasMediaPayload(M, isTs, requestTrack, payloadStart, payloadStart + duration)) {
        continue;
      }
      writeSegment(result, M, ext, urlPrefix, sessionId, timingTrack, fragment, startTime,
                   duration);
    }

    writeMediaFooter(result, M, userSelect, lowLatencyDisabled, muxed, ext, urlPrefix,
                     sessionId, requestTrack, timingTrack, partTargetMs,
                     availableFirstFragment, endFragment, lastFragmentStart, lastPartCount);
    return Playlist(200, result.str());
  }

} // namespace HLS

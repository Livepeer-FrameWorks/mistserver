#pragma once

#include "comms.h"
#include "dtsc.h"

namespace HLS {

  /// Generated playlist data and the HTTP status that should accompany it.
  struct Playlist {
      Playlist(uint16_t responseCode = 200, const std::string & playlist = "") : code(responseCode), data(playlist) {}

      uint16_t code;
      std::string data;
  };

  class Generator {
    public:
      Generator();

      /// Adds a request parameter forwarded into generated playlist URLs or LL-HLS handling.
      void setParam(const std::string & name, const std::string & value);
      /// Sets the media object extension (for example .ts or .m4s).
      void setExt(const std::string & value);
      /// Limits the number of complete live segments while retaining the HLS safety floor.
      void setListLimit(uint64_t value);
      /// Sets the stable LL-HLS part production grid in milliseconds.
      void setPartTarget(uint32_t value);
      /// Sets the optional prefix used for media object URLs.
      void setUrlPrefix(const std::string & value);
      /// Enables a single media playlist containing all selected audio/video tracks.
      void setMuxed(bool value);
      /// Sets the muxed media path advertised by the master playlist.
      void setMediaPath(const std::string & value);

      /// Generates an HLS master playlist for the selected tracks.
      std::string masterPlaylist(const DTSC::Meta & M, const std::map<size_t, Comms::Users> & userSelect, size_t mainTrack) const;
      /// Generates an HLS media playlist for requestTrack, timed against mainTrack by default.
      Playlist mediaPlaylist(const DTSC::Meta & M, const std::map<size_t, Comms::Users> & userSelect,
                             size_t requestTrack, size_t mainTrack) const;

    protected:
      std::map<std::string, std::string> params;
      std::string ext; ///< File extension for media objects.
      std::string urlPrefix;
      std::string mediaPath;
      uint64_t listLimit;
      uint32_t partTargetMs;
      bool muxed;
  };

} // namespace HLS

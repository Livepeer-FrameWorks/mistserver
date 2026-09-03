#pragma once

#include <string>

namespace Mist {
  namespace ThumbnailArtifacts {
    struct Paths {
        std::string poster;
        std::string sprite;
        std::string manifest;
    };

    /// Publishes one complete thumbnail generation. Each visible file is
    /// replaced atomically, and no file is touched when staging or target
    /// validation fails.
    bool publish(const std::string & directory, const std::string & posterData, const std::string & spriteData,
                 const std::string & manifestData, Paths & paths, std::string & error);
  } // namespace ThumbnailArtifacts
} // namespace Mist

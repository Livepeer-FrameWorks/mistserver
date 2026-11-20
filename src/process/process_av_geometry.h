#pragma once
#include <mist/json.h>

#include <cstdint>
#include <cstdlib>

namespace Mist {
  inline uint64_t evenDimension(uint64_t value) {
    if (value > 2 && (value % 2)) { --value; }
    return value;
  }

  inline void applyRequestedResolution(const JSON::Value & opt, uint64_t srcWidth, uint64_t srcHeight,
                                       uint64_t & reqWidth, uint64_t & reqHeight) {
    if (!opt.isMember("resolution") || !opt["resolution"]) { return; }
    std::string res = opt["resolution"].asString();
    size_t xPos = res.find("x");
    if (xPos == std::string::npos) { return; }
    uint64_t parsedWidth = strtoull(res.substr(0, xPos).c_str(), NULL, 0);
    uint64_t parsedHeight = strtoull(res.substr(xPos + 1).c_str(), NULL, 0);
    if (!parsedWidth && !parsedHeight) { return; }
    if (!parsedWidth && parsedHeight && srcWidth && srcHeight) {
      if (srcWidth < srcHeight) {
        parsedWidth = parsedHeight;
        parsedHeight = srcHeight * parsedWidth / srcWidth;
      } else {
        parsedWidth = srcWidth * parsedHeight / srcHeight;
      }
    }
    if (parsedWidth && !parsedHeight && srcWidth && srcHeight) { parsedHeight = srcHeight * parsedWidth / srcWidth; }
    if (parsedWidth && parsedHeight) {
      reqWidth = evenDimension(parsedWidth);
      reqHeight = evenDimension(parsedHeight);
    }
  }

} // namespace Mist

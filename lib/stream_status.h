#pragma once

#include "defines.h"

#include <cstdint>

namespace Util {
  inline bool streamStatusIsTerminal(uint8_t status) {
    return status == STRMSTAT_OFF || status == STRMSTAT_OFFLINE;
  }

  inline const char *streamStatusDescription(uint8_t status) {
    switch (status) {
      case STRMSTAT_OFF: return "Stream is offline";
      case STRMSTAT_INIT: return "Stream is initializing";
      case STRMSTAT_BOOT: return "Stream is booting";
      case STRMSTAT_WAIT: return "Stream is waiting for data";
      case STRMSTAT_READY: return "Stream is online";
      case STRMSTAT_SHUTDOWN: return "Stream is shutting down";
      case STRMSTAT_OFFLINE: return "Stream is offline";
      case STRMSTAT_INVALID: return "Stream status is invalid?!";
      default: return "Stream status is unknown?!";
    }
  }
} // namespace Util

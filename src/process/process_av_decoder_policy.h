#pragma once
namespace Mist {
  inline bool hardwareDecoderFailurePermanent(bool formatDeclined, bool producedFrame) {
    return formatDeclined && !producedFrame;
  }
}

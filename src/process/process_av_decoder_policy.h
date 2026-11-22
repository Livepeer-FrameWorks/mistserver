#pragma once
namespace Mist {
  class DecoderErrorStreak {
      unsigned int errors = 0;

    public:
      bool invalidPacket() { return ++errors >= 128; }
      void decodedFrame() { errors = 0; }
  };
  inline bool hardwareDecoderFailurePermanent(bool formatDeclined, bool producedFrame) {
    return formatDeclined && !producedFrame;
  }
} // namespace Mist

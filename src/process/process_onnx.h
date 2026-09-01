#include "../input/input.h"
#include "../output/output.h"

#include <mist/defines.h>
#include <mist/json.h>
#include <mist/stream.h>

// Forward declaration
struct VideoPacket;

namespace Mist {
  class ProcONNX {
    public:
      ProcONNX() {}
      bool CheckConfig();
      void Run();
      void runSourceThread();
      void runSinkThread();
      void runProcessThread();
      JSON::Value processVideoFrame(const VideoPacket & vp);
  };

  class ProcessSource : public Output {
    public:
      ProcessSource(Socket::Connection & c, Util::Config & cfg, JSON::Value & capa);
      bool isRecording() override { return false; }
      bool isReadyForPlay() override { return true; }
      static void init(Util::Config *cfg, JSON::Value & capa);
      virtual bool onFinish() override;
      virtual void dropTrack(size_t trackId, const std::string & reason, bool probablyBad = true) override;
      virtual void sendHeader() override;
      virtual void connStats(uint64_t now, Comms::Connections & statComm) override;
      virtual void sendNext() override;

    protected:
      inline virtual bool keepGoing() override { return config->is_active; }

    private:
      bool sendFirst{false}; //< Whether first packet has been sent
      uint64_t sendPacketTime;
  };
} // namespace Mist

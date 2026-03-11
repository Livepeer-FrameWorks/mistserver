
#include "output.h"

extern "C" {
  #include <libavcodec/avcodec.h>
}

namespace Mist{
  class OutJPEGReceiver : public Output{
  public:
    OutJPEGReceiver(Socket::Connection &conn, Util::Config & cfg, JSON::Value & capa);
    ~OutJPEGReceiver();
    static void init(Util::Config *cfg, JSON::Value & capa);
    void sendNext(){};
    static bool listenMode(Util::Config * config){return true;};
    bool isReadyForPlay(){return true;}
    void requestHandler(bool readable);
  private:
    Util::ResizeablePointer bufferJPEG;
    Util::ResizeablePointer bufferH264;
    Util::ResizeablePointer ppsInfo;
    Util::ResizeablePointer spsInfo;
    uint64_t lastPos;
    bool bufferValid;
    size_t trkIdx;
    const AVCodec *codec_H264; ///< Output codec for libav
    const AVCodec *codec_JPEG; ///< Input codec for libav 
    AVCodecContext *context_H264; ///< Output context for libav
    AVCodecContext *context_JPEG; ///< Input context for libav
    AVFrame *frame_JPEG; ///< Raw frame data for libav
    uint64_t offset;
    bool isKey;

    std::string getStatsName();
    void skipFrame();
    uint64_t parseMarker();
    uint64_t getFrameSize();
    void printError(std::string preamble, int code);
    bool decodeJPEG(uint64_t size);
    bool encodeH264();
    bool retrievePacket();
    void sendH264();
    void setInit();

  protected:
    inline virtual bool keepGoing(){
      return config->is_active && (!listenMode(config) || myConn);
    }
  };
}// namespace Mist

typedef Mist::OutJPEGReceiver mistOut;

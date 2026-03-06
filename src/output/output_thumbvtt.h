#include "output_http.h"
#include <string>

namespace Mist{
  class OutThumbVTT : public HTTPOutput{
  public:
    OutThumbVTT(Socket::Connection &conn, Util::Config &cfg, JSON::Value &capa);
    ~OutThumbVTT();
    static void init(Util::Config *cfg, JSON::Value &capa);
    void onHTTP();
    void sendNext();
    void sendHeader();
    bool isReadyForPlay();
  private:
    size_t findSpriteTrack();
    bool pushMode;
    std::string boundary;
    size_t vttTrack;
    size_t spriteTrack;
    std::string pendingVtt;
    std::string pendingJpeg;
    void pushPair();
  };
}// namespace Mist

typedef Mist::OutThumbVTT mistOut;

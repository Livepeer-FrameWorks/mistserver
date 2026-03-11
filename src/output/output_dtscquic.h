#include "output.h"
#include <mist/url.h>
#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_gnutls.h>

namespace Mist{

  class OutDTSCQuic : public Output{
  public:
    OutDTSCQuic(Socket::Connection &conn, Util::Config & cfg, JSON::Value & capa);
    ~OutDTSCQuic();
    static void init(Util::Config *cfg, JSON::Value & capa);
    void onRequest();
    void sendNext();
    void sendHeader();
    static bool listenMode(Util::Config * config){return !(config->getString("target").size());}
    void onFail(const std::string &msg, bool critical = false);
    void stats(bool force = false);
    void sendCmd(const JSON::Value &data);
    void sendOk(const std::string &msg);
    bool isFileTarget();
    bool veryLiveSeek();

  private:
    unsigned int lastActive; ///< Time of last sending of data.
    std::string getStatsName();
    std::string salt;
    HTTP::URL pushUrl;
    void handlePush(DTSC::Scan &dScan);
    void handlePlay(DTSC::Scan &dScan);
    bool isSyncReceiver;
  };
}// namespace Mist

typedef Mist::OutDTSCQuic mistOut;

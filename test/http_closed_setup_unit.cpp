#include "../src/output/output_http.h"

#include <mist/timing.h>

#include <cassert>
#include <cstdio>
#include <cstring>

class ClosedSetup : public Mist::HTTPOutput {
  public:
    bool isPushing() override { return false; }
    ClosedSetup(Socket::Connection & connection, Util::Config & config, JSON::Value & capa)
      : Mist::HTTPOutput(connection, config, capa) {}
    void expiredHTTPSetup() {
      myConn.close();
      parseData = false;
      lastHTTPRequest = Util::bootSecs() - 61;
      requestHandler(false);
    }
};

int main() {
  Util::Config config("closed-setup-test");
  JSON::Value capa;
  Mist::HTTPOutput::init(&config, capa);
  Socket::Connection connection;
  ClosedSetup output(connection, config, capa);
  Util::exitReason[0] = 0;
  output.expiredHTTPSetup();
  Util::logExitReason(ER_CLEAN_INACTIVE, "received no data for 10+ seconds");
  if (strcmp(Util::exitReason, "received no data for 10+ seconds")) {
    fprintf(stderr, "closed WHEP setup stole the media exit reason: %s\n", Util::exitReason);
    return 1;
  }
  return 0;
}

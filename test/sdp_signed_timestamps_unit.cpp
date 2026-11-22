#include "../src/output/output_sdp.h"

#include <mist/bitfields.h>

#include <cassert>
#include <cstdio>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

class SDPClockProbe : public Mist::OutSDP {
  public:
    SDPClockProbe(Socket::Connection & conn, Util::Config & cfg, JSON::Value & capa) : Mist::OutSDP(conn, cfg, capa) {}
    bool isRecording() override { return true; }
    void prepare(uint16_t port) {
      meta.reInit("", true);
      thisIdx = meta.addTrack();
      meta.setID(thisIdx, 1);
      meta.setType(thisIdx, "video");
      meta.setCodec(thisIdx, "H264");
      meta.setWidth(thisIdx, 320);
      meta.setHeight(thisIdx, 180);
      meta.setInit(thisIdx, std::string("\x01\x42\x00\x1e\xff\xe0\x00", 7));
      userSelect[thisIdx];
      targetParams["targetIP"] = "127.0.0.1";
      targetParams["startPort"] = std::to_string(port);
      sendHeader();
    }
    void packet(uint64_t dts, int64_t offset) {
      const char data[] = {0, 0, 0, 1, 0x65};
      thisPacket.genericFill(dts, offset, 1, data, sizeof(data), 0, true);
      sendNext();
    }
};

int main() {
  int receiver = socket(AF_INET, SOCK_DGRAM, 0);
  assert(receiver >= 0);
  sockaddr_in address = {};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  assert(bind(receiver, (sockaddr *)&address, sizeof(address)) == 0);
  socklen_t length = sizeof(address);
  assert(getsockname(receiver, (sockaddr *)&address, &length) == 0);
  timeval timeout = {2, 0};
  setsockopt(receiver, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  int fds[2];
  assert(pipe(fds) == 0);
  Socket::Connection connection(fds[1], -1);
  Util::Config cfg("sdp-clock-probe");
  JSON::Value capa;
  Mist::OutSDP::init(&cfg, capa);
  SDPClockProbe output(connection, cfg, capa);
  output.prepare(ntohs(address.sin_port));
  for (const int64_t dts : {40, 120}) {
    output.packet(dts, -80);
    char data[2048];
    const ssize_t bytes = recv(receiver, data, sizeof(data), 0);
    const uint32_t expected = uint32_t((dts - 80) * 90);
    if (bytes < 12 || Bit::btohl(data + 4) != expected) {
      fprintf(stderr, "SDP sendNext lost signed composition time: DTS=%lld offset=-80 expected=%u actual=%u bytes=%zd\n",
              (long long)dts, expected, bytes >= 12 ? Bit::btohl(data + 4) : 0, bytes);
      return 1;
    }
  }
  close(receiver);
  close(fds[0]);
  puts("PASS real SDP sendNext emits signed composition time modulo the 32-bit RTP clock");
}

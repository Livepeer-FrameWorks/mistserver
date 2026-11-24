#include "output_hls.h"

#include <mist/shared_memory.h>
#include <mist/util.h>

#include <arpa/inet.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sys/socket.h>
#include <unistd.h>

class Probe : public Mist::OutHLS {
  public:
    bool isPushing() override { return false; }
    bool isRecording() override { return false; }
    Probe(Socket::Connection & conn, Util::Config & cfg, JSON::Value & cap) : Mist::OutHLS(conn, cfg, cap) {
      streamName = "unavailable-probe";
      responded = false;
      H.method = "GET";
      H.protocol = "HTTP/1.1";
      reqUrl = "http://localhost/hls/unavailable-probe/index.m3u8";
    }
};

int main(int argc, char **argv) {
  if (argc > 1 && std::string(argv[1]) == "PLAY_REWRITE") {
    char input[4096];
    while (read(STDIN_FILENO, input, sizeof(input)) > 0) {}
    return 0;
  }
  const bool denial = argc > 1 && std::string(argv[1]) == "deny";
  DTSC::trackValidMask = TRACK_VALID_EXT_HUMAN;
  char tmp[] = "/tmp/mist-unavailable-XXXXXX";
  if (!mkdtemp(tmp)) { return 3; }
  setenv("TMP", tmp, 1);
  setenv("UDP_API", "udp://127.0.0.1:0", 1);
  setenv("MIST_CONTROL", "1", 1);
  // A bound socket without listen keeps the endpoint deterministically refused.
  const int unavailable = socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in address = {};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(unavailable, (sockaddr *)&address, sizeof(address))) { return 3; }
  socklen_t len = sizeof(address);
  getsockname(unavailable, (sockaddr *)&address, &len);
  char name[NAME_BUFFER_SIZE];
  snprintf(name, sizeof(name), SHM_TRIGGER, "PLAY_REWRITE");
  std::string response;
  {
    IPC::sharedPage page(name, 32768, true, false);
    if (!page.mapped) { return 3; }
    Util::RelAccX trigger(page.mapped, false);
    trigger.addField("url", RAX_128STRING);
    trigger.addField("sync", RAX_UINT);
    trigger.addField("streams", RAX_256RAW);
    trigger.addField("params", RAX_128STRING);
    trigger.addField("default", RAX_128STRING);
    trigger.addField("onfail", RAX_UINT);
    trigger.setReady();
    trigger.setString("url", denial ? argv[0] : "http://127.0.0.1:" + std::to_string(ntohs(address.sin_port)) + "/decision", 0);
    trigger.setInt("sync", 1, 0);
    trigger.setInt("onfail", Triggers::ACT_DENY, 0);
    trigger.setString("params", "", 0);
    trigger.setString("default", "", 0);
    memset(trigger.getPointer("streams", 0), 0, 256);
    trigger.setRCount(1);
    trigger.setEndPos(1);
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair)) { return 3; }
    {
      Socket::Connection conn(pair[0]);
      Util::Config config("http-unavailable-test");
      JSON::Value cap;
      Mist::OutHLS::init(&config, cap);
      Probe output(conn, config, cap);
      output.initialize();
    }
    char buf[4096];
    ssize_t n;
    while ((n = read(pair[1], buf, sizeof(buf))) > 0) { response.append(buf, n); }
    close(pair[1]);
  }
  close(unavailable);
  rmdir(tmp);
  std::cerr << response << std::endl;
  if (denial) {
    if (response.find(" 404 ") == std::string::npos || response.find("Retry-After:") != std::string::npos) {
      std::cerr << "FAIL authoritative denial must remain final" << std::endl;
      return 1;
    }
    std::cerr << "PASS authoritative denial remains HTTP 404" << std::endl;
    return 0;
  }
  if (response.find(" 503 ") == std::string::npos) {
    std::cerr << "FAIL unavailable PLAY_REWRITE must produce HTTP 503, not a permanent playback rejection" << std::endl;
    return 1;
  }
  if (response.find("Retry-After: 1") == std::string::npos || response.find("no-store") == std::string::npos) {
    std::cerr << "FAIL transient playback refusal must be retryable and uncacheable" << std::endl;
    return 1;
  }
  std::cerr << "PASS transport unavailability is a retryable closed refusal" << std::endl;
  return 0;
}

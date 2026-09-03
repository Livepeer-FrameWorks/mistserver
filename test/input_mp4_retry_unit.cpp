#include "../src/input/input_mp4.h"

#include <mist/http_parser.h>
#include <mist/socket.h>
#include <mist/timing.h>

#include <atomic>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

namespace {

  bool readRequest(Socket::Connection & connection, HTTP::Parser & request) {
    const uint64_t deadline = Util::bootMS() + 3000;
    while (connection && Util::bootMS() < deadline) {
      connection.spool();
      if (request.Read(connection)) { return true; }
      Util::sleep(10);
    }
    return false;
  }

  void sendResponse(Socket::Connection & connection, const std::string & code, const std::string & message,
                    const std::string & body, const std::string & contentRange = "") {
    HTTP::Parser response;
    response.protocol = "HTTP/1.1";
    response.body = body;
    response.SetHeader("Accept-Ranges", "bytes");
    response.SetHeader("Connection", "close");
    response.SetHeader("Content-Length", std::to_string(body.size()));
    if (contentRange.size()) { response.SetHeader("Content-Range", contentRange); }
    response.SendResponse(code, message, connection);
  }

  class InputMP4Probe : public Mist::InputMP4 {
    public:
      explicit InputMP4Probe(Util::Config *config) : Mist::InputMP4(config) {
        standAlone = true;
        activityCounter = Util::bootSecs();
      }

      bool openSource(const HTTP::URL & url) {
        readPos = 0;
        readBuffer.truncate(0);
        return inFile.open(url);
      }

      bool fetch(size_t position, size_t length) { return shiftTo(position, length); }

      std::string buffered() const { return std::string((const char *)readBuffer, readBuffer.size()); }
  };

} // namespace

int main() {
  if (!Mist::mp4IncompleteReadMayRecover(std::string::npos, 8) || !Mist::mp4IncompleteReadMayRecover(8, 2) ||
      Mist::mp4IncompleteReadMayRecover(8, 8) || Mist::mp4IncompleteReadMayRecover(4, 8)) {
    std::cerr << "MP4 incomplete-read retry policy does not distinguish missing bytes from known EOF" << std::endl;
    return 1;
  }

  Socket::Server server(0, "127.0.0.1", false);
  if (!server.getBoundAddr().port()) {
    std::cerr << "Could not bind MP4 retry fixture" << std::endl;
    return 1;
  }

  std::atomic<bool> servedAll(false);
  std::thread peer([&]() {
    Socket::Connection headConnection = server.accept();
    HTTP::Parser headRequest;
    if (!readRequest(headConnection, headRequest) || headRequest.method != "HEAD") { return; }
    sendResponse(headConnection, "200", "OK", "12345678");
    headConnection.close();

    for (size_t attempt = 0; attempt < 3; ++attempt) {
      Socket::Connection shortConnection = server.accept();
      HTTP::Parser shortRequest;
      if (!readRequest(shortConnection, shortRequest) || shortRequest.method != "GET" || shortRequest.GetHeader("Range") != "bytes=0-") {
        return;
      }
      sendResponse(shortConnection, "206", "Partial Content", "ab", "bytes 0-1/8");
      shortConnection.close();
    }

    Socket::Connection retryConnection = server.accept();
    HTTP::Parser retryRequest;
    if (!readRequest(retryConnection, retryRequest) || retryRequest.method != "GET" || retryRequest.GetHeader("Range") != "bytes=0-") {
      return;
    }
    sendResponse(retryConnection, "206", "Partial Content", "abcdefgh", "bytes 0-7/8");
    retryConnection.close();
    servedAll = true;
  });

  Util::Config config("input-mp4-retry-unit");
  config.is_active = true;
  InputMP4Probe input(&config);
  const HTTP::URL url("http://127.0.0.1:" + std::to_string(server.getBoundAddr().port()) + "/asset.mp4");
  const bool opened = input.openSource(url);
  const bool fetched = opened && input.fetch(0, 4);
  const bool correctData = input.buffered().size() >= 4 && input.buffered().compare(0, 4, "abcd") == 0;

  server.close();
  peer.join();
  if (!opened) { std::cerr << "MP4 retry fixture did not open" << std::endl; }
  if (!fetched) { std::cerr << "MP4 input did not retry a short ranged read" << std::endl; }
  if (!correctData) { std::cerr << "MP4 input did not replace the partial buffer with retried bytes" << std::endl; }
  if (!servedAll) { std::cerr << "MP4 retry fixture did not receive the expected retry" << std::endl; }
  return opened && fetched && correctData && servedAll ? 0 : 1;
}

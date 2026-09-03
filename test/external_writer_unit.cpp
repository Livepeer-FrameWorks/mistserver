#include <mist/dtsc.h>
#include <mist/http_parser.h>
#include <mist/socket.h>
#include <mist/timing.h>

#include <atomic>
#include <cassert>
#include <string>
#include <thread>

namespace {

  bool readUploadHeaders(Socket::Connection & connection, HTTP::Parser & request) {
    request.headerOnly = true;
    const uint64_t deadline = Util::bootMS() + 3000;
    while (connection && Util::bootMS() < deadline) {
      connection.spool();
      if (request.Read(connection)) { return true; }
      Util::sleep(5);
    }
    return false;
  }

} // namespace

int main() {
  Socket::Server server(0, "127.0.0.1", false);
  assert(server.getBoundAddr().port());

  std::atomic<bool> validHeaders(false);
  std::atomic<bool> sawFinalChunk(false);
  std::atomic<bool> sentResponse(false);
  std::thread peer([&]() {
    Socket::Connection connection = server.accept();
    HTTP::Parser request;
    if (!readUploadHeaders(connection, request)) { return; }
    validHeaders = request.method == "PUT" && request.GetHeader("Transfer-Encoding") == "chunked";
    connection.SendNow("HTTP/1.1 100 Continue\r\n\r\n");

    std::string body;
    const uint64_t deadline = Util::bootMS() + 3000;
    while (connection && Util::bootMS() < deadline && body.find("0\r\n\r\n") == std::string::npos) {
      connection.spool();
      const size_t available = connection.Received().bytes();
      if (available) { body += connection.Received().remove(available); }
      if (body.find("0\r\n\r\n") == std::string::npos) { Util::sleep(5); }
    }
    if (body.find("0\r\n\r\n") == std::string::npos) { return; }
    sawFinalChunk = true;

    Util::sleep(150);
    HTTP::Parser response;
    response.protocol = "HTTP/1.1";
    response.SetHeader("Connection", "close");
    response.SetHeader("Content-Length", "0");
    response.SendResponse("201", "Created", connection);
    sentResponse = true;
  });

  DTSC::Meta metadata;
  metadata.reInit("", true);
  const std::string uri = "http://127.0.0.1:" + std::to_string(server.getBoundAddr().port()) + "/header.dtsh";
  const uint64_t started = Util::bootMS();
  metadata.toFile(uri);
  const uint64_t elapsed = Util::bootMS() - started;

  peer.join();
  server.close();
  assert(validHeaders);
  assert(sawFinalChunk);
  assert(sentResponse);
  assert(elapsed >= 100);
  assert(elapsed < 3000);
  return 0;
}

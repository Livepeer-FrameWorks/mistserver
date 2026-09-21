#include <mist/http_parser.h>
#include <mist/socket.h>
#include <mist/timing.h>
#include <mist/urireader.h>

#include <atomic>
#include <iostream>
#include <string>
#include <thread>

namespace {
  bool readRequest(Socket::Connection & conn, HTTP::Parser & request) {
    const uint64_t deadline = Util::bootMS() + 3000;
    while (conn && Util::bootMS() < deadline) {
      conn.spool();
      if (request.Read(conn)) { return true; }
      Util::sleep(1);
    }
    return false;
  }

  bool check(bool keepAlive) {
    Socket::Server server(0, "127.0.0.1", true);
    if (!server.getBoundAddr().port()) { return false; }
    const std::string body(8192, '\xff');
    std::atomic<bool> ready(false), finished(false), served(false);
    std::thread peer([&]() {
      const uint64_t deadline = Util::bootMS() + 5000;
      auto accept = [&]() {
        Socket::Connection conn;
        while (!finished && Util::bootMS() < deadline) {
          conn = server.accept(true);
          if (conn) { break; }
          Util::sleep(1);
        }
        return conn;
      };
      Socket::Connection conn = accept();
      HTTP::Parser head;
      if (!readRequest(conn, head) || head.method != "HEAD") { return; }
      conn.SendNow("HTTP/1.1 200 OK\r\nAccept-Ranges: bytes\r\nContent-Length: 8192\r\nConnection: " +
                   std::string(keepAlive ? "keep-alive" : "close") + "\r\n\r\n");
      if (!keepAlive) {
        conn.close();
        conn = accept();
      }
      HTTP::Parser get;
      if (!readRequest(conn, get) || get.method != "GET" || get.GetHeader("Range") != "bytes=0-") { return; }
      // Send headers and binary data together only after binaryMode() is set.
      while (!ready && !finished && Util::bootMS() < deadline) { Util::sleep(1); }
      if (!ready) { return; }
      conn.SendNow("HTTP/1.1 206 Partial Content\r\nContent-Length: 8192\r\n"
                   "Content-Range: bytes 0-8191/8192\r\nConnection: keep-alive\r\n\r\n" +
                   body);
      served = true;
      while (!finished && Util::bootMS() < deadline) { Util::sleep(1); }
    });

    HTTP::URIReader reader;
    const bool opened = reader.open(HTTP::URL("http://127.0.0.1:" + std::to_string(server.getBoundAddr().port()) + "/asset"));
    reader.binaryMode();
    ready = true;
    const uint64_t deadline = Util::bootMS() + 2000;
    while (opened && reader.getDataCallbackPos() < body.size() && Util::bootMS() < deadline) {
      reader.readSome(body.size(), reader);
      Util::sleep(1);
    }
    bool passed = false;
    if (reader.getDataCallbackPos() == body.size()) {
      char *data = 0;
      size_t size = 0;
      passed = reader.readSome(data, size, body.size()) == body.size() && std::string(data, size) == body;
    }
    finished = true;
    reader.close();
    peer.join();
    if (!passed || !served) {
      std::cerr << "Binary HTTP body stalled or changed; keepalive=" << keepAlive << std::endl;
    }
    return passed && served;
  }
} // namespace

int main() {
  const bool closed = check(false);
  const bool persistent = check(true);
  return closed && persistent ? 0 : 1;
}

#include <mist/http_parser.h>
#include <mist/socket.h>
#include <mist/timing.h>
#include <mist/urireader.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <iostream>
#include <limits>
#include <string>
#include <thread>

namespace {
  bool localPointerReads() {
    char path[] = "/tmp/uri-reader-local-XXXXXX";
    const int fd = mkstemp(path);
    if (fd < 0) { return false; }
    const std::string payload("\0\5hello", 7);
    const bool written = write(fd, payload.data(), payload.size()) == (ssize_t)payload.size();
    close(fd);
    HTTP::URIReader reader{std::string(path)};
    auto read = [&](size_t wanted, const std::string & expected) {
      char *data = 0;
      size_t size = 0;
      const size_t got = reader.readSome(data, size, wanted);
      const bool ok = got == expected.size() && size == expected.size() &&
        (size == 0 || (data && !std::memcmp(data, expected.data(), size)));
      if (!ok) { std::cerr << "Local pointer read: got " << got << ", wanted " << expected.size() << std::endl; }
      return ok;
    };
    bool ok = written && read(2, payload.substr(0, 2)) && reader.getPos() == 2;
    ok = reader.seek(2) && read(99, "hello") && reader.isEOF() && ok;
    ok = reader.seek(0) && reader.readSome(2, reader) == 2 && read(2, payload.substr(0, 2)) && read(5, "hello") &&
      reader.isEOF() && ok;
    ok = reader.seek(0) && reader.readSome(2, reader) == 2 && read(1, payload.substr(0, 1)) &&
      read(std::numeric_limits<size_t>::max(), payload.substr(1)) && reader.isEOF() && ok;
    ok = reader.seek(99) && read(2, "") && reader.isEOF() && ok;
    ok = reader.seek(0) && read(2, payload.substr(0, 2)) && ok;
    reader.close();
    unlink(path);
    return ok;
  }

  bool pipePointerReads() {
    int fds[2];
    if (pipe(fds)) { return false; }
    const bool written = write(fds[1], "data", 4) == 4;
    close(fds[1]);
    HTTP::URIReader reader;
    const bool opened = reader.open(fds[0]);
    char *data = 0;
    size_t size = 0;
    bool ok = written && opened && reader.readSome(data, size, 4) == 4 && size == 4 && data && !std::memcmp(data, "data", 4);
    ok = reader.readSome(data, size, 1) == 0 && reader.isEOF() && ok;
    reader.close();
    if (!ok) { std::cerr << "Raw-stream pointer read stalled or changed" << std::endl; }
    return ok;
  }

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
  const bool local = localPointerReads();
  const bool pipe = pipePointerReads();
  const bool closed = check(false);
  const bool persistent = check(true);
  return local && pipe && closed && persistent ? 0 : 1;
}

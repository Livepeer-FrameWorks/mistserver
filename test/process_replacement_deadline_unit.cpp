#include "../src/input/process_replacement_request.h"

#include <mist/config.h>
#include <mist/defines.h>
#include <mist/procs.h>
#include <mist/shared_memory.h>
#include <mist/util.h>

#include <arpa/inet.h>
#include <atomic>
#include <csignal>
#include <cstring>
#include <fstream>
#include <iostream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {
  void publish(IPC::sharedPage & page, const std::string & handler, unsigned count) {
    char name[NAME_BUFFER_SIZE];
    snprintf(name, sizeof(name), SHM_TRIGGER, "PROCESS_REPLACE");
    page.init(name, 32768, true, false);
    Util::RelAccX data(page.mapped, false);
    data.addField("url", RAX_128STRING);
    data.addField("sync", RAX_UINT);
    data.addField("streams", RAX_256RAW);
    data.addField("params", RAX_128STRING);
    data.addField("default", RAX_128STRING);
    data.addField("onfail", RAX_UINT);
    data.setReady();
    for (unsigned i = 0; i < count; ++i) {
      data.setString("url", handler, i);
      data.setInt("sync", 1, i);
      memset(data.getPointer("streams", i), 0, 4);
      data.setString("params", "", i);
      data.setString("default", "", i);
      data.setInt("onfail", Triggers::ACT_LEGACY, i);
    }
    data.setRCount(count);
    data.setEndPos(count);
  }
} // namespace

int main(int argc, char **argv) {
  if (argc > 1 && std::string(argv[1]) == "PROCESS_REPLACE") {
    std::ofstream(getenv("MIST_DEADLINE_PID")) << getpid();
    Util::sleep(2000);
    return 0;
  }
  if (argc != 2) { return 2; }
  struct sigaction action = {};
  action.sa_handler = [](int) {};
  sigaction(SIGPIPE, &action, 0);
  char directory[] = "/tmp/mist-replace-deadline-XXXXXX";
  if (!mkdtemp(directory)) { return 2; }
  setenv("TMP", directory, 1);
  const std::string pidPath = std::string(directory) + "/handler.pid";
  setenv("MIST_DEADLINE_PID", pidPath.c_str(), 1);
  int listener = socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in address = {};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(listener, (sockaddr *)&address, sizeof(address)) || listen(listener, 8)) { return 2; }
  socklen_t size = sizeof(address);
  getsockname(listener, (sockaddr *)&address, &size);
  std::atomic<bool> stopping(false);
  std::atomic<unsigned> delay(1000), requests(0);
  std::thread server([&]() {
    while (!stopping) {
      int client = accept(listener, 0, 0);
      if (client < 0) { break; }
      char buffer[8192];
      std::string request;
      while (request.find("deadline-payload") == std::string::npos) {
        int got = recv(client, buffer, sizeof(buffer), 0);
        if (got <= 0) { break; }
        request.append(buffer, got);
      }
      if (request.find("deadline-payload") != std::string::npos) { ++requests; }
      Util::sleep(delay.load());
      const std::string body = "[{\"process\":\"AV\",\"codec\":\"H264\"}]";
      const std::string reply =
        "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
      send(client, reply.data(), reply.size(), 0);
      close(client);
    }
  });
  bool passed = true;
  {
    IPC::sharedPage page;
    const std::string handler = "http://127.0.0.1:" + std::to_string(ntohs(address.sin_port)) + "/replace";
    publish(page, handler, 2);
    std::string response;
    uint64_t begin = Util::bootMS();
    bool success = Mist::runProcessReplacementRequest("deadline", "deadline-payload", response, argv[1], 1500);
    uint64_t elapsed = Util::bootMS() - begin;
    if (success || elapsed > 1750) {
      std::cerr << "multiple HTTP handlers exceeded the overall deadline: success=" << success << " elapsed=" << elapsed << "ms\n";
      passed = false;
    }
    // Allow the server to finish its canceled response before the success control.
    Util::sleep(700);
    delay = 0;
    publish(page, handler, 1);
    success = Mist::runProcessReplacementRequest("deadline", "deadline-payload", response, argv[1], 1000);
    if (!success || response.find("H264") == std::string::npos || requests < 3) {
      std::cerr << "an ordinary replacement response did not survive the bounded request: success=" << success
                << " requests=" << requests << " response=" << response << "\n";
      passed = false;
    }
    publish(page, Util::getMyPath() + "procreplacedeadlinetest", 1);
    begin = Util::bootMS();
    success = Mist::runProcessReplacementRequest("deadline", std::string(1024 * 1024, 'x'), response, argv[1], 500);
    elapsed = Util::bootMS() - begin;
    pid_t handlerPid = 0;
    std::ifstream(pidPath) >> handlerPid;
    const uint64_t reapDeadline = Util::bootMS() + 150;
    while (handlerPid && !kill(handlerPid, 0) && Util::bootMS() < reapDeadline) { Util::sleep(2); }
    if (success || elapsed > 750 || !handlerPid || !kill(handlerPid, 0)) {
      std::cerr << "unread EXEC payload exceeded the deadline or left its handler: elapsed=" << elapsed
                << "ms pid=" << handlerPid << "\n";
      passed = false;
    }
  }
  stopping = true;
  shutdown(listener, SHUT_RDWR);
  close(listener);
  server.join();
  unlink(pidPath.c_str());
  rmdir(directory);
  if (passed) {
    std::cout << "PASS aggregate HTTP deadline, ordinary response, blocked EXEC input and owned handler cleanup\n";
  }
  return passed ? 0 : 1;
}

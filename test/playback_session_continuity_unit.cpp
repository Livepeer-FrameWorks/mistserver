#include "output_hls.h"

#include <mist/comms.h>
#include <mist/shared_memory.h>
#include <mist/util.h>

#include <arpa/inet.h>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char **environ;

namespace {
  const std::string admittedToken = "admitted-token-" + std::to_string(getpid());

  class UnavailableHandler {
    public:
      int fd;
      std::string url;
      std::atomic<bool> stopping{false};
      std::thread worker;
      UnavailableHandler() {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address = {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(fd, (sockaddr *)&address, sizeof(address)) || listen(fd, 8)) { std::abort(); }
        socklen_t len = sizeof(address);
        getsockname(fd, (sockaddr *)&address, &len);
        url = "http://127.0.0.1:" + std::to_string(ntohs(address.sin_port)) + "/decision";
        fcntl(fd, F_SETFL, O_NONBLOCK);
        worker = std::thread([&]() {
          while (!stopping) {
            int client = accept(fd, 0, 0);
            if (client < 0) {
              usleep(1000);
              continue;
            }
            std::string received;
            char bytes[4096];
            ssize_t count;
            while ((count = read(client, bytes, sizeof(bytes))) > 0) {
              received.append(bytes, count);
              size_t end = received.find("\r\n\r\n");
              if (end == std::string::npos) { continue; }
              std::string headers = received.substr(0, end);
              for (char & c : headers) { c = std::tolower((unsigned char)c); }
              size_t length = headers.find("\r\ncontent-length:");
              size_t body = length == std::string::npos ? 0 : strtoull(headers.c_str() + length + 17, 0, 10);
              if (received.size() >= end + 4 + body) { break; }
            }
            const char response[] =
              "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            write(client, response, sizeof(response) - 1);
            close(client);
          }
        });
      }
      ~UnavailableHandler() {
        stopping = true;
        worker.join();
        close(fd);
      }
  };

  class Probe : public Mist::OutHLS {
    public:
      Probe(Socket::Connection & conn, Util::Config & cfg, JSON::Value & cap, const std::string & token,
            const std::string & origin, const std::string & stream)
        : Mist::OutHLS(conn, cfg, cap) {
        streamName = stream;
        tkn = token;
        viewerOrigin = origin;
        reqUrl = "http://edge/hls/" + stream + "/index.m3u8?tkn=" + token;
      }
      Mist::PlayRewriteOutcome rewrite() { return applyPlayRewrite("playback"); }
      std::string sessionID() {
        Comms::Connections identity;
        return identity.generateSession(streamName, getConnectedBinHost(), tkn, getStatsName(),
                                        Comms::sessionViewerMode, Comms::viewerSessionOrigin(viewerOrigin, ""));
      }
      std::string host() { return getConnectedBinHost(); }
      std::string target() const { return streamName; }
  };

  struct Request {
      Mist::PlayRewriteOutcome outcome;
      std::string sessionID;
      std::string host;
      std::string target;
  };

  Request request(const std::string & token = admittedToken, const std::string & origin = "https://embed.example",
                  const std::string & stream = "continuity-probe") {
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets)) { std::abort(); }
    Request result;
    {
      Socket::Connection conn(sockets[0]);
      Util::Config cfg("playback-continuity-test");
      JSON::Value cap;
      Mist::OutHLS::init(&cfg, cap);
      Probe output(conn, cfg, cap, token, origin, stream);
      result.sessionID = output.sessionID();
      result.host = output.host();
      result.outcome = output.rewrite();
      result.target = output.target();
    }
    close(sockets[1]);
    return result;
  }
} // namespace

int main(int argc, char **argv) {
  if (argc > 2 && std::string(argv[1]) == "ADMISSION_GATE") {
    IPC::semaphore gate(argv[2], O_RDWR, ACCESSPERMS, 1, true);
    if (!gate || !gate.tryWaitOneSecond()) { return 3; }
    if (write(STDOUT_FILENO, "R", 1) != 1) { return 3; }
    char release;
    if (read(STDIN_FILENO, &release, 1) != 1) { return 3; }
    gate.post();
    return write(STDOUT_FILENO, "D", 1) == 1 ? 0 : 3;
  }
  if (argc > 1 && std::string(argv[1]) == "PLAY_REWRITE") {
    char buffer[4096];
    while (read(STDIN_FILENO, buffer, sizeof(buffer)) > 0) {}
    if (getenv("MIST_CONTINUITY_ADMIT")) { std::cout << "continuity-probe"; }
    return 0;
  }
  char tmp[] = "/tmp/mist-continuity-XXXXXX";
  if (!mkdtemp(tmp)) { return 3; }
  setenv("TMP", tmp, 1);
  setenv("UDP_API", "udp://127.0.0.1:0", 1);
  setenv("MIST_CONTROL", "1", 1);
  DTSC::trackValidMask = TRACK_VALID_EXT_HUMAN;
  Comms::sessionViewerMode = argc > 1 && std::string(argv[1]) == "protocol-bound" ? 0x0F : SESS_BUNDLE_DEFAULT_VIEWER;

  IPC::sharedPage settingsPage(SHM_GLOBAL_CONF, 4096, true, false);
  if (!settingsPage) { return 3; }
  Util::RelAccX settings(settingsPage.mapped, false);
  const auto modes = {std::make_pair("sessionViewerMode", Comms::sessionViewerMode),
                      std::make_pair("sessionInputMode", Comms::sessionInputMode),
                      std::make_pair("sessionOutputMode", Comms::sessionOutputMode),
                      std::make_pair("sessionUnspecifiedMode", Comms::sessionUnspecifiedMode),
                      std::make_pair("sessionStreamInfoMode", Comms::sessionStreamInfoMode),
                      std::make_pair("tknMode", Comms::tknMode)};
  for (const auto & value : modes) { settings.addField(value.first, RAX_UINT); }
  for (const char *key : {"iid", "hrn", "udpApi"}) { settings.addField(key, RAX_256STRING); }
  settings.setRCount(1);
  settings.setEndPos(1);
  for (const auto & value : modes) { settings.setInt(value.first, value.second, 0); }
  settings.setString("udpApi", "udp://127.0.0.1:9", 0);
  settings.setReady();

  // HTTP 503 exercises the same failed-handler branch without the platform's
  // TCP refusal timeout dominating the admission checks.
  UnavailableHandler unavailable;
  const std::string unavailableURL = unavailable.url;
  int failures = 0;
  auto expect = [&](const char *name, Mist::PlayRewriteOutcome actual, Mist::PlayRewriteOutcome wanted) {
    if (actual != wanted) {
      std::cerr << "FAIL " << name << ": outcome=" << actual << ", want=" << wanted << std::endl;
      ++failures;
    } else {
      std::cerr << "PASS " << name << std::endl;
    }
  };
  {
    char name[NAME_BUFFER_SIZE];
    snprintf(name, sizeof(name), SHM_TRIGGER, "PLAY_REWRITE");
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
    trigger.setString("url", argv[0], 0);
    trigger.setInt("sync", 1, 0);
    trigger.setInt("onfail", Triggers::ACT_DENY, 0);
    trigger.setString("params", "", 0);
    trigger.setString("default", "", 0);
    memset(trigger.getPointer("streams", 0), 0, 256);
    trigger.setRCount(1);
    trigger.setEndPos(1);
    setenv("MIST_CONTINUITY_ADMIT", "1", 1);
    Request admitted = request();
    expect("initial playback decision admits the viewer", admitted.outcome, Mist::PLAY_REWRITE_UNCHANGED);
    unsetenv("MIST_CONTINUITY_ADMIT");

    // The session page is the existing viewer's admission/accounting state.
    // Its semaphore is released only after USER_NEW has answered.
    snprintf(name, sizeof(name), SEM_SESSION, admitted.sessionID.c_str());
    IPC::semaphore sessionLock;
    sessionLock.open(name, O_CREAT | O_RDWR, ACCESSPERMS, 1);
    Comms::Connections session;
    session.reload(admitted.sessionID, true);
    Comms::Connections viewer;
    viewer.reload(admitted.sessionID);
    viewer.setStatus(COMM_STATUS_ACTIVE | COMM_STATUS_NOKILL);
    viewer.setStream("continuity-probe");
    viewer.setHost(admitted.host);
    viewer.setConnector("HLS");
    viewer.setNow(Util::bootSecs());
    Comms::Sessions statistics;
    statistics.reload(true);
    IPC::sharedPage statsPage(COMMS_STATISTICS, 0, false, false);
    Util::RelAccX stats(statsPage.mapped, false);
    if (!stats.hasField("requestedstream")) { stats.addField("requestedstream", RAX_STRING, 100); }
    Comms::Sessions sessionProcess;
    sessionProcess.reload();
    sessionProcess.setStatus(COMM_STATUS_ACTIVE | COMM_STATUS_NOKILL);
    sessionProcess.setSessId(admitted.sessionID);
    sessionProcess.setStream("continuity-probe");
    sessionProcess.setNow(Util::bootSecs());
    stats.setString("requestedstream", "logical-alias", 0);

    trigger.setString("url", unavailableURL, 0);
    expect("admitted HLS session keeps playing while the handler is down", request().outcome, Mist::PLAY_REWRITE_UNCHANGED);
    Request aliased = request(admittedToken, "https://embed.example", "logical-alias");
    expect("an admitted alias retains its resolved stream during an outage", aliased.outcome, Mist::PLAY_REWRITE_CHANGED);
    if (aliased.target != "continuity-probe") {
      std::cerr << "FAIL admitted alias target=" << aliased.target << std::endl;
      ++failures;
    }
    expect("an alias with another token needs a decision",
           request("new-token", "https://embed.example", "logical-alias").outcome, Mist::PLAY_REWRITE_UNAVAILABLE);
    expect("a new token needs a decision", request("new-token").outcome, Mist::PLAY_REWRITE_UNAVAILABLE);
    expect("a different origin needs a decision", request(admittedToken, "https://other.example").outcome, Mist::PLAY_REWRITE_UNAVAILABLE);
    expect("a different stream needs a decision",
           request(admittedToken, "https://embed.example", "other-stream").outcome, Mist::PLAY_REWRITE_UNAVAILABLE);
    // MistSession owns admission in a separate process. Its acknowledgement
    // proves the named semaphore is held before the output checks it.
    int input[2], output[2];
    if (pipe(input) || pipe(output)) { std::abort(); }
    // Child stdio owns 0 and 1; closing a fixture pipe must not close it.
    for (int *fd : {&input[0], &input[1], &output[0], &output[1]}) {
      if (*fd < 3) {
        const int copy = fcntl(*fd, F_DUPFD_CLOEXEC, 3);
        if (copy < 0) { std::abort(); }
        close(*fd);
        *fd = copy;
      }
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, input[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO);
    for (int fd : {input[0], input[1], output[0], output[1]}) { posix_spawn_file_actions_addclose(&actions, fd); }
    char *gateArgs[] = {argv[0], const_cast<char *>("ADMISSION_GATE"), name, 0};
    pid_t gatePID = 0;
    const int spawned = posix_spawnp(&gatePID, argv[0], &actions, 0, gateArgs, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(input[0]);
    close(output[1]);
    char ack;
    const ssize_t ready = spawned ? -1 : read(output[0], &ack, 1);
    if (spawned || ready != 1 || ack != 'R') {
      std::cerr << "FAIL admission fixture spawn=" << spawned << " stdin=" << input[0] << ',' << input[1]
                << " stdout=" << output[0] << ',' << output[1] << " ready=" << ready << std::endl;
      std::abort();
    }
    expect("pending admission cannot serve media", request().outcome, Mist::PLAY_REWRITE_UNAVAILABLE);
    if (write(input[1], "D", 1) != 1 || read(output[0], &ack, 1) != 1 || ack != 'D') { std::abort(); }
    close(input[1]);
    close(output[0]);
    int status;
    while (waitpid(gatePID, &status, 0) < 0 && errno == EINTR) {}
    sessionProcess.setNow(Util::bootSecs() - STATS_DELAY - 1);
    expect("a stale connection cannot admit media", request().outcome, Mist::PLAY_REWRITE_UNAVAILABLE);
    sessionProcess.setNow(Util::bootSecs());
    sessionProcess.setPid(0);
    expect("a missing session process cannot admit media", request().outcome, Mist::PLAY_REWRITE_UNAVAILABLE);
    sessionProcess.setPid(getpid());
    viewer.setStatus(COMM_STATUS_DISCONNECT | COMM_STATUS_NOKILL);
    expect("HLS gaps between HTTP requests retain the admitted session", request().outcome, Mist::PLAY_REWRITE_UNCHANGED);
    sessionProcess.setStatus(COMM_STATUS_DISCONNECT | COMM_STATUS_NOKILL);
    expect("a disconnected session cannot admit media", request().outcome, Mist::PLAY_REWRITE_UNAVAILABLE);
    sessionProcess.setStatus(COMM_STATUS_ACTIVE | COMM_STATUS_NOKILL);
    viewer.setStatus(COMM_STATUS_ACTIVE | COMM_STATUS_NOKILL);
    trigger.setString("url", argv[0], 0);
    expect("authoritative denial still ends an admitted session", request().outcome, Mist::PLAY_REWRITE_DENIED);
    trigger.setString("url", unavailableURL, 0);
    session.setExit();
    expect("a revoked session cannot serve media", request().outcome, Mist::PLAY_REWRITE_UNAVAILABLE);
    viewer.setStatus(COMM_STATUS_DISCONNECT | COMM_STATUS_NOKILL);
    sessionProcess.setStatus(COMM_STATUS_DISCONNECT | COMM_STATUS_NOKILL);
  }
  rmdir(tmp);
  return failures ? 1 : 0;
}

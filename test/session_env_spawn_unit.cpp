// Session boots and trigger firings from many threads of one process at once, as in a
// process binary whose source and sink threads boot their sessions while its upload threads
// fire triggers. Every session child must start, see exactly its own SESSION_* values and
// none of a trigger's MIST_* values; every trigger handler must get its own identity.
//
// The binary stages itself in a private directory next to a copy named MistSession, because
// Comms::Connections starts getMyPath() + "MistSession". Run as MistSession, it stands in for
// the session: it records the environment it was started with and serves the connection page.
// Run with an ENVSTRESSEXEC trigger name as its argument, it is an executable trigger handler.
#include <mist/comms.h>
#include <mist/config.h>
#include <mist/defines.h>
#include <mist/procs.h>
#include <mist/shared_memory.h>
#include <mist/timing.h>
#include <mist/triggers.h>
#include <mist/util.h>

#include <arpa/inet.h>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <mutex>
#include <netinet/in.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
  const size_t SPAWN_THREADS = 6;
  // Each thread boots at least SPAWNS_PER_THREAD sessions and keeps going for MIN_SPAWN_MS.
  const size_t SPAWNS_PER_THREAD = 40;
  const size_t MAX_SPAWNS_PER_THREAD = 2000;
  const uint64_t MIN_SPAWN_MS = 4000;
  const size_t TRIGGER_THREADS = 8;
  const size_t EXEC_TRIGGERS = 20;
  const char *PROTOCOL = "ENVSTRESS";

  int fail(const std::string & message) {
    std::cerr << message << std::endl;
    return 1;
  }

  std::string baseName(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
  }

  std::string envOr(const char *name, const char *fallback) {
    const char *value = getenv(name);
    return value ? value : fallback;
  }

  void writeFile(const std::string & path, const std::string & contents) {
    std::ofstream out((path + ".tmp").c_str());
    out << contents;
    out.close();
    rename((path + ".tmp").c_str(), path.c_str());
  }

  /// MistSession stand-in: records its environment, then serves the connection page until the
  /// connecting thread has claimed and released its record.
  int runSession(int argc, char **argv) {
    if (argc < 2) { return 1; }
    const std::string sessionId = argv[1];
    std::string tkn;
    for (int i = 2; i + 1 < argc; ++i) {
      if (std::string(argv[i]) == "--tkn") { tkn = argv[i + 1]; }
    }
    std::stringstream record;
    record << "tkn=" << tkn << "\n";
    record << "requrl=" << envOr("SESSION_REQURL", "<unset>") << "\n";
    record << "protocol=" << envOr("SESSION_PROTOCOL", "<unset>") << "\n";
    record << "tuuid=" << envOr("MIST_TUUID", "<unset>") << "\n";
    record << "trigger=" << envOr("MIST_TRIGGER", "<unset>") << "\n";
    writeFile(envOr("ENVSTRESS_RESULTS", ".") + "/session-" + tkn, record.str());

    char semName[NAME_BUFFER_SIZE];
    snprintf(semName, NAME_BUFFER_SIZE, SEM_SESSION, sessionId.c_str());
    IPC::semaphore sessionLock(semName, O_CREAT | O_RDWR, ACCESSPERMS, 1);
    Comms::Connections connections;
    connections.reload(sessionId, true);
    if (!connections) { return 1; }
    bool claimed = false;
    const uint64_t deadline = Util::bootMS() + 20000;
    while (Util::bootMS() < deadline) {
      bool active = false;
      for (size_t i = 0; i < connections.recordCount(); ++i) {
        const uint8_t status = connections.getStatus(i);
        if (status == COMM_STATUS_INVALID) { continue; }
        claimed = true;
        if (!(status & COMM_STATUS_DISCONNECT)) { active = true; }
      }
      if (claimed && !active) { break; }
      Util::sleep(20);
    }
    // A record that is still connected must not get its process signalled when this
    // stand-in removes the page.
    for (size_t i = 0; i < connections.recordCount(); ++i) {
      if (connections.getStatus(i) != COMM_STATUS_INVALID) {
        connections.setStatus(connections.getStatus(i) | COMM_STATUS_NOKILL | COMM_STATUS_DISCONNECT, i);
      }
    }
    return 0;
  }

  /// Executable trigger handler: records the trigger identity it was started with.
  int runTriggerHandler() {
    std::string payload;
    std::getline(std::cin, payload);
    std::stringstream record;
    record << "trigger=" << envOr("MIST_TRIGGER", "") << "\n";
    record << "tuuid=" << envOr("MIST_TUUID", "") << "\n";
    record << "time=" << envOr("MIST_TIME", "") << "\n";
    record << "date=" << envOr("MIST_DATE", "") << "\n";
    record << "requrl=" << envOr("SESSION_REQURL", "<unset>") << "\n";
    writeFile(envOr("ENVSTRESS_RESULTS", ".") + "/exec-" + payload, record.str());
    std::cout << "true";
    return 0;
  }

  bool copyFile(const std::string & from, const std::string & to) {
    std::ifstream in(from.c_str(), std::ios::binary);
    std::ofstream out(to.c_str(), std::ios::binary);
    if (!in || !out) { return false; }
    out << in.rdbuf();
    out.close();
    return out.good() && chmod(to.c_str(), 0755) == 0;
  }

  std::map<std::string, std::string> readRecord(const std::string & path) {
    std::map<std::string, std::string> ret;
    std::ifstream in(path.c_str());
    std::string line;
    while (std::getline(in, line)) {
      size_t eq = line.find('=');
      if (eq != std::string::npos) { ret[line.substr(0, eq)] = line.substr(eq + 1); }
    }
    return ret;
  }

  void removeTree(const std::string & dir) {
    DIR *d = opendir(dir.c_str());
    if (d) {
      while (struct dirent *e = readdir(d)) {
        const std::string name = e->d_name;
        if (name == "." || name == "..") { continue; }
        const std::string path = dir + "/" + name;
        struct stat st;
        if (!lstat(path.c_str(), &st) && S_ISDIR(st.st_mode)) {
          removeTree(path);
        } else {
          unlink(path.c_str());
        }
      }
      closedir(d);
    }
    rmdir(dir.c_str());
  }

  /// Answers every trigger request with "true", counting requests that lack the trigger identity.
  /// It serves one connection at a time, so under load a sender can time out and give up before
  /// its whole request arrived; such a request is counted as abandoned, not checked.
  class TriggerStub {
    public:
      TriggerStub() : listener(-1), port(0), stop(false), missingIdentity(0), requests(0), abandoned(0) {}

      bool start() {
        listener = socket(AF_INET, SOCK_STREAM, 0);
        if (listener < 0) { return false; }
        int enabled = 1;
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
        sockaddr_in address;
        memset(&address, 0, sizeof(address));
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(listener, (sockaddr *)&address, sizeof(address)) || listen(listener, 64)) { return false; }
        socklen_t len = sizeof(address);
        getsockname(listener, (sockaddr *)&address, &len);
        port = ntohs(address.sin_port);
        server = std::thread([this]() { serve(); });
        return true;
      }

      void finish() {
        stop = true;
        shutdown(listener, SHUT_RDWR);
        close(listener);
        if (server.joinable()) { server.join(); }
      }

      int listener;
      uint16_t port;
      std::atomic<bool> stop;
      std::atomic<size_t> missingIdentity;
      std::atomic<size_t> requests;
      std::atomic<size_t> abandoned;

    private:
      std::thread server;

      static bool hasValue(const std::string & request, const std::string & name) {
        const size_t field = request.find("\r\n" + name + ": ");
        if (field == std::string::npos) { return false; }
        const size_t value = field + name.size() + 4;
        return value < request.size() && request[value] != '\r';
      }

      void serve() {
        while (!stop) {
          const int conn = accept(listener, NULL, NULL);
          if (conn < 0) { continue; }
          std::string request;
          char buffer[4096];
          while (request.find("\r\n\r\n") == std::string::npos) {
            const ssize_t got = recv(conn, buffer, sizeof(buffer), 0);
            if (got <= 0) { break; }
            request.append(buffer, got);
          }
          if (request.find("\r\n\r\n") == std::string::npos) {
            ++abandoned;
            close(conn);
            continue;
          }
          ++requests;
          if (!hasValue(request, "X-Trigger-UUID") || !hasValue(request, "X-Trigger-UnixMillis") || !hasValue(request, "Date")) {
            ++missingIdentity;
          }
          // The sender may have given up on its request already: answering it must not end this
          // process with SIGPIPE.
          const std::string response = "HTTP/1.1 200 OK\r\nContent-Length: 4\r\nConnection: close\r\n\r\ntrue";
#ifdef SO_NOSIGPIPE
          const int noSigPipe = 1;
          setsockopt(conn, SOL_SOCKET, SO_NOSIGPIPE, &noSigPipe, sizeof(noSigPipe));
#endif
#ifdef MSG_NOSIGNAL
          send(conn, response.data(), response.size(), MSG_NOSIGNAL);
#else
          send(conn, response.data(), response.size(), 0);
#endif
          close(conn);
        }
      }
  };

  /// Publishes a one-handler trigger page the way the controller does.
  void publishTrigger(IPC::sharedPage & page, const std::string & type, const std::string & handler) {
    char pageName[NAME_BUFFER_SIZE];
    snprintf(pageName, NAME_BUFFER_SIZE, SHM_TRIGGER, type.c_str());
    page.init(pageName, 32 * 1024, true, false);
    Util::RelAccX tPage(page.mapped, false);
    tPage.addField("url", RAX_128STRING);
    tPage.addField("sync", RAX_UINT);
    tPage.addField("streams", RAX_256RAW);
    tPage.addField("params", RAX_128STRING);
    tPage.addField("default", RAX_128STRING);
    tPage.addField("onfail", RAX_UINT);
    tPage.setReady();
    tPage.setString("url", handler, 0);
    tPage.setInt("sync", 1, 0);
    char *streams = tPage.getPointer("streams", 0);
    if (streams) { memset(streams, 0, 4); }
    tPage.setString("params", "", 0);
    tPage.setString("default", "", 0);
    tPage.setInt("onfail", Triggers::ACT_LEGACY, 0);
    tPage.setRCount(1);
    tPage.setEndPos(1);
  }

  int runStress() {
    const std::string dir = Util::getMyPath();
    const std::string results = envOr("ENVSTRESS_RESULTS", "");
    if (results.empty()) { return fail("results directory missing"); }

    TriggerStub stub;
    if (!stub.start()) { return fail("could not start the trigger stub"); }
    const std::string httpType = "ENVSTRESSHTTP" + std::to_string(getpid());
    const std::string execType = "ENVSTRESSEXEC" + std::to_string(getpid());
    IPC::sharedPage httpPage, execPage;
    publishTrigger(httpPage, httpType, "http://127.0.0.1:" + std::to_string(stub.port) + "/trigger");
    publishTrigger(execPage, execType, dir + "envstress");

    std::atomic<bool> stopTriggers(false);
    std::atomic<size_t> triggerFirings(0);
    std::vector<std::thread> triggerThreads;
    for (size_t t = 0; t < TRIGGER_THREADS; ++t) {
      triggerThreads.emplace_back([&]() {
        while (!stopTriggers) {
          Triggers::doTrigger(httpType, "payload", "");
          ++triggerFirings;
        }
      });
    }
    std::thread execThread([&]() {
      for (size_t i = 0; i < EXEC_TRIGGERS; ++i) { Triggers::doTrigger(execType, std::to_string(i) + "\n", ""); }
    });

    std::mutex failureMutex;
    std::vector<std::string> failures;
    std::vector<std::thread> spawnThreads;
    std::vector<size_t> spawned(SPAWN_THREADS, 0);
    const uint64_t spawnStart = Util::bootMS();
    for (size_t t = 0; t < SPAWN_THREADS; ++t) {
      spawnThreads.emplace_back([&, t]() {
        // Boot sessions only while triggers are firing.
        while (!triggerFirings && Util::bootMS() < spawnStart + 5000) { Util::sleep(1); }
        for (size_t i = 0; i < MAX_SPAWNS_PER_THREAD; ++i) {
          if (i >= SPAWNS_PER_THREAD && Util::bootMS() >= spawnStart + MIN_SPAWN_MS) { break; }
          spawned[t] = i + 1;
          const std::string tkn = "t" + std::to_string(getpid()) + "x" + std::to_string(t) + "x" + std::to_string(i);
          Comms::Connections conn;
          const uint64_t start = Util::bootMS();
          conn.reload("envstress", std::string(16, '\0'), tkn, PROTOCOL, "url-" + tkn);
          // The session stand-in may still be creating its page when this thread first looks.
          while (!conn && Util::bootMS() < start + 10000) {
            Util::sleep(20);
            conn.reload("envstress", std::string(16, '\0'), tkn, PROTOCOL, "url-" + tkn);
          }
          if (!conn) {
            std::lock_guard<std::mutex> guard(failureMutex);
            failures.push_back("session " + tkn + " did not become available within 10 s");
            continue;
          }
          conn.unload();
        }
      });
    }
    for (std::thread & th : spawnThreads) { th.join(); }
    const uint64_t spawnMs = Util::bootMS() - spawnStart;
    stopTriggers = true;
    for (std::thread & th : triggerThreads) { th.join(); }
    execThread.join();
    stub.finish();

    // Session stand-ins write their record before serving the page; give stragglers a moment.
    Util::sleep(200);
    size_t sessions = 0;
    for (size_t t = 0; t < SPAWN_THREADS; ++t) {
      for (size_t i = 0; i < spawned[t]; ++i) {
        const std::string tkn = "t" + std::to_string(getpid()) + "x" + std::to_string(t) + "x" + std::to_string(i);
        std::map<std::string, std::string> rec = readRecord(results + "/session-" + tkn);
        if (rec.empty()) {
          failures.push_back("session for " + tkn + " never started");
          continue;
        }
        ++sessions;
        if (rec["requrl"] != "url-" + tkn) {
          failures.push_back("session for " + tkn + " saw SESSION_REQURL=" + rec["requrl"]);
        }
        if (rec["protocol"] != PROTOCOL) {
          failures.push_back("session for " + tkn + " saw SESSION_PROTOCOL=" + rec["protocol"]);
        }
        if (rec["tuuid"] != "<unset>" || rec["trigger"] != "<unset>") {
          failures.push_back("session for " + tkn + " inherited trigger variables (MIST_TUUID=" + rec["tuuid"] + ")");
        }
      }
    }
    for (size_t i = 0; i < EXEC_TRIGGERS; ++i) {
      std::map<std::string, std::string> rec = readRecord(results + "/exec-" + std::to_string(i));
      if (rec.empty()) {
        failures.push_back("executable trigger " + std::to_string(i) + " did not run");
        continue;
      }
      if (rec["trigger"] != execType || rec["tuuid"].empty() || rec["time"].empty() || rec["date"].empty()) {
        failures.push_back("executable trigger " + std::to_string(i) + " lacked its identity variables");
      }
      if (rec["requrl"] != "<unset>") {
        failures.push_back("executable trigger " + std::to_string(i) + " inherited SESSION_REQURL=" + rec["requrl"]);
      }
    }
    if (stub.missingIdentity) {
      failures.push_back(std::to_string(stub.missingIdentity) + " HTTP trigger requests lacked their identity headers");
    }
    if (!stub.requests) { failures.push_back("no HTTP trigger reached the stub"); }
    for (const char *name : {"MIST_TUUID", "MIST_TIME", "MIST_DATE", "SESSION_REQURL", "SESSION_PROTOCOL"}) {
      if (getenv(name)) { failures.push_back(std::string("the process environment kept ") + name); }
    }

    std::cout << sessions << " sessions booted in " << spawnMs << " ms alongside " << triggerFirings
              << " HTTP trigger firings (" << stub.requests << " requests, " << stub.abandoned << " abandoned)" << std::endl;
    for (size_t i = 0; i < failures.size() && i < 20; ++i) { std::cerr << failures[i] << std::endl; }
    if (failures.size()) { return fail(std::to_string(failures.size()) + " failures"); }
    return 0;
  }

  /// Copies this binary into a fresh directory, next to a MistSession copy when withSession is
  /// set, and runs it there in the given mode.
  int stageAndRun(bool withSession, const char *mode) {
    char tmpl[] = "/tmp/mist-envstress.XXXXXX";
    const char *dir = mkdtemp(tmpl);
    if (!dir) { return fail("could not create a staging directory"); }
    const std::string stage = dir;
    const std::string results = stage + "/results";
    mkdir(results.c_str(), 0755);
    const std::string me = Util::getMyPathWithBin();
    if (!copyFile(me, stage + "/envstress") || (withSession && !copyFile(me, stage + "/MistSession"))) {
      removeTree(stage);
      return fail("could not stage the test binary");
    }
    // Single-threaded startup: the staged run inherits the results directory.
    setenv("ENVSTRESS_RESULTS", results.c_str(), 1);
    const std::string staged = stage + "/envstress";
    int status = 1;
    pid_t child = fork();
    if (!child) {
      execl(staged.c_str(), staged.c_str(), mode, (char *)NULL);
      _exit(127);
    }
    if (child > 0) {
      int raw = 0;
      if (waitpid(child, &raw, 0) == child && WIFEXITED(raw)) { status = WEXITSTATUS(raw); }
      if (WIFSIGNALED(raw)) { std::cerr << "the staged run died of signal " << WTERMSIG(raw) << std::endl; }
    }
    if (getenv("MIST_KEEP_TEST_ARTIFACTS")) {
      std::cerr << "kept " << stage << std::endl;
    } else {
      removeTree(stage);
    }
    return status;
  }
} // namespace

int main(int argc, char **argv) {
  if (baseName(argv[0]) == "MistSession") { return runSession(argc, argv); }
  if (argc > 1 && std::string(argv[1]).compare(0, 13, "ENVSTRESSEXEC") == 0) { return runTriggerHandler(); }
  if (argc > 1 && std::string(argv[1]) == "run") { return runStress(); }
  return stageAndRun(true, "run");
}

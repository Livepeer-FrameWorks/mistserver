#include <mist/comms.h>
#include <mist/defines.h>
#include <mist/shared_memory.h>
#include <mist/util.h>

#include <csignal>
#include <cstring>
#include <iostream>
#include <semaphore.h>
#include <sys/wait.h>
#include <unistd.h>

// A rolling controller restart keeps the statistics page the previous release created. A
// session started by the new release registers on that page even though it lacks the fields
// added since, exactly as MistSession does after an in-place Mist update.
namespace {
  void addPreviousReleaseSessionFields(Util::RelAccX & page) {
    page.addField("status", RAX_UINT);
    page.addField("pid", RAX_64UINT);
    page.addField("now", RAX_64UINT);
    page.addField("time", RAX_64UINT);
    page.addField("lastsecond", RAX_64UINT);
    page.addField("down", RAX_64UINT);
    page.addField("up", RAX_64UINT);
    page.addField("host", RAX_RAW, 16);
    page.addField("stream", RAX_STRING, 100);
    page.addField("connector", RAX_STRING, 20);
    page.addField("pktcount", RAX_64UINT);
    page.addField("pktloss", RAX_64UINT);
    page.addField("pktretrans", RAX_64UINT);
    page.addField("tags", RAX_STRING, 512);
    page.addField("sessid", RAX_STRING, 80);
  }
} // namespace

int main() {
  // A session that died while registering leaves the statistics lock taken; start from none.
  sem_unlink(SEM_STATISTICS);
  IPC::sharedPage stale(COMMS_STATISTICS, 0, false, false);
  if (stale) { stale.master = true; }
  stale.close();

  IPC::sharedPage page(COMMS_STATISTICS, COMMS_STATISTICS_INITSIZE, true);
  if (!page) {
    std::cerr << "FAIL could not create the statistics page" << std::endl;
    return 1;
  }
  Util::RelAccX stats(page.mapped, false);
  addPreviousReleaseSessionFields(stats);
  stats.setRCount((COMMS_STATISTICS_INITSIZE - stats.getOffset()) / stats.getRSize());
  stats.setEndPos(1);
  stats.setPresent(0);
  stats.setReady();

  const std::string sessionID = "Icommsrollinglayout" + std::to_string(getpid());
  pid_t child = fork();
  if (child == 0) {
    Comms::Sessions session;
    session.reload();
    if (!session) { _exit(2); }
    std::string host(16, '\0');
    session.setHost(host);
    session.setSessId(sessionID);
    session.setStream("rolling-layout");
    session.setRequestedStream("rolling-alias");
    _exit(session.getSessId() == sessionID ? 0 : 3);
  }
  int status = 0;
  while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}

  int failures = 0;
  if (WIFSIGNALED(status)) {
    std::cerr << "FAIL a session on the previous release's statistics page died from signal " << WTERMSIG(status) << std::endl;
    ++failures;
  } else if (!WIFEXITED(status) || WEXITSTATUS(status)) {
    std::cerr << "FAIL a session on the previous release's statistics page exited with " << WEXITSTATUS(status) << std::endl;
    ++failures;
  }
  if (!failures && stats.getPointer("sessid", 0) && sessionID != stats.getPointer("sessid", 0)) {
    std::cerr << "FAIL the session did not register its id on the previous release's page" << std::endl;
    ++failures;
  }
  page.master = true;
  sem_unlink(SEM_STATISTICS);
  if (!failures) { std::cout << "ok   a session registers on the previous release's statistics page" << std::endl; }
  return failures ? 1 : 0;
}

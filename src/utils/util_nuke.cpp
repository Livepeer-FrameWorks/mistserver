#include <mist/comms.h>
#include <mist/config.h>
#include <mist/procs.h>
#include <mist/shared_memory.h>
#include <mist/stream.h>
#include <mist/stream_status.h>
#include <mist/timing.h>
#include <mist/util.h>

/// Gets a PID from a shared memory page, if it exists
uint64_t getPidFromPage(const char * pagePattern){
  char pageName[NAME_BUFFER_SIZE];
  snprintf(pageName, NAME_BUFFER_SIZE, pagePattern, Util::streamName);
  IPC::sharedPage pidPage(pageName, 8, false, false);
  if (pidPage){
    return *(uint64_t*)(pidPage.mapped);
  }
  return 0;
}

/// Deletes a shared memory page, if it exists
void nukePage(const char * pagePattern){
  char pageName[NAME_BUFFER_SIZE];
  snprintf(pageName, NAME_BUFFER_SIZE, pagePattern, Util::streamName);
  IPC::sharedPage page(pageName, 0, false, false);
  page.master = true;
}

/// Deletes a semaphore, if it exists
void nukeSem(const char * pagePattern){
  char pageName[NAME_BUFFER_SIZE];
  snprintf(pageName, NAME_BUFFER_SIZE, pagePattern, Util::streamName);
  IPC::semaphore sem(pageName, O_RDWR, ACCESSPERMS, 0, true);
  if (sem){sem.unlink();}
}

// Remove process that are no longer running from the process list
void cleanUpProcessList(std::set<pid_t> & checkPids) {
  if (checkPids.size()) {
    std::set<pid_t> gonePids;
    for (auto & P : checkPids) {
      if (!Util::Procs::isRunning(P)) { gonePids.insert(P); }
    }
    for (auto & P : gonePids) { checkPids.erase(P); }
  }
}

void killProcesses(std::set<pid_t> & checkPids, const char *procType) {
  cleanUpProcessList(checkPids);
  if (checkPids.size()) {
    // Wait a bit to settle
    Util::sleep(1000);
    cleanUpProcessList(checkPids);
  }
  // Hard-kill any remaining processes
  if (checkPids.size()) {
    WARN_MSG("Hard killing %zu %s processes", checkPids.size(), procType);
    while (checkPids.size()) {
      INFO_MSG("Hard killing %s process %" PRIu64, procType, (uint64_t)*checkPids.begin());
      Util::Procs::Murder(*checkPids.begin());
      checkPids.erase(*checkPids.begin());
    }
  }
}

// Main semaphore for SEM_LIVE lock
IPC::semaphore mainSem, pullSem;
char mainSemName[NAME_BUFFER_SIZE], pullSemName[NAME_BUFFER_SIZE];

/// Attempts to lock the SEM_LIVE semaphore for the stream, only if not already locked.
/// Returns current lock status
bool tryLock() {
  if (!mainSem.locked()) {
    mainSem.open(mainSemName, O_CREAT | O_RDWR, ACCESSPERMS, 1);
    if (mainSem.tryWait()) { INFO_MSG("Placed input lock"); }
  }
  if (!pullSem.locked()) {
    pullSem.open(pullSemName, O_CREAT | O_RDWR, ACCESSPERMS, 1);
    if (pullSem.tryWait()) { INFO_MSG("Placed pull lock"); }
  }
  return mainSem.locked() && pullSem.locked();
}

/// The input and pull processes this nuke found or stopped: the generation it tears down.
std::set<pid_t> generation;

/// Adds the processes the stream's input and pull PID pages name to the generation.
void recordGeneration() {
  const char *pidPages[] = {SHM_STREAM_IPID, SHM_STREAM_PPID};
  for (const char *pidPage : pidPages) {
    uint64_t pid = getPidFromPage(pidPage);
    if (pid > 1) { generation.insert((pid_t)pid); }
  }
}

/// Whether a newer generation of the stream has started since this nuke began. A new input writes its
/// PID to the stream's input or pull PID page as soon as it holds the stream's lock, and the locks
/// this nuke holds cannot keep it out once an exiting input has unlinked their names. A running
/// process there that is not part of the generation means every page named after the stream now
/// belongs to that newer generation. A dead one is a leftover this nuke still has to clean.
bool newerGenerationStarted() {
  const char *pidPages[] = {SHM_STREAM_IPID, SHM_STREAM_PPID};
  for (const char *pidPage : pidPages) {
    uint64_t pid = getPidFromPage(pidPage);
    if (pid > 1 && !generation.count((pid_t)pid) && Util::Procs::isRunning((pid_t)pid)) { return true; }
  }
  return false;
}

/// Ends the nuke without touching the newer generation. Processes of the torn-down generation that
/// still run are stopped and then killed, since they are this nuke's by construction; every page
/// and semaphore named after the stream is left alone, and the lock names are released rather than
/// unlinked, since they now name the newer generation's locks.
int leaveNewerGeneration(const char *step) {
  WARN_MSG("Stream %s restarted during the nuke (before %s); leaving the new generation alone", Util::streamName, step);
  std::set<pid_t> leftovers;
  for (pid_t pid : generation) {
    if (Util::Procs::isRunning(pid)) {
      Util::Procs::Stop(pid);
      leftovers.insert(pid);
    }
  }
  killProcesses(leftovers, "previous generation");
  mainSem.close();
  pullSem.close();
  return 0;
}

int main(int argc, char **argv){
  Util::redirectLogsIfNeeded();
  if (argc < 2) {
    FAIL_MSG("Usage: %s STREAM_NAME", argv[0]);
    return 1;
  }
  Util::setStreamName(argv[1]);

  // Track process IDs that we want to ensure are fully off by the time we finish
  std::set<pid_t> checkPids;

  // Write stream name into mainSemName / pullSemName
  snprintf(mainSemName, NAME_BUFFER_SIZE, SEM_INPUT, Util::streamName);
  snprintf(pullSemName, NAME_BUFFER_SIZE, "/MstSemPull_%s", Util::streamName);
  recordGeneration();
  tryLock();

  uint8_t state = Util::getStreamStatus(Util::streamName);
  INFO_MSG("Current stream status: %s", Util::streamStatusDescription(state));
  uint64_t startTime = Util::bootMS();
  if (!Util::streamStatusIsTerminal(state)) { INFO_MSG("Attempting clean shutdown..."); }
  while (!Util::streamStatusIsTerminal(state) && Util::bootMS() < startTime + 5000) {
    if (newerGenerationStarted()) { return leaveNewerGeneration("stopping its input"); }
    uint64_t pid;
    pid = getPidFromPage(SHM_STREAM_IPID);
    if (pid > 1) {
      Util::Procs::Stop(pid);
      checkPids.insert(pid);
      generation.insert(pid);
    }
    pid = getPidFromPage(SHM_STREAM_PPID);
    if (pid > 1) {
      Util::Procs::Stop(pid);
      checkPids.insert(pid);
      generation.insert(pid);
    }
    if (!tryLock()) {
      Util::wait(1);
      if (!tryLock()) {
        Util::wait(2);
        tryLock();
      }
    }
    uint8_t prevState = state;
    state = Util::getStreamStatus(Util::streamName);
    if (prevState != state) { INFO_MSG("Current stream status: %s", Util::streamStatusDescription(state)); }
    Util::wait(10);
    tryLock();
  }

  // Ensure we have the input lock, one way or another
  if (!tryLock()) {
    if (!mainSem.locked()) {
      INFO_MSG("Breaking input lock forcefully...");
      mainSem.unlink();
    }
    if (!pullSem.locked()) {
      INFO_MSG("Breaking pull lock forcefully...");
      pullSem.unlink();
    }
    if (!tryLock()) {
      FAIL_MSG("Could not force input and/or pull lock..? Aborting!");
      return 2;
    }
  }

  if (newerGenerationStarted()) { return leaveNewerGeneration("stopping its inputs"); }
  INFO_MSG("Detecting running inputs...");
  // Scoping to clear up metadata and track providers
  {
    char pageName[NAME_BUFFER_SIZE];
    snprintf(pageName, NAME_BUFFER_SIZE, SHM_STREAM_META, argv[1]);
    IPC::sharedPage streamPage(pageName, 0, false, false);
    if (streamPage.mapped) {
      streamPage.master = true;
      Util::RelAccX stream(streamPage.mapped, false);
      if (stream.isReady()) {
        Util::RelAccX trackList(stream.getPointer("tracks"), false);
        if (trackList.isReady()) {
          for (size_t i = 0; i < trackList.getPresent(); i++) {
            IPC::sharedPage trackPage(trackList.getPointer("page", i), SHM_STREAM_TRACK_LEN, false, false);
            trackPage.master = true;
            pid_t pid = trackList.getInt("pid", i);
            if (pid > 1) {
              Util::Procs::Stop(pid);
              checkPids.insert(pid);
              generation.insert(pid);
            }
          }
        }
      }
    }
  }
  { // Double-check input and pull input process numbers just in case
    uint64_t pid;
    pid = getPidFromPage(SHM_STREAM_IPID);
    if (pid > 1) {
      Util::Procs::Stop(pid);
      checkPids.insert(pid);
      generation.insert(pid);
    }
    pid = getPidFromPage(SHM_STREAM_PPID);
    if (pid > 1) {
      Util::Procs::Stop(pid);
      checkPids.insert(pid);
      generation.insert(pid);
    }
  }
  killProcesses(checkPids, "input");
  if (newerGenerationStarted()) { return leaveNewerGeneration("wiping its shared memory"); }
  INFO_MSG("Detecting and wiping leftovers in shared memory...");
  // Scoping to clear up metadata and track providers
  {
    char pageName[NAME_BUFFER_SIZE];
    snprintf(pageName, NAME_BUFFER_SIZE, SHM_STREAM_META, argv[1]);
    IPC::sharedPage streamPage(pageName, 0, false, false);
    if (streamPage.mapped){
      streamPage.master = true;
      Util::RelAccX stream(streamPage.mapped, false);
      if (stream.isReady()){
        Util::RelAccX trackList(stream.getPointer("tracks"), false);
        if (trackList.isReady()){
          for (size_t i = 0; i < trackList.getPresent(); i++){
            IPC::sharedPage trackPage(trackList.getPointer("page", i), SHM_STREAM_TRACK_LEN, false, false);
            trackPage.master = true;
            pid_t pid = trackList.getInt("pid", i);
            if (pid > 1){
              Util::Procs::Stop(pid);
              checkPids.insert(pid);
              generation.insert(pid);
            }
            if (trackPage){
              Util::RelAccX track(trackPage.mapped, false);
              if (track.isReady()){
                Util::RelAccX pages(track.getPointer("pages"), false);
                if (pages.isReady()){
                  for (uint64_t j = pages.getDeleted(); j < pages.getEndPos(); j++){
                    char thisPageName[NAME_BUFFER_SIZE];
                    snprintf(thisPageName, NAME_BUFFER_SIZE, SHM_TRACK_DATA,
                             argv[1], i, (uint32_t)pages.getInt("firstkey", j));
                    IPC::sharedPage p(thisPageName, 0);
                    p.master = true;
                  }
                }
              }
            }
          }
        }
      }
    }
  }
  //Wipe relevant pages
  nukePage(SHM_STREAM_STATE);
  nukePage(SHM_STREAM_IPID);
  nukePage(SHM_STREAM_PPID);
  if (newerGenerationStarted()) { return leaveNewerGeneration("stopping its users"); }
  // Scoping to clear up users page
  {
    Comms::Users cleanUsers;
    cleanUsers.reload(Util::streamName, true);
    std::set<pid_t> checkPids;
    for (size_t i = 0; i < cleanUsers.recordCount(); ++i){
      uint8_t status = cleanUsers.getStatus(i);
      cleanUsers.setStatus(COMM_STATUS_INVALID, i);
      if (status != COMM_STATUS_INVALID && !(status & COMM_STATUS_DISCONNECT)){
        pid_t pid = cleanUsers.getPid(i);
        if (pid > 1 && !(cleanUsers.getStatus(i) & COMM_STATUS_NOKILL)){
          Util::Procs::Stop(pid);
          checkPids.insert(pid);
        }
      }
    }
    cleanUsers.setMaster(true);
  }
  killProcesses(checkPids, "output");
  if (newerGenerationStarted()) { return leaveNewerGeneration("removing its semaphores"); }
  nukePage(COMMS_USERS);
  nukeSem(SEM_USERS);
  nukeSem(SEM_LIVE);
  nukeSem(SEM_TRACKLIST);
  // Finally, remove the input and pull lock semaphores
  pullSem.unlink();
  mainSem.unlink();
  INFO_MSG("Completed cleanup");
  return 0;
}

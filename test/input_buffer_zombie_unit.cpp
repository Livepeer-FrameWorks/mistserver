#define private public
#define protected public
#include "../src/input/input_buffer.h"
#undef protected
#undef private

#include <mist/procs.h>

#include <cerrno>
#include <csignal>
#include <iostream>
#include <sys/wait.h>
#include <unistd.h>

int main() {
  Util::Config config("buffer-zombie-unit");
  Mist::InputBuffer input(&config);
  input.streamName = "zombie" + std::to_string(getpid());
  input.meta.reInit("", true);
  const size_t track = input.meta.addTrack();
  input.meta.setID(track, 1);
  input.meta.setType(track, "video");
  input.meta.setCodec(track, "H264");
  input.meta.setWidth(track, 640);
  input.meta.setHeight(track, 360);
  input.meta.validateTrack(track, TRACK_VALID_ALL);
  input.meta.update(0, 0, track, 1, 0, true, 1);
  input.meta.breakClaim(track);
  input.processControlledRealtime = false;
  input.everHadPush = true;
  input.hasPush = true;
  config.is_active = true;
  JSON::Value proc;
  proc["process"] = "ZombieProbe";
  proc["source"] = input.streamName;
  const std::string key = proc.toString();
  JSON::Value configured;
  configured.append(proc);
  input.processDeclarations[key].null();
  int gate[2];
  if (pipe(gate)) { return 2; }
  const pid_t child = fork();
  if (child < 0) {
    close(gate[0]);
    close(gate[1]);
    return 2;
  }
  if (!child) {
    close(gate[1]);
    char byte;
    while (read(gate[0], &byte, 1) < 0 && errno == EINTR) {}
    _exit(2);
  }
  close(gate[0]);
  input.runningProcs[key] = child;
  input.procBoots[key] = 1;
  input.checkProcesses(configured);
  const bool liveRetained = input.runningProcs.count(key) && !input.procHardFailed.count(key);
  close(gate[1]);
  siginfo_t info = {};
  int waited;
  do { waited = waitid(P_PID, child, &info, WEXITED | WNOWAIT); } while (waited && errno == EINTR);
  const bool zombieExists = !waited && !kill(child, 0) && info.si_status == 2;
  input.checkProcesses(configured);
  const bool collected = !input.runningProcs.count(key) && input.procHardFailed.count(key);
  input.runningProcs.clear();
  int status;
  while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
  if (!liveRetained || !zombieExists || !collected) {
    std::cerr << "FAIL: live retained=" << liveRetained << ", zombie confirmed=" << zombieExists
              << ", exit collected on supervisor pass=" << collected << std::endl;
    return 1;
  }
  std::cout << "Live child stays supervised; an unreaped exit is collected on the same pass with its exit code" << std::endl;
  return 0;
}

// The track list lock serializes only what appends to a stream's track list or grows it, and
// claims. Describing a track, validating or removing it, releasing a claim and the stream fields
// are written without it: the buffer and every producer write some of them for each packet, and a
// lock there would make every writer wait for any process that holds it.
#include <mist/defines.h>
#include <mist/dtsc.h>
#include <mist/shared_memory.h>

#include <iostream>
#include <string>

#ifdef __linux__
#include <dlfcn.h>
#include <semaphore.h>
#include <time.h>
#include <unistd.h>

namespace {
  size_t lockAttempts = 0;
}

extern "C" int sem_wait(sem_t *sem) {
  static int (*real)(sem_t *) = (int (*)(sem_t *))dlsym(RTLD_NEXT, "sem_wait");
  ++lockAttempts;
  return real(sem);
}

extern "C" int sem_trywait(sem_t *sem) {
  static int (*real)(sem_t *) = (int (*)(sem_t *))dlsym(RTLD_NEXT, "sem_trywait");
  ++lockAttempts;
  return real(sem);
}

extern "C" int sem_timedwait(sem_t *sem, const struct timespec *deadline) {
  static int (*real)(sem_t *, const struct timespec *) =
    (int (*)(sem_t *, const struct timespec *))dlsym(RTLD_NEXT, "sem_timedwait");
  ++lockAttempts;
  return real(sem, deadline);
}

namespace {
  int failures = 0;

  void expect(bool ok, const std::string & what) {
    if (!ok) {
      std::cerr << "FAIL: " << what << std::endl;
      ++failures;
    }
  }
} // namespace

int main() {
  const std::string streamName = "lockbudget" + std::to_string(getpid());
  size_t video = INVALID_TRACK_ID;
  {
    DTSC::Meta buffer;
    buffer.reInit(streamName, true);
    video = buffer.addTrack();
    buffer.addTrack();
    DTSC::Meta producer;
    producer.reInit(streamName, false, false);

    lockAttempts = 0;
    for (size_t i = 0; i < 1000; ++i) {
      buffer.setLive(true);
      buffer.setVod(false);
      buffer.setBootMsOffset(i);
      buffer.setUTCOffset(i, 1);
      buffer.setBufferWindow(i);
      buffer.setMaxKeepAway(i);
      buffer.setMinKeepAway(video, i);
      buffer.setSource("push://");
      buffer.setMinimumFragmentDuration(1000);
      producer.setType(video, "video");
      producer.setCodec(video, "H264");
      producer.setID(video, 1);
      producer.setSourceTrack(video, INVALID_TRACK_ID);
      producer.markUpdated(video);
      producer.validateTrack(video);
    }
    expect(!lockAttempts,
           "describing tracks and writing stream fields does not take the track list lock (took it " +
             std::to_string(lockAttempts) + " times)");

    lockAttempts = 0;
    buffer.breakClaim(video);
    expect(!lockAttempts, "releasing a claim does not take the track list lock");
    expect(producer.claimTrack(video), "a released track can be claimed");
    expect(lockAttempts == 1, "a claim takes the track list lock once (took it " + std::to_string(lockAttempts) + " times)");
    lockAttempts = 0;
    producer.abandonTrack(video);
    expect(!lockAttempts, "abandoning a claimed track does not take the track list lock");

    lockAttempts = 0;
    producer.addTrack();
    expect(lockAttempts == 1, "adding a track takes the track list lock once (took it " + std::to_string(lockAttempts) + " times)");
  }

  char name[NAME_BUFFER_SIZE];
  for (size_t idx = 0; idx < 2 * DEFAULT_TRACK_COUNT; ++idx) {
    snprintf(name, NAME_BUFFER_SIZE, SHM_STREAM_TM, streamName.c_str(), (uint32_t)getpid(), idx);
    IPC::sharedPage page(name, 0, false, false);
    if (page) { page.master = true; }
  }
  snprintf(name, NAME_BUFFER_SIZE, SEM_TRACKLIST, streamName.c_str());
  IPC::semaphore trackLock(name, O_CREAT | O_RDWR, ACCESSPERMS, 1);
  trackLock.unlink();

  if (failures) { return 1; }
  std::cout << "only appending, growing and claiming take the track list lock" << std::endl;
  return 0;
}
#else
int main() {
  std::cout << "semaphore call interposition is only set up on Linux" << std::endl;
  return 77;
}
#endif

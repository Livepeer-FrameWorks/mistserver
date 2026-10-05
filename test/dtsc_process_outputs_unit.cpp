// The output key of each process output and the buffer's resume hold of each track live on a page
// of their own, by track index, that the stream's master creates and nobody ever replaces: growing
// the track list cannot lose them, and the track list keeps the record layout every version uses.
// A stream whose master is of a version without that page has no keys: processes then register
// their outputs as new tracks, without errors.
#include <mist/defines.h>
#include <mist/dtsc.h>
#include <mist/shared_memory.h>

#include <cstdio>
#include <iostream>
#include <string>
#include <unistd.h>

namespace {
  int failures = 0;

  void expect(bool ok, const std::string & what) {
    if (!ok) {
      std::cerr << "FAIL: " << what << std::endl;
      ++failures;
    }
  }

  DTSC::TrackMetadata sprite() {
    DTSC::TrackMetadata trk;
    trk.type = "video";
    trk.codec = "JPEG";
    trk.width = 240;
    trk.height = 88;
    trk.output = "sprite";
    return trk;
  }

  std::string pageName(const char *pattern, const std::string & streamName) {
    char name[NAME_BUFFER_SIZE];
    snprintf(name, NAME_BUFFER_SIZE, pattern, streamName.c_str());
    return name;
  }

  bool pageExists(const char *pattern, const std::string & streamName) {
    IPC::sharedPage page(pageName(pattern, streamName), 0, false, false);
    return page.mapped;
  }

  void removeStream(const std::string & streamName) {
    char name[NAME_BUFFER_SIZE];
    for (size_t idx = 0; idx < 4 * DEFAULT_TRACK_COUNT; ++idx) {
      snprintf(name, NAME_BUFFER_SIZE, SHM_STREAM_TM, streamName.c_str(), (uint32_t)getpid(), idx);
      IPC::sharedPage page(name, 0, false, false);
      if (page) { page.master = true; }
    }
    const char *pages[] = {SHM_STREAM_META, SHM_STREAM_POUT};
    for (const char *pattern : pages) {
      IPC::sharedPage page(pageName(pattern, streamName), 0, false, false);
      if (page) { page.master = true; }
    }
    IPC::semaphore trackLock(pageName(SEM_TRACKLIST, streamName).c_str(), O_CREAT | O_RDWR, ACCESSPERMS, 1);
    trackLock.unlink();
  }

  const std::string identity = DTSC::processIdentity("{\"process\":\"Thumbs\"}");

  // A producer's key and the buffer's hold survive the list growing, and every process sees them.
  void keysSurviveGrowth() {
    const std::string streamName = "po" + std::to_string(getpid() % 100000);
    size_t created = INVALID_TRACK_ID;
    {
      DTSC::Meta buffer(streamName, true);
      buffer.setLive(true);
      expect(pageExists(SHM_STREAM_POUT, streamName), "the stream's master creates the process outputs page");
      DTSC::Meta producer(streamName, false, false);
      DTSC::outputKeyScope = identity;
      created = producer.addOrResumeTrack(sprite());
      DTSC::outputKeyScope.clear();
      buffer.reloadReplacedPagesIfNeeded();
      buffer.breakClaim(created);
      buffer.setResumeUntil(created, 12345);
      DTSC::Meta other(streamName, false, false);
      for (size_t i = 0; i < 2 * DEFAULT_TRACK_COUNT; ++i) { other.addTrack(); }

      DTSC::Meta reader(streamName, false, false);
      expect(reader.getOutputKey(created) == DTSC::outputKey(identity, "sprite"), "the output key survives the list growing");
      expect(reader.getResumeUntil(created) == 12345, "the resume hold survives the list growing");
      expect(producer.getOutputKey(created) == DTSC::outputKey(identity, "sprite"),
             "a process that has not reloaded the grown list sees the key too");

      DTSC::Meta restarted(streamName, false, false);
      DTSC::outputKeyScope = identity;
      expect(restarted.addOrResumeTrack(sprite()) == created, "a restarted producer resumes its track by key");
      DTSC::outputKeyScope.clear();
    }
    expect(!pageExists(SHM_STREAM_POUT, streamName), "the master removes the process outputs page with the stream");
    removeStream(streamName);
  }

  // The master is of a version without process outputs.
  void masterWithoutProcessOutputs() {
    const std::string streamName = "pn" + std::to_string(getpid() % 100000);
    {
      DTSC::Meta buffer(streamName, true);
      buffer.setLive(true);
      {
        IPC::sharedPage outputs(pageName(SHM_STREAM_POUT, streamName), 0, false, false);
        outputs.master = true;
      }
      DTSC::Meta producer(streamName, false, false);
      DTSC::outputKeyScope = identity;
      const size_t first = producer.addOrResumeTrack(sprite());
      expect(first != INVALID_TRACK_ID && producer.getOutputKey(first).empty(), "an output is registered without a key");
      producer.abandonTrack(first);
      DTSC::Meta restarted(streamName, false, false);
      const size_t second = restarted.addOrResumeTrack(sprite());
      expect(second != INVALID_TRACK_ID && second != first, "a restarted producer registers its output as a new track");
      expect(restarted.reserveOutputTrack(DTSC::outputKey(identity, "other")) == INVALID_TRACK_ID,
             "no output is reserved without a key to find it by");
      expect(!restarted.getResumeUntil(first), "no track is held");
      DTSC::outputKeyScope.clear();
    }
    removeStream(streamName);
  }
} // namespace

int main() {
  keysSurviveGrowth();
  masterWithoutProcessOutputs();
  if (failures) { return 1; }
  std::cout << "output keys and resume holds survive the track list growing, and their absence breaks nothing" << std::endl;
  return 0;
}

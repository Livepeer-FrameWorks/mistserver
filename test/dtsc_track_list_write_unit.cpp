// Processes share a live stream's track list. A process that adds a track to a full list replaces
// the list page with a larger copy, flagging the old page for reload before it copies it. A write
// another process makes to the old page is repeated on the replacement, so no write is lost and no
// process waits for a lock to write. Each case runs its processes step by step, so the list grows
// exactly where the case needs it.
#include <mist/defines.h>
#include <mist/dtsc.h>
#include <mist/shared_memory.h>
#include <mist/timing.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <set>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

namespace {
  int failures = 0;

  void expect(bool ok, const std::string & what) {
    if (!ok) {
      std::cerr << "FAIL: " << what << std::endl;
      ++failures;
    }
  }

  std::set<pid_t> trackOwners;

  /// A process attached to the stream that runs the steps it is sent, one at a time.
  class Worker {
    public:
      Worker(const std::string & streamName, const std::function<void(DTSC::Meta &, char)> & steps) {
        int cmd[2], ack[2];
        if (pipe(cmd) || pipe(ack)) { exit(2); }
        pid = fork();
        if (!pid) {
          close(cmd[1]);
          close(ack[0]);
          DTSC::Meta M(streamName, false, false);
          char step;
          while (read(cmd[0], &step, 1) == 1) {
            if (step == 'q') { _exit(0); }
            steps(M, step);
            if (write(ack[1], &step, 1) != 1) { _exit(2); }
          }
          _exit(0);
        }
        // Only the worker holds these ends, so a worker that died is noticed instead of waited for.
        close(cmd[0]);
        close(ack[1]);
        cmdFd = cmd[1];
        ackFd = ack[0];
        trackOwners.insert(pid);
      }
      ~Worker() {
        char quit = 'q';
        if (write(cmdFd, &quit, 1) != 1) {}
        close(cmdFd);
        close(ackFd);
        waitpid(pid, 0, 0);
      }
      void run(char step) {
        char done;
        if (write(cmdFd, &step, 1) != 1 || read(ackFd, &done, 1) != 1) { expect(false, "a worker runs its step"); }
      }
      pid_t pid;

    private:
      int cmdFd, ackFd;
  };

  size_t addDescribed(DTSC::Meta & M, const char *type, const char *codec) {
    const size_t idx = M.addTrack(DEFAULT_FRAGMENT_COUNT, DEFAULT_KEY_COUNT, DEFAULT_PART_COUNT, DEFAULT_PAGE_COUNT, false);
    M.setType(idx, type);
    M.setCodec(idx, codec);
    M.validateTrack(idx);
    return idx;
  }

  void removeStream(const std::string & streamName) {
    char name[NAME_BUFFER_SIZE];
    trackOwners.insert(getpid());
    for (pid_t owner : trackOwners) {
      for (size_t idx = 0; idx < 8 * DEFAULT_TRACK_COUNT; ++idx) {
        snprintf(name, NAME_BUFFER_SIZE, SHM_STREAM_TM, streamName.c_str(), (uint32_t)owner, idx);
        IPC::sharedPage page(name, 0, false, false);
        if (page) { page.master = true; }
      }
    }
    trackOwners.clear();
    snprintf(name, NAME_BUFFER_SIZE, SHM_STREAM_META, streamName.c_str());
    IPC::sharedPage meta(name, 0, false, false);
    if (meta) { meta.master = true; }
    snprintf(name, NAME_BUFFER_SIZE, SEM_TRACKLIST, streamName.c_str());
    IPC::semaphore trackLock(name, O_CREAT | O_RDWR, ACCESSPERMS, 1);
    trackLock.unlink();
  }

  /// A short stream name: shared memory names are limited to 31 characters on macOS.
  std::string streamFor(const char *testCase) {
    return std::string("tw") + testCase + std::to_string(getpid() % 100000);
  }

  // A producer describes the track it added after another process grew the list: on the list it
  // still has loaded, or after it reloaded the grown list while its track was not valid yet.
  void describeAfterGrowth(bool reloadFirst) {
    const std::string streamName = streamFor(reloadFirst ? "e" : "d");
    {
      DTSC::Meta buffer(streamName, true);
      buffer.setLive(true);
      for (int i = 0; i < DEFAULT_TRACK_COUNT - 1; ++i) { addDescribed(buffer, "audio", "AAC"); }
      static size_t added = INVALID_TRACK_ID;
      Worker producer(streamName, [](DTSC::Meta & M, char step) {
        if (step == 'a') {
          added = M.addTrack(DEFAULT_FRAGMENT_COUNT, DEFAULT_KEY_COUNT, DEFAULT_PART_COUNT, DEFAULT_PAGE_COUNT, false);
        }
        if (step == 'r') { M.reloadReplacedPagesIfNeeded(); }
        if (step == 'd') {
          M.setType(added, "video");
          M.setCodec(added, "H264");
          M.setID(added, 42);
          M.validateTrack(added);
        }
      });
      Worker grower(streamName, [](DTSC::Meta & M, char) { addDescribed(M, "video", "HEVC"); });
      producer.run('a'); // takes the last free record
      grower.run('g'); // grows the list
      if (reloadFirst) { producer.run('r'); } // keeps its own track that is not valid yet
      producer.run('d'); // describes its track
      buffer.reloadReplacedPagesIfNeeded();
      const size_t idx = DEFAULT_TRACK_COUNT - 1;
      expect(buffer.trackValid(idx) && buffer.getType(idx) == "video" && buffer.getCodec(idx) == "H264" && buffer.getID(idx) == 42,
             "a track described after another process grew the list keeps its description (valid " +
               std::to_string(buffer.trackValid(idx)) + ", type '" + buffer.getType(idx) + "', codec '" +
               buffer.getCodec(idx) + "')");
    }
    removeStream(streamName);
  }

  // A claimant that has not seen the list grow (twice) claims a released track; a second claimant
  // that has seen it must then find it taken.
  void claimOnStaleList() {
    const std::string streamName = streamFor("c");
    {
      DTSC::Meta buffer(streamName, true);
      buffer.setLive(true);
      for (int i = 0; i < DEFAULT_TRACK_COUNT; ++i) { addDescribed(buffer, "audio", "AAC"); }
      buffer.breakClaim(0);
      Worker first(streamName, [](DTSC::Meta & M, char step) {
        if (step == 'c' && !M.claimTrack(0, false)) { _exit(3); }
      });
      Worker grower(streamName, [](DTSC::Meta & M, char) {
        for (int i = 0; i < 2 * DEFAULT_TRACK_COUNT; ++i) { addDescribed(M, "meta", "JSON"); }
      });
      Worker second(streamName, [](DTSC::Meta & M, char step) {
        M.reloadReplacedPagesIfNeeded();
        if (step == 'c' && M.claimTrack(0, false)) { _exit(4); }
      });
      grower.run('g');
      first.run('c');
      second.run('c');
      buffer.reloadReplacedPagesIfNeeded();
      expect(buffer.isClaimedBy(0) == (uint64_t)first.pid,
             "a claim made before seeing the grown list holds, and nobody else gets the track");
    }
    removeStream(streamName);
  }

  // The buffer, which has not seen the list grow, releases a claim, removes a track and changes a
  // stream field.
  void bufferWritesAfterGrowth() {
    const std::string streamName = streamFor("b");
    {
      DTSC::Meta buffer(streamName, true);
      buffer.setLive(true);
      for (int i = 0; i < DEFAULT_TRACK_COUNT; ++i) { addDescribed(buffer, "audio", "AAC"); }
      Worker grower(streamName, [](DTSC::Meta & M, char) { addDescribed(M, "video", "HEVC"); });
      grower.run('g');
      buffer.breakClaim(0);
      buffer.removeTrack(1);
      buffer.setBufferWindow(12345);
      DTSC::Meta reader(streamName, false, false);
      expect(!reader.isClaimed(0), "a claim the buffer released on the replaced list stays released");
      expect(!reader.trackValid(1), "a track the buffer removed on the replaced list stays removed");
      expect(reader.getBufferWindow() == 12345, "a stream field the buffer wrote on the replaced list keeps its value");
      expect(reader.trackValid(DEFAULT_TRACK_COUNT), "the track that grew the list is valid");
    }
    removeStream(streamName);
  }

  // Two producers register three outputs each at the same time, adding then describing every
  // track while the other one grows the list. Every track ends up valid and described.
  void concurrentRegistration() {
    int incomplete = 0;
    for (int round = 0; round < 20; ++round) {
      const std::string streamName = streamFor("r");
      {
        DTSC::Meta buffer(streamName, true);
        buffer.setLive(true);
        addDescribed(buffer, "video", "H264");
        addDescribed(buffer, "audio", "AAC");
        int go[2];
        if (pipe(go)) { exit(2); }
        pid_t producers[2];
        for (pid_t & producer : producers) {
          producer = fork();
          if (!producer) {
            srand(getpid());
            DTSC::Meta M(streamName, false, false);
            char start;
            if (read(go[0], &start, 1) != 1) { _exit(2); }
            for (int k = 0; k < 3; ++k) {
              const size_t idx =
                M.addTrack(DEFAULT_FRAGMENT_COUNT, DEFAULT_KEY_COUNT, DEFAULT_PART_COUNT, DEFAULT_PAGE_COUNT, false);
              usleep(rand() % 3000);
              M.setType(idx, "video");
              M.setCodec(idx, "H264");
              M.validateTrack(idx);
            }
            _exit(0);
          }
          trackOwners.insert(producer);
        }
        if (write(go[1], "gg", 2) != 2) { exit(2); }
        for (pid_t producer : producers) { waitpid(producer, 0, 0); }
        close(go[0]);
        close(go[1]);
        buffer.reloadReplacedPagesIfNeeded();
        for (size_t idx = 0; idx < 8; ++idx) {
          if (!buffer.trackValid(idx) || buffer.getType(idx).empty() || buffer.getCodec(idx).empty()) { ++incomplete; }
        }
      }
      removeStream(streamName);
    }
    expect(!incomplete, std::to_string(incomplete) + " of 160 concurrently registered tracks ended up invalid or undescribed");
  }

  // The process growing the list dies after flagging the page for reload, before the replacement
  // exists. A write waits for the replacement at most a second, and only once.
  void replacementNeverComes() {
    const std::string streamName = streamFor("n");
    {
      DTSC::Meta buffer(streamName, true);
      buffer.setLive(true);
      addDescribed(buffer, "video", "H264");
      DTSC::Meta writer(streamName, false, false);
      char name[NAME_BUFFER_SIZE];
      snprintf(name, NAME_BUFFER_SIZE, SHM_STREAM_META, streamName.c_str());
      {
        IPC::sharedPage page(name, 0, false, false);
        Util::RelAccX(page.mapped, false).setReload();
        page.master = true; // unlinked on close: no replacement under its name
      }
      uint64_t start = Util::bootMS();
      writer.setSource("push://first");
      const uint64_t first = Util::bootMS() - start;
      start = Util::bootMS();
      writer.setSource("push://second");
      const uint64_t second = Util::bootMS() - start;
      expect(first >= 900 && first < 2000,
             "a write waits about a second for a replacement that never comes (waited " + std::to_string(first) + " ms)");
      expect(second < 100, "a later write does not wait again (waited " + std::to_string(second) + " ms)");
      expect(writer.getSource() == "push://second", "the writes land on the page the writer has");
    }
    removeStream(streamName);
  }
  // A write gives up on a replacement that never came for its page. That holds for that page only:
  // a write on the page that replaced it later is repeated on that page's own replacement, also
  // when the later page is mapped where the earlier one was.
  void givenUpForOnePageOnly() {
    const std::string streamName = streamFor("g");
    {
      DTSC::Meta buffer(streamName, true);
      buffer.setLive(true);
      for (int i = 0; i < DEFAULT_TRACK_COUNT; ++i) { addDescribed(buffer, "audio", "AAC"); }
      DTSC::Meta writer(streamName, false, false);
      char name[NAME_BUFFER_SIZE];
      snprintf(name, NAME_BUFFER_SIZE, SHM_STREAM_META, streamName.c_str());
      std::string copy;
      {
        IPC::sharedPage page(name, 0, false, false);
        copy.assign(page.mapped, page.len);
        Util::RelAccX(page.mapped, false).setReload();
      }
      writer.setSource("push://first"); // no replacement within a second: gives up on this page
      {
        IPC::sharedPage flagged(name, 0, false, false);
        flagged.master = true; // unlinked on close
      }
      {
        IPC::sharedPage replacement(name, copy.size(), true, false);
        memcpy(replacement.mapped, copy.data(), copy.size());
        replacement.master = false;
      }
      writer.reloadReplacedPagesIfNeeded(); // maps the replacement, likely where the flagged page was
      Worker grower(streamName, [](DTSC::Meta & M, char) { addDescribed(M, "video", "HEVC"); });
      grower.run('g'); // the list is full: grows it
      const uint64_t start = Util::bootMS();
      writer.setBufferWindow(777);
      const uint64_t waited = Util::bootMS() - start;
      DTSC::Meta reader(streamName, false, false);
      expect(reader.getBufferWindow() == 777, "a write after a given up replacement lands on the next page's replacement");
      expect(waited < 900,
             "that write did not wait for a replacement that was already there (waited " + std::to_string(waited) + " ms)");
    }
    removeStream(streamName);
  }

  // A process keeps its metadata across the stream's next generation. The track list lock it then
  // takes is the next generation's: another process holding that lock keeps it from claiming.
  void lockAcrossGenerations() {
    const std::string streamName = streamFor("l");
    {
      DTSC::Meta *first = new DTSC::Meta(streamName, true);
      first->setLive(true);
      addDescribed(*first, "video", "H264");
      first->breakClaim(0);
      DTSC::Meta claimant(streamName, false, false);
      expect(claimant.claimTrack(0, false), "the claimant claims a track of the first generation");
      delete first; // the first generation ends, its lock goes with it

      DTSC::Meta second(streamName, true);
      second.setLive(true);
      addDescribed(second, "video", "H264");
      second.breakClaim(0);
      claimant.reloadReplacedPagesIfNeeded(); // follows the stream to its next generation

      int held[2];
      if (pipe(held)) { exit(2); }
      pid_t holder = fork();
      if (!holder) {
        char name[NAME_BUFFER_SIZE];
        snprintf(name, NAME_BUFFER_SIZE, SEM_TRACKLIST, streamName.c_str());
        IPC::semaphore lock(name, O_CREAT | O_RDWR, ACCESSPERMS, 1);
        lock.wait();
        if (write(held[1], "h", 1) != 1) { _exit(2); }
        Util::sleep(1000);
        lock.post();
        _exit(0);
      }
      char ack;
      if (read(held[0], &ack, 1) != 1) { expect(false, "the holder takes the lock"); }
      const uint64_t start = Util::bootMS();
      const bool claimed = claimant.claimTrack(0, false);
      const uint64_t waited = Util::bootMS() - start;
      waitpid(holder, 0, 0);
      close(held[0]);
      close(held[1]);
      expect(waited >= 800,
             "a claim waits for the next generation's lock another process holds (waited " + std::to_string(waited) + " ms)");
      expect(claimed, "the claim succeeds once that lock is released");
    }
    removeStream(streamName);
  }

  // A producer removes a track of its own. Its record no longer names it: a reload keeps a
  // process's own tracks that are not valid (yet), and must not load the removed one again.
  void removedOwnTrack() {
    const std::string streamName = streamFor("o");
    {
      DTSC::Meta buffer(streamName, true);
      buffer.setLive(true);
      addDescribed(buffer, "audio", "AAC");
      static size_t added = INVALID_TRACK_ID;
      Worker producer(streamName, [](DTSC::Meta & M, char step) {
        if (step == 'a') { added = addDescribed(M, "video", "H264"); }
        if (step == 'r') { M.removeTrack(added); }
      });
      producer.run('a');
      producer.run('r');
      buffer.reloadReplacedPagesIfNeeded();
      expect(!buffer.trackValid(1), "the removed track is not valid");
      expect(!buffer.isClaimed(1),
             "the removed track's record no longer names its producer (claimed by " + std::to_string(buffer.isClaimedBy(1)) + ")");
    }
    removeStream(streamName);
  }
} // namespace

int main() {
  signal(SIGPIPE, SIG_IGN); // a worker that died is reported, not fatal
  describeAfterGrowth(false);
  describeAfterGrowth(true);
  claimOnStaleList();
  bufferWritesAfterGrowth();
  concurrentRegistration();
  replacementNeverComes();
  givenUpForOnePageOnly();
  lockAcrossGenerations();
  removedOwnTrack();
  if (failures) { return 1; }
  std::cout << "every write to a shared track list survives another process growing it" << std::endl;
  return 0;
}

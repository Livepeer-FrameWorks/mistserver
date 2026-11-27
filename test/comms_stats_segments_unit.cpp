#include <mist/comms.h>
#include <mist/defines.h>
#include <mist/shared_memory.h>
#include <mist/timing.h>
#include <mist/util.h>

#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <semaphore.h>
#include <string>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

// Statistics segments across rolling controller restarts. A session started by a newer release
// must register with every field of its layout, sessions of the previous layout stay visible to the
// controller until they end, and drained segments disappear.
namespace {
  int failures = 0;

  void check(bool ok, const std::string & what) {
    if (ok) {
      std::cout << "ok   " << what << std::endl;
      return;
    }
    std::cerr << "FAIL " << what << std::endl;
    ++failures;
  }

  void unlinkPage(const std::string & name) {
    IPC::sharedPage page(name, 0, false, false);
    if (page) { page.master = true; }
  }

  bool pageExists(const std::string & name) {
    IPC::sharedPage page(name, 0, false, false);
    return page;
  }

  // A session that died while registering leaves the statistics lock taken; start from none, and
  // leave no segment or index behind for the next test.
  void cleanup() {
    sem_unlink(SEM_STATISTICS);
    Comms::StatsIndex idx;
    if (Comms::readStatsIndex(idx)) {
      for (const Comms::StatsSegment & seg : idx.segments) { unlinkPage(seg.name); }
    }
    for (int i = 0; i < 64; ++i) { unlinkPage(std::string(COMMS_STATISTICS) + "." + std::to_string(i)); }
    unlinkPage(COMMS_STATISTICS);
    unlinkPage(SHM_STATS_INDEX);
  }

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

  /// Creates a statistics page with the previous release's layout, as that release's controller did.
  uint64_t createPreviousReleasePage(const std::string & name) {
    IPC::sharedPage page(name, COMMS_STATISTICS_INITSIZE, true);
    page.master = false;
    Util::RelAccX stats(page.mapped, false);
    addPreviousReleaseSessionFields(stats);
    stats.setRCount((COMMS_STATISTICS_INITSIZE - stats.getOffset()) / stats.getRSize());
    stats.setEndPos(1);
    stats.setPresent(0);
    stats.setReady();
    return Comms::layoutFingerprint(page.mapped, page.len);
  }

  /// A MistSession stand-in: registers like MistSession does, reports where, and stays registered
  /// until `stop` is called.
  struct Client {
      pid_t pid = 0;
      int release = -1;
      std::string page;
      bool registered = false;

      void start(const std::string & sessId, const std::string & requested = "") {
        int ready[2], hold[2];
        if (pipe(ready) || pipe(hold)) { std::abort(); }
        pid = fork();
        if (pid == 0) {
          close(ready[0]);
          close(hold[1]);
          Comms::Sessions session;
          session.reload();
          std::string where = session ? session.pageName() : std::string();
          if (session) {
            session.setHost(std::string(16, '\0'));
            session.setSessId(sessId);
            session.setStream("segments");
            session.setRequestedStream(requested);
            session.setNow(Util::bootSecs());
          }
          where += "\n";
          if (write(ready[1], where.data(), where.size()) < 0) { _exit(4); }
          char c;
          while (read(hold[0], &c, 1) < 0 && errno == EINTR) {}
          _exit(0);
        }
        close(ready[1]);
        close(hold[0]);
        release = hold[1];
        char buf[128];
        std::string got;
        ssize_t r;
        while ((r = read(ready[0], buf, sizeof(buf))) != 0) {
          if (r < 0 && errno == EINTR) { continue; }
          if (r < 0) { break; }
          got.append(buf, r);
          if (got.find('\n') != std::string::npos) { break; }
        }
        close(ready[0]);
        page = got.substr(0, got.find('\n'));
        registered = page.size();
      }

      void stop() {
        if (!pid) { return; }
        close(release);
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        pid = 0;
      }
  };

  /// The segment of `segs` and record holding `sessId`, or false.
  bool findSession(Comms::StatisticsSegments & segs, const std::string & sessId, size_t & segment, size_t & record) {
    for (segment = 0; segment < segs.size(); ++segment) {
      Comms::Sessions & seg = segs[segment];
      for (record = 0; record < seg.recordCount(); ++record) {
        const uint8_t s = seg.getStatus(record);
        if (s == COMM_STATUS_INVALID || (s & COMM_STATUS_DISCONNECT)) { continue; }
        if (seg.getSessId(record) == sessId) { return true; }
      }
    }
    return false;
  }

  /// Runs the controller's collection pass over every segment, then its retirement step.
  void collect(Comms::StatisticsSegments & segs, uint64_t nowMs) {
    for (size_t s = 0; s < segs.size(); ++s) {
      Comms::Sessions & seg = segs[s];
      COMM_LOOP(seg, (void)0, (void)0);
    }
    segs.retire(nowMs);
  }

  bool segmentListed(const std::string & name, uint32_t state) {
    Comms::StatsIndex idx;
    if (!Comms::readStatsIndex(idx)) { return false; }
    for (const Comms::StatsSegment & seg : idx.segments) {
      if (seg.name == name && seg.state == state) { return true; }
    }
    return false;
  }

  bool hasSegment(Comms::StatisticsSegments & segs, const std::string & name) {
    for (size_t s = 0; s < segs.size(); ++s) {
      if (segs[s].pageName() == name) { return true; }
    }
    return false;
  }

  // 1. A controller start next to the previous release's page.
  void legacyStart() {
    createPreviousReleasePage(COMMS_STATISTICS);
    Client legacy;
    legacy.start("Ilegacysession");
    check(legacy.registered && legacy.page == COMMS_STATISTICS, "a session without an index registers on the legacy page");
    {
      Comms::StatisticsSegments segs;
      segs.start();
      const std::string current = segs.size() ? segs[0].pageName() : "";
      check(current.size() > strlen(COMMS_STATISTICS) && current != COMMS_STATISTICS,
            "the controller creates a new current segment instead of reusing the legacy page (" + current + ")");
      Comms::StatsIndex idx;
      check(Comms::readStatsIndex(idx) && idx.current() && idx.current()->name == current &&
              idx.current()->fingerprint == Comms::Sessions::buildFingerprint(),
            "the index publishes the current segment with this build's layout");
      check(segmentListed(COMMS_STATISTICS, Comms::STATS_SEGMENT_RETIRING), "the index lists the legacy page as retiring");
      size_t seg = 0, rec = 0;
      check(findSession(segs, "Ilegacysession", seg, rec) && segs[seg].pageName() == COMMS_STATISTICS,
            "the legacy session stays visible through the segments");

      Client fresh;
      fresh.start("Ifreshsession", "alias-stream");
      check(fresh.registered && fresh.page == current, "a new session registers on the current segment");
      check(findSession(segs, "Ifreshsession", seg, rec) && seg == 0 && segs[0].getRequestedStream(rec) == "alias-stream",
            "the controller reads the requested stream a new session stored");
      fresh.stop();
      legacy.stop();
      segs.stop(false, false);
    }
    check(!pageExists(SHM_STATS_INDEX), "a full shutdown unlinks the index");
  }

  // 2. A drained retiring segment closes registration and is removed after the grace period.
  void retireDrained() {
    createPreviousReleasePage(COMMS_STATISTICS);
    Client legacy;
    legacy.start("Ilegacydrain");
    Comms::StatisticsSegments segs;
    segs.graceMs = 300;
    segs.start();
    collect(segs, Util::bootMS());
    {
      IPC::sharedPage page(COMMS_STATISTICS, 0, false, false);
      Util::RelAccX rax(page.mapped, false);
      check(page && !rax.isExit(), "a retiring segment with a live session keeps registration open");
    }
    legacy.stop();
    // The first pass marks the ended session's record disconnected, the second one collects it.
    const uint64_t closedAt = Util::bootMS();
    collect(segs, closedAt);
    collect(segs, closedAt);
    {
      IPC::sharedPage page(COMMS_STATISTICS, 0, false, false);
      Util::RelAccX rax(page.mapped, false);
      check(page && rax.isExit(), "a drained retiring segment closes registration");
    }
    check(segs.size() == 2 && segmentListed(COMMS_STATISTICS, Comms::STATS_SEGMENT_RETIRING),
          "a closed segment stays listed during the grace period");
    collect(segs, closedAt + 100);
    check(segs.size() == 2 && pageExists(COMMS_STATISTICS), "the closed segment survives until the grace period ends");
    collect(segs, closedAt + 300);
    check(segs.size() == 1 && !pageExists(COMMS_STATISTICS) && !segmentListed(COMMS_STATISTICS, Comms::STATS_SEGMENT_RETIRING),
          "after the grace period the segment leaves the index and is unlinked");
    check(pageExists(segs[0].pageName()), "the current segment remains");
    segs.stop(false, false);
  }

  // 3. A rolling restart finds the current and every retiring segment again through the index.
  void rollingRestart() {
    const std::string previous = std::string(COMMS_STATISTICS) + ".7";
    const uint64_t previousLayout = createPreviousReleasePage(previous);
    {
      IPC::semaphore sem(SEM_STATISTICS, O_CREAT | O_RDWR, ACCESSPERMS, 1);
      IPC::sharedPage indexPage(SHM_STATS_INDEX, SHM_STATS_INDEX_SIZE, true);
      indexPage.master = false;
      Comms::StatsSegment seg;
      seg.name = previous;
      seg.state = Comms::STATS_SEGMENT_CURRENT;
      seg.fingerprint = previousLayout;
      std::vector<Comms::StatsSegment> list(1, seg);
      sem.wait();
      Comms::writeStatsIndex(indexPage.mapped, indexPage.len, list);
      sem.post();
    }
    Client old;
    old.start("Ioldlayout");
    check(old.registered && old.page == previous, "a session registers on the segment the index names current");
    std::string current;
    {
      Comms::StatisticsSegments first;
      first.start();
      current = first.size() ? first[0].pageName() : "";
      check(current.size() && current != previous, "a current segment with another layout is replaced");
      check(segmentListed(previous, Comms::STATS_SEGMENT_RETIRING), "the replaced segment is listed as retiring");
      first.stop(true, false);
    }
    check(pageExists(current) && pageExists(previous) && pageExists(SHM_STATS_INDEX),
          "a rolling restart keeps every segment and the index");
    {
      Comms::StatisticsSegments second;
      second.start();
      check(second.size() == 2 && second[0].pageName() == current, "the next controller attaches the same current segment");
      check(hasSegment(second, previous), "the next controller attaches the retiring segment the index lists");
      size_t seg = 0, rec = 0;
      check(findSession(second, "Ioldlayout", seg, rec) && second[seg].pageName() == previous,
            "the retiring segment's live session is visible again");
      old.stop();
      second.stop(false, false);
    }
    check(!pageExists(current) && !pageExists(previous), "a full shutdown unlinks every segment");
  }

  // 4. Before the controller publishes an index, sessions use the legacy page.
  void noIndex() {
    createPreviousReleasePage(COMMS_STATISTICS);
    Comms::Sessions session;
    session.reload();
    check(session && session.pageName() == COMMS_STATISTICS, "without an index a session registers on the legacy page");
    session.unload();
    unlinkPage(COMMS_STATISTICS);
  }

  // 5. A session that read the index just before the controller replaced and closed that segment.
  void closedRegistration() {
    const std::string closed = std::string(COMMS_STATISTICS) + ".3";
    const std::string next = std::string(COMMS_STATISTICS) + ".5";
    {
      createPreviousReleasePage(closed);
      IPC::sharedPage page(closed, 0, false, false);
      Util::RelAccX(page.mapped, false).setExit();
    }
    // A segment with this build's layout.
    IPC::sharedPage nextPage(next, COMMS_STATISTICS_INITSIZE, true);
    {
      Util::RelAccX rax(nextPage.mapped, false);
      addPreviousReleaseSessionFields(rax);
      rax.addField("requestedstream", RAX_STRING, 100);
      rax.setRCount((COMMS_STATISTICS_INITSIZE - rax.getOffset()) / rax.getRSize());
      rax.setEndPos(1);
      rax.setPresent(0);
      rax.setReady();
    }
    IPC::sharedPage indexPage(SHM_STATS_INDEX, SHM_STATS_INDEX_SIZE, true);
    Comms::StatsSegment seg;
    seg.name = closed;
    seg.state = Comms::STATS_SEGMENT_CURRENT;
    seg.fingerprint = 1;
    std::vector<Comms::StatsSegment> list(1, seg);
    Comms::writeStatsIndex(indexPage.mapped, indexPage.len, list);
    pid_t publisher = fork();
    if (publisher == 0) {
      usleep(30000);
      Comms::StatsSegment cur;
      cur.name = next;
      cur.state = Comms::STATS_SEGMENT_CURRENT;
      cur.fingerprint = Comms::Sessions::buildFingerprint();
      std::vector<Comms::StatsSegment> now(1, cur);
      now.push_back(seg);
      now.back().state = Comms::STATS_SEGMENT_RETIRING;
      Comms::writeStatsIndex(indexPage.mapped, indexPage.len, now);
      _exit(0);
    }
    {
      Comms::Sessions session;
      session.reload();
      check(session && session.pageName() == next,
            "a session refused by a closed segment registers on the segment the index names next (" + session.pageName() + ")");
      session.unload();
    }
    int status = 0;
    while (waitpid(publisher, &status, 0) < 0 && errno == EINTR) {}
    indexPage.master = true;
    nextPage.master = true;
    unlinkPage(closed);
  }

  void writeHeader(char *page, uint32_t magic, uint32_t version, uint64_t generation, uint32_t count, uint32_t entrySize) {
    memcpy(page, &magic, 4);
    memcpy(page + 4, &version, 4);
    memcpy(page + 8, &generation, 8);
    memcpy(page + 16, &count, 4);
    memcpy(page + 20, &entrySize, 4);
  }

  // 6. The index rejects what it cannot validate and never returns a torn snapshot.
  void indexRobustness() {
    std::vector<char> buf(SHM_STATS_INDEX_SIZE, 0);
    char *page = buf.data();
    Comms::StatsIndex idx;
    check(!Comms::readStatsIndex(page, buf.size(), idx), "an all-zero page is no index");

    std::vector<Comms::StatsSegment> list;
    Comms::StatsSegment a;
    a.name = "/MstStat.4";
    a.state = Comms::STATS_SEGMENT_CURRENT;
    a.fingerprint = 42;
    Comms::StatsSegment b;
    b.name = COMMS_STATISTICS;
    b.state = Comms::STATS_SEGMENT_RETIRING;
    b.fingerprint = 7;
    list.push_back(a);
    list.push_back(b);
    check(Comms::writeStatsIndex(page, buf.size(), list), "the index stores valid entries");
    check(Comms::readStatsIndex(page, buf.size(), idx) && idx.segments.size() == 2 && idx.current() &&
            idx.current()->name == "/MstStat.4" && idx.current()->fingerprint == 42 && idx.generation == 2,
          "the index reads back what was written");

    std::vector<char> saved(buf);
    writeHeader(page, Comms::STATS_INDEX_MAGIC, Comms::STATS_INDEX_VERSION, 2, 0xFFFFFFFF, Comms::STATS_INDEX_ENTRY_SIZE);
    check(!Comms::readStatsIndex(page, buf.size(), idx), "an entry count beyond the page is rejected");
    writeHeader(page, Comms::STATS_INDEX_MAGIC, Comms::STATS_INDEX_VERSION, 2, 2, 0xFFFFFFF0);
    check(!Comms::readStatsIndex(page, buf.size(), idx), "an entry size beyond the page is rejected");
    writeHeader(page, Comms::STATS_INDEX_MAGIC, Comms::STATS_INDEX_VERSION, 2, 2, 8);
    check(!Comms::readStatsIndex(page, buf.size(), idx), "an entry size below the entry fields is rejected");
    writeHeader(page, Comms::STATS_INDEX_MAGIC, Comms::STATS_INDEX_VERSION + 1, 2, 2, Comms::STATS_INDEX_ENTRY_SIZE);
    check(!Comms::readStatsIndex(page, buf.size(), idx), "an unknown format version is rejected");
    check(!Comms::readStatsIndex(page, 16, idx), "a page shorter than the header is rejected");

    buf = saved;
    page = buf.data();
    char *first = page + Comms::STATS_INDEX_HEADER_SIZE;
    memset(first, 'x', Comms::STATS_INDEX_NAME_SIZE);
    check(Comms::readStatsIndex(page, buf.size(), idx) && idx.segments.size() == 1 && !idx.current() && idx.segments[0].name == COMMS_STATISTICS,
          "an unterminated entry name is skipped");
    strcpy(first, "/dev/shm/../etc");
    check(Comms::readStatsIndex(page, buf.size(), idx) && idx.segments.size() == 1, "a name outside the statistics pages is skipped");
    strcpy(first, "/MstStat.4");
    uint32_t badState = 9;
    memcpy(first + 64, &badState, 4);
    check(Comms::readStatsIndex(page, buf.size(), idx) && idx.segments.size() == 1, "an unknown entry state is skipped");

    std::vector<Comms::StatsSegment> tooMany(60, a);
    check(!Comms::writeStatsIndex(page, buf.size(), tooMany), "entries beyond the page are refused");
    std::vector<Comms::StatsSegment> badName(1, a);
    badName[0].name = "/MstStatX";
    check(!Comms::writeStatsIndex(page, buf.size(), badName), "an invalid segment name is refused");
    check(!Comms::layoutFingerprint(page, 8) && !Comms::layoutFingerprint(std::vector<char>(64, 0).data(), 64),
          "a page without a field table has no fingerprint");

    // A writer mid-change: the reader retries until the generation is even again.
    char *shared = (char *)mmap(0, SHM_STATS_INDEX_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    memcpy(shared, saved.data(), SHM_STATS_INDEX_SIZE);
    uint64_t odd = 3;
    memcpy(shared + 8, &odd, 8);
    check(!Comms::readStatsIndex(shared, SHM_STATS_INDEX_SIZE, idx, 5), "a reader gives up on an index that stays mid-write");
    pid_t finisher = fork();
    if (finisher == 0) {
      usleep(50000);
      uint64_t even = 4;
      __atomic_store_n((uint64_t *)(shared + 8), even, __ATOMIC_RELEASE);
      _exit(0);
    }
    check(Comms::readStatsIndex(shared, SHM_STATS_INDEX_SIZE, idx) && idx.generation == 4 && idx.segments.size() == 2,
          "a reader retries until the writer finishes");
    int status = 0;
    while (waitpid(finisher, &status, 0) < 0 && errno == EINTR) {}

    // A writer that keeps changing the index: every snapshot holds one complete write, where
    // each entry's fingerprint is the entry count of that write.
    memset(shared, 0, SHM_STATS_INDEX_SIZE);
    pid_t writer = fork();
    if (writer == 0) {
      for (uint64_t round = 0; round < 200000; ++round) {
        const size_t n = 1 + round % 40;
        std::vector<Comms::StatsSegment> entries;
        for (size_t i = 0; i < n; ++i) {
          Comms::StatsSegment e;
          e.name = std::string(COMMS_STATISTICS) + "." + std::to_string(round * 41 + i);
          e.state = i ? Comms::STATS_SEGMENT_RETIRING : Comms::STATS_SEGMENT_CURRENT;
          e.fingerprint = n;
          entries.push_back(e);
        }
        Comms::writeStatsIndex(shared, SHM_STATS_INDEX_SIZE, entries);
        if (__atomic_load_n((uint64_t *)(shared + SHM_STATS_INDEX_SIZE - 8), __ATOMIC_ACQUIRE)) { break; }
      }
      _exit(0);
    }
    size_t torn = 0, reads = 0, changed = 0;
    uint64_t lastGeneration = 0;
    const uint64_t until = Util::bootMS() + 1500;
    while (Util::bootMS() < until) {
      if (!Comms::readStatsIndex(shared, SHM_STATS_INDEX_SIZE, idx)) { continue; }
      ++reads;
      if (idx.generation != lastGeneration) { ++changed; }
      lastGeneration = idx.generation;
      bool whole = idx.segments.size() && !(idx.generation & 1);
      for (const Comms::StatsSegment & e : idx.segments) { whole &= (e.fingerprint == idx.segments.size()); }
      if (!whole) { ++torn; }
    }
    __atomic_store_n((uint64_t *)(shared + SHM_STATS_INDEX_SIZE - 8), (uint64_t)1, __ATOMIC_RELEASE);
    while (waitpid(writer, &status, 0) < 0 && errno == EINTR) {}
    check(reads > 100 && changed > 10 && !torn,
          "concurrent index changes never yield a torn snapshot (" + std::to_string(reads) + " reads, " +
            std::to_string(changed) + " generations, " + std::to_string(torn) + " torn)");
    munmap(shared, SHM_STATS_INDEX_SIZE);
  }

  // Fixture modes for controller_stats_segments_integration.sh, which runs a real controller.

  /// The pages a previous release's controller leaves behind: a statistics page and a streams
  /// state page, both without the fields this build added.
  int seedPreviousRelease() {
    createPreviousReleasePage(COMMS_STATISTICS);
    IPC::sharedPage streams(SHM_STATE_STREAMS, 5 * 1024 * 1024, true);
    streams.master = false;
    Util::RelAccX rax(streams.mapped, false);
    rax.addField("stream", RAX_128STRING);
    rax.addField("status", RAX_UINT, 1);
    rax.addField("viewers", RAX_64UINT);
    rax.addField("inputs", RAX_64UINT);
    rax.addField("outputs", RAX_64UINT);
    rax.addField("tags", RAX_512STRING);
    rax.setRCount(16);
    rax.setReady();
    return 0;
  }

  volatile sig_atomic_t holdStop = 0;
  void onHoldSignal(int) {
    holdStop = 1;
  }

  /// Registers a session the way MistSession does, prints the page it registered on, and keeps
  /// the record fresh until SIGTERM.
  int holdSession(const std::string & sessId, const std::string & requested) {
    signal(SIGTERM, onHoldSignal);
    Comms::Sessions session;
    session.reload();
    if (!session) {
      std::cout << "unregistered" << std::endl;
      return 1;
    }
    session.setHost(std::string(16, '\0'));
    session.setSessId(sessId);
    session.setStream("segments");
    session.setConnector("HLS");
    session.setRequestedStream(requested);
    std::cout << session.pageName() << std::endl;
    while (!holdStop) {
      session.setNow(Util::bootSecs());
      Util::sleep(200);
    }
    session.unload();
    return 0;
  }

  /// Checks what a controller start beside the previous release's pages must leave: a current
  /// full-layout segment in the index, the legacy page listed as retiring, and a streams state
  /// page with this build's fields. Prints the current segment's name.
  int checkStarted() {
    Comms::StatsIndex idx;
    check(Comms::readStatsIndex(idx), "the controller publishes a statistics index");
    const Comms::StatsSegment *current = idx.current();
    check(current && current->name != COMMS_STATISTICS && current->fingerprint == Comms::Sessions::buildFingerprint(),
          "the index names a current segment with this build's layout");
    check(segmentListed(COMMS_STATISTICS, Comms::STATS_SEGMENT_RETIRING), "the index lists the legacy page as retiring");
    if (current) {
      IPC::sharedPage page(current->name, 0, false, false);
      Util::RelAccX rax(page.mapped, false);
      check(page && rax.isReady() && rax.hasField("requestedstream"), "the current segment stores requested streams");
    }
    IPC::sharedPage streams(SHM_STATE_STREAMS, 0, false, false);
    Util::RelAccX rax(streams.mapped, false);
    check(streams && rax.isReady() && rax.hasField("unspecified"), "the streams state page has this build's fields");
    std::cout << "current " << (current ? current->name : std::string("none")) << std::endl;
    return failures ? 1 : 0;
  }

  /// The session `sessId` is on `pageName` with `requested` stored.
  int checkSession(const std::string & pageName, const std::string & sessId, const std::string & requested) {
    IPC::sharedPage page(pageName, 0, false, false);
    Util::RelAccX rax(page.mapped, false);
    bool found = false;
    for (uint64_t i = 0; page && i < rax.getEndPos(); ++i) {
      if ((rax.getInt("status", i) & COMM_STATUS_ACTIVE) && std::string(rax.getPointer("sessid", i)) == sessId) {
        found = !rax.hasField("requestedstream") ? requested.empty() : std::string(rax.getPointer("requestedstream", i)) == requested;
      }
    }
    check(found, "session " + sessId + " is registered on " + pageName + " with requested stream '" + requested + "'");
    return failures ? 1 : 0;
  }

  int checkLegacyClosed() {
    IPC::sharedPage page(COMMS_STATISTICS, 0, false, false);
    Util::RelAccX rax(page.mapped, false);
    check(page && rax.isExit(), "the controller closes registration on the drained legacy page");
    return failures ? 1 : 0;
  }

  int checkShutdown(const std::string & current) {
    check(!pageExists(SHM_STATS_INDEX), "a full controller shutdown unlinks the statistics index");
    check(!pageExists(current), "a full controller shutdown unlinks the current segment");
    return failures ? 1 : 0;
  }
} // namespace

int main(int argc, char **argv) {
  const std::string mode = argc > 1 ? argv[1] : "";
  if (mode == "clean") {
    cleanup();
    unlinkPage(SHM_STATE_STREAMS);
    return 0;
  }
  if (mode == "seed-previous-release") { return seedPreviousRelease(); }
  if (mode == "hold" && argc > 3) { return holdSession(argv[2], argv[3]); }
  if (mode == "check-started") { return checkStarted(); }
  if (mode == "check-session" && argc > 4) { return checkSession(argv[2], argv[3], argv[4]); }
  if (mode == "check-legacy-closed") { return checkLegacyClosed(); }
  if (mode == "check-shutdown" && argc > 2) { return checkShutdown(argv[2]); }
  cleanup();
  if (mode == "legacy-start") {
    legacyStart();
  } else if (mode == "retire") {
    retireDrained();
  } else if (mode == "restart") {
    rollingRestart();
  } else if (mode == "no-index") {
    noIndex();
  } else if (mode == "closed") {
    closedRegistration();
  } else if (mode == "index") {
    indexRobustness();
  } else {
    std::cerr << "usage: " << argv[0] << " legacy-start|retire|restart|no-index|closed|index" << std::endl;
    return 2;
  }
  cleanup();
  return failures ? 1 : 0;
}

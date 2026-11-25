#include "../src/output/output.h"

#include <mist/defines.h>
#include <mist/shared_memory.h>
#include <mist/timing.h>

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>

// A reader of a process-controlled live stream waits for data the buffer withholds while it
// pauses the source feed. The pause must not use up the reader's data-wait budget: once the feed
// resumes, the reader waits the full data-wait timeout for the next packet before it gives up.

namespace {
  const uint64_t dataWait = 4000;
  const char payload[16] = {0};

  class LiveWriter : public Mist::InOutBase {
    public:
      DTSC::Meta live;
      size_t track;

      explicit LiveWriter(const std::string & name) {
        streamName = name;
        standAlone = true;
        live.reInit(name, true);
        live.setLive(true);
        track = live.addTrack();
        live.setID(track, 1);
        live.setType(track, "video");
        live.setCodec(track, "H264");
        live.setWidth(track, 320);
        live.setHeight(track, 180);
      }

      void put(uint64_t time, bool key) { bufferLivePacket(time, 0, track, payload, sizeof(payload), 0, key, live); }
  };

  class PauseReader : public Mist::Output {
    public:
      PauseReader(Socket::Connection & conn, Util::Config & cfg, JSON::Value & capa) : Mist::Output(conn, cfg, capa) {}
      void sendNext() override {}
      // No viewer connection: the reader runs for as long as the test steps it.
      bool keepGoing() override { return true; }
      void stats(bool) override {}

      void attach(const std::string & name) {
        streamName = name;
        meta.reInit(name, false);
        isInitialized = true;
        targetParams["video"] = "all";
        targetParams["audio"] = "none";
        targetParams["subtitle"] = "none";
        targetParams["meta"] = "none";
        selectDefaultTracks();
        seek(uint64_t(0));
        dataWaitTimeout = dataWait;
      }

      bool reading() const { return userSelect.size(); }

      /// Steps the reader until it returns a packet (true) or waits for data (false).
      bool next(uint64_t & time) {
        for (int tries = 0; tries < 100; ++tries) {
          thisBootMs = Util::bootMS();
          const size_t wait = prepareNext();
          if (!reading()) { return false; }
          if (!wait && thisPacket) {
            time = thisTime;
            return true;
          }
          if (wait > 1) { return false; }
        }
        return false;
      }

      /// Keeps a waiting reader polling for durationMs, as its wait loop would.
      bool waitFor(uint64_t durationMs) {
        const uint64_t end = Util::bootMS() + durationMs;
        while (Util::bootMS() < end) {
          uint64_t time = 0;
          if (next(time)) {
            fprintf(stderr, "reader returned a packet at %" PRIu64 " ms while no data was buffered\n", time);
            return false;
          }
          if (!reading()) { return false; }
          Util::sleep(100);
        }
        return true;
      }
  };

  void writeStreamConfig(const std::string & name, IPC::sharedPage & page) {
    JSON::Value conf;
    conf["name"] = name;
    conf["process_controlled_realtime"] = true;
    const std::string packed = conf.toPacked();
    char pageName[NAME_BUFFER_SIZE];
    snprintf(pageName, NAME_BUFFER_SIZE, SHM_STREAM_CONF, name.c_str());
    page.init(pageName, packed.size() + 100, true, false);
    Util::RelAccX rax(page.mapped, false);
    rax.addField("dtsc_data", RAX_DTSC, packed.size());
    memcpy(rax.getPointer("dtsc_data"), packed.data(), packed.size());
    rax.setRCount(1);
    rax.setEndPos(1);
    rax.setReady();
  }

  int fail(const char *message) {
    fprintf(stderr, "FAIL: %s\n", message);
    return 1;
  }
} // namespace

int main() {
  const std::string name = "feedpausewait" + std::to_string(getpid());
  IPC::sharedPage confPage;
  writeStreamConfig(name, confPage);
  char pageName[NAME_BUFFER_SIZE];
  snprintf(pageName, NAME_BUFFER_SIZE, SHM_STREAM_STATE, name.c_str());
  IPC::sharedPage statePage(pageName, STRMSTATE_PAGE_LEN, true, false);
  snprintf(pageName, NAME_BUFFER_SIZE, SHM_STREAM_IPID, name.c_str());
  IPC::sharedPage pidPage(pageName, sizeof(uint64_t), true, false);
  if (!confPage || !statePage || !pidPage) { return fail("could not create the stream's shared pages"); }
  statePage.mapped[0] = STRMSTAT_READY;
  *(uint64_t *)pidPage.mapped = getpid();

  LiveWriter writer(name);
  Comms::Users bufferUsers;
  bufferUsers.reload(name, true);
  writer.put(0, true);
  writer.put(40, false);
  writer.put(80, false);

  Util::Config cfg("feed-pause-wait");
  JSON::Value capa;
  Socket::Connection conn;
  PauseReader reader(conn, cfg, capa);
  reader.attach(name);
  uint64_t time = 0;
  if (!reader.next(time) || time != 0 || !reader.next(time) || time != 40) {
    return fail("reader did not return the buffered packets");
  }
  if (reader.next(time) || !reader.reading()) { return fail("reader did not wait for the packet after 80 ms"); }

  // The buffer pauses the feed for most of a data wait, then resumes it. The next packet arrives
  // after the rest of the original budget ran out, but within a full data wait of the resume.
  statePage.mapped[STRMSTATE_PROCESS_FEED_PAUSED_OFFSET] = 1;
  if (!reader.waitFor(dataWait - 1000)) { return fail("reader gave up while the feed was paused"); }
  statePage.mapped[STRMSTATE_PROCESS_FEED_PAUSED_OFFSET] = 0;
  if (!reader.waitFor(2000)) {
    return fail("the feed pause used up the reader's data wait: it gave up before a full timeout after the resume");
  }
  writer.put(120, true);
  if (!reader.next(time) || time != 80) { return fail("reader did not continue once the resumed feed delivered data"); }

  // Without a pause, a feed that stops still ends the reader after one data wait.
  if (reader.next(time) || !reader.reading()) { return fail("reader did not wait for the packet after 120 ms"); }
  const uint64_t stallStart = Util::bootMS();
  reader.waitFor(dataWait + 2000);
  if (reader.reading()) { return fail("a stalled feed that was not paused never timed out"); }
  if (Util::bootMS() - stallStart < dataWait - 200) { return fail("a stalled feed timed out before a full data wait"); }
  fprintf(stderr, "feed pause keeps the full data wait: OK\n");
  return 0;
}

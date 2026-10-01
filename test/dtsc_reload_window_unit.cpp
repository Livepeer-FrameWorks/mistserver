#include "../src/output/recording_summary.h"
#include "../src/output/track_reload.h"

#include <mist/defines.h>
#include <mist/dtsc.h>
#include <mist/shared_memory.h>
#include <mist/stream.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <sys/mman.h>
#include <unistd.h>

// A writer resizing a track unlinks the track's metadata page and only then
// creates the replacement under the same name. A reader that reloads inside
// that window must keep a usable view of the track.
namespace {
  int failures = 0;

  void check(bool ok, const char *what) {
    if (!ok) {
      fprintf(stderr, "FAIL: %s\n", what);
      ++failures;
    }
  }

  std::string trackPageName(const std::string & stream, size_t idx) {
    char name[NAME_BUFFER_SIZE];
    snprintf(name, NAME_BUFFER_SIZE, SHM_STREAM_TM, stream.c_str(), (uint32_t)getpid(), idx);
    return name;
  }

  // What the writer has done when the reader runs inside the resize window: the
  // old page carries the reload flag and its name no longer exists.
  bool openResizeWindow(const std::string & stream, size_t idx) {
    const std::string name = trackPageName(stream, idx);
    {
      IPC::sharedPage old(name, 0, false, false);
      if (!old.mapped) {
        fprintf(stderr, "FAIL: cannot open track page %s\n", name.c_str());
        return false;
      }
      Util::RelAccX(old.mapped, false).setReload();
    }
#ifdef SHM_ENABLED
    const int unlinked = shm_unlink(name.c_str());
#else
    const int unlinked = unlink((Util::getTmpFolder() + name).c_str());
#endif
    if (unlinked) {
      fprintf(stderr, "FAIL: cannot unlink track page %s: %s\n", name.c_str(), strerror(errno));
      return false;
    }
    return true;
  }

  void setStreamReload(const std::string & stream) {
    char name[NAME_BUFFER_SIZE];
    snprintf(name, NAME_BUFFER_SIZE, SHM_STREAM_META, stream.c_str());
    IPC::sharedPage page(name, 0, false, false);
    if (page.mapped) { Util::RelAccX(page.mapped, false).setReload(); }
  }

  void addTracks(DTSC::Meta & writer, size_t & video, size_t & audio) {
    video = writer.addTrack();
    writer.setType(video, "video");
    writer.setCodec(video, "H264");
    writer.setID(video, 1);
    audio = writer.addTrack();
    writer.setType(audio, "audio");
    writer.setCodec(audio, "AAC");
    writer.setID(audio, 2);
    writer.update(1000, 0, audio, 10, 0, true, 10);
    writer.update(2000, 0, audio, 10, 0, true, 10);
  }

  // The reader caught mid-resize keeps the old page until the replacement
  // exists, then maps the replacement with its new capacity.
  void resizeWindow() {
    const std::string stream = "rw" + std::to_string(getpid());
    DTSC::Meta writer;
    writer.reInit(stream, true);
    size_t video, audio;
    addTracks(writer, video, audio);

    DTSC::Meta reader;
    reader.reInit(stream, false, false);
    reader.reloadReplacedPagesIfNeeded();
    check(reader.trackLoaded(audio), "reader loads the audio track");

    if (!openResizeWindow(stream, audio)) {
      ++failures;
      return;
    }
    reader.reloadReplacedPagesIfNeeded();
    check(reader.trackValid(audio), "the audio track stays valid in the resize window");
    check(reader.trackLoaded(audio), "the audio track stays loaded in the resize window");
    check(reader.getValidTracks().count(audio), "the audio track stays in the valid track set in the resize window");
    check(!Mist::trackAwaitingReload(reader, audio), "the audio track is not awaiting a reload in the resize window");
    check(Mist::recordedTrackDescribable(reader, audio), "the audio track can be described in the resize window");
    try {
      check(reader.getFirstms(audio) == 1000, "the kept page still reports the track bounds");
      JSON::Value T;
      Mist::describeRecordedTrack(reader, audio, T);
      check(T["codec"].asStringRef() == "AAC", "the recording summary describes the kept page");
    } catch (const std::exception & e) {
      fprintf(stderr, "FAIL: reading the audio track in the resize window threw: %s\n", e.what());
      ++failures;
    }

    const size_t keyCount = DEFAULT_KEY_COUNT * 2;
    writer.resizeTrack(audio, DEFAULT_FRAGMENT_COUNT, keyCount, DEFAULT_PART_COUNT, DEFAULT_PAGE_COUNT, "test");
    reader.reloadReplacedPagesIfNeeded();
    check(reader.trackLoaded(audio), "the reader loads the replacement page");
    if (reader.trackLoaded(audio)) {
      check(reader.keys(audio).getRCount() == keyCount, "the reader maps the replacement page with its new capacity");
      check(reader.getFirstms(audio) == 1000, "the replacement page keeps the track bounds");
    }
  }

  // A full metadata reload that finds a track page missing leaves that track
  // unloaded rather than loaded without a page, and loads it once the page exists.
  void fullReloadWindow() {
    const std::string stream = "fr" + std::to_string(getpid());
    DTSC::Meta writer;
    writer.reInit(stream, true);
    size_t video, audio;
    addTracks(writer, video, audio);

    DTSC::Meta reader;
    reader.reInit(stream, false, false);
    reader.reloadReplacedPagesIfNeeded();
    check(reader.trackLoaded(audio), "reader loads the audio track before the full reload");

    if (!openResizeWindow(stream, audio)) {
      ++failures;
      return;
    }
    setStreamReload(stream);
    reader.reloadReplacedPagesIfNeeded();
    check(reader.trackValid(audio), "the audio track stays valid after the full reload");
    check(!reader.trackLoaded(audio), "a missing page leaves the audio track unloaded, not loaded without a page");
    check(Mist::trackAwaitingReload(reader, audio), "the unloaded audio track is awaiting a reload");
    check(!Mist::recordedTrackDescribable(reader, audio), "the unloaded audio track is not described");
    check(!reader.getValidTracks().count(audio), "the unloaded audio track is not in the valid track set");
    check(reader.trackLoaded(video), "the video track loads in the same full reload");

    writer.resizeTrack(audio, DEFAULT_FRAGMENT_COUNT, DEFAULT_KEY_COUNT * 2, DEFAULT_PART_COUNT, DEFAULT_PAGE_COUNT, "test");
    reader.reloadReplacedPagesIfNeeded();
    check(reader.trackLoaded(audio), "the reader loads the audio track once its page exists");
    check(!Mist::trackAwaitingReload(reader, audio), "the loaded audio track is no longer awaiting a reload");
    if (reader.trackLoaded(audio)) {
      check(reader.getFirstms(audio) == 1000, "the recreated page reports the track bounds");
    }
  }

  // swap exchanges two open pages without closing or unlinking either.
  void pageSwap() {
    const std::string nameA = "/MstSwpA" + std::to_string(getpid());
    const std::string nameB = "/MstSwpB" + std::to_string(getpid());
    IPC::sharedPage a(nameA, 4096, true);
    IPC::sharedPage b(nameB, 8192, true);
    a.mapped[0] = 'a';
    b.mapped[0] = 'b';
    const int handleA = a.handle;
    a.swap(b);
    check(a.name == nameB && b.name == nameA, "swap exchanges the page names");
    check(a.len == 8192 && b.len == 4096, "swap exchanges the page lengths");
    check(a.handle != handleA && b.handle == handleA, "swap exchanges the handles");
    check(a.mapped[0] == 'b' && b.mapped[0] == 'a', "swap exchanges the mappings");
    check(a.master && b.master, "swap keeps ownership with the mapping");
    IPC::sharedPage stillThere(nameA, 0, false, false);
    check(stillThere.mapped && stillThere.mapped[0] == 'a', "swap does not unlink either page");
  }
} // namespace

int main() {
  resizeWindow();
  fullReloadWindow();
  pageSwap();
  if (failures) {
    fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  fprintf(stderr, "dtsc reload window: OK\n");
  return 0;
}

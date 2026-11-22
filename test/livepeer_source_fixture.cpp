#include <mist/dtsc.h>
#include <mist/proc_stats.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(int argc, char **argv) {
  if (argc == 3 && !strcmp(argv[1], "reason")) {
    char name[NAME_BUFFER_SIZE];
    snprintf(name, sizeof(name), SHM_PROC_STATE, atoi(argv[2]));
    IPC::sharedPage page(name, 0, false, false);
    page.master = false;
    ProcState state;
    if (!ProcState::readSnapshot(page, state)) { return 1; }
    printf("%s\n", state.shortReason);
    page.master = true;
    return 0;
  }
  if (argc != 6) { return 2; }
  const bool delayed = !strncmp(argv[2], "H264+EARLY", 10);
  DTSC::Meta meta(argv[1], true);
  const size_t track = meta.addTrack(1, 1, 16, 2, true);
  meta.setID(track, 1);
  meta.setType(track, "video");
  meta.setCodec(track, !strncmp(argv[2], "H264+", 5) ? "H264" : argv[2]);
  meta.setWidth(track, 320);
  meta.setHeight(track, 180);
  meta.setRotation(track, atoi(argv[3]));
  meta.setMirror(track, atoi(argv[4]));
  meta.setVod(!strcmp(argv[5], "vod"));
  meta.setLive(!strcmp(argv[5], "live"));
  if (!delayed) { meta.update(0, 0, track, 1, 0, true, 1); }
  meta.setBps(track, 100000);
  if (!strncmp(argv[2], "H264+", 5)) {
    const size_t raw = meta.addTrack(1, 1, 16, 2, true);
    meta.setID(raw, 2);
    meta.setType(raw, "video");
    meta.setCodec(raw, delayed ? (strstr(argv[2], "EARLYJPEG") ? "JPEG" : "AV1") : (!strcmp(argv[2], "H264+MPEG2") ? "MPEG2" : "I420"));
    meta.setWidth(raw, 320);
    meta.setHeight(raw, 180);
    meta.update(0, 0, raw, 1, 0, true, 1);
    meta.setBps(raw, 10000000);
  }
  puts("ready");
  fflush(stdout);
  if (delayed) {
    getchar();
    meta.update(0, 0, track, 1, 0, true, 1);
    puts("published");
    fflush(stdout);
  }
  getchar();
  return 0;
}

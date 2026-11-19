#include <mist/dtsc.h>
#include <mist/socket.h>
#include <mist/timing.h>

#include <cstdio>
#include <set>
#include <string>
#include <unistd.h>

static int failures = 0;
static int64_t wireZero(DTSC::Meta & meta) {
  int fds[2];
  if (pipe(fds)) {
    ++failures;
    return 0;
  }
  {
    Socket::Connection output(fds[1], -1);
    output.setBlocking(true);
    meta.send(output, true, std::set<size_t>(), true);
    output.close();
  }
  std::string bytes;
  char buffer[4096];
  ssize_t size;
  while ((size = read(fds[0], buffer, sizeof(buffer))) > 0) { bytes.append(buffer, size); }
  close(fds[0]);
  DTSC::Packet header(bytes.data(), bytes.size(), true);
  return header.getScan().getMember("unixzero").asInt();
}
static void explicitClock(bool live, bool vod) {
  DTSC::Meta source;
  source.reInit("", true);
  source.setLive(live);
  source.setVod(vod);
  const int64_t expected = 1700000000123LL;
  source.setUTCOffset(expected);
  source.setBootMsOffset(0);
  const int64_t actual = wireZero(source);
  if (actual != expected) {
    fprintf(stderr, "FAIL: live=%d vod=%d Unix zero=%lld, expected authoritative clock %lld\n", live, vod,
            (long long)actual, (long long)expected);
    ++failures;
  }
}
int main() {
  explicitClock(true, true);
  explicitClock(true, false);
  explicitClock(false, true);
  DTSC::Meta source;
  source.reInit("", true);
  source.setLive(true);
  source.setBootMsOffset(12345);
  const int64_t beforeUnix = Util::unixMS();
  const int64_t beforeBoot = Util::bootMS();
  const int64_t actual = wireZero(source);
  const int64_t afterBoot = Util::bootMS();
  const int64_t afterUnix = Util::unixMS();
  if (actual < beforeUnix - afterBoot + 12345 || actual > afterUnix - beforeBoot + 12345) {
    fprintf(stderr, "FAIL: live source without UTC lost its boot-clock fallback\n");
    ++failures;
  }
  return failures ? 1 : 0;
}

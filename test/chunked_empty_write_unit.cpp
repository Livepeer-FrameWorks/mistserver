#include <mist/dtsc.h>
#include <mist/socket.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <unistd.h>

static int failures = 0;

static void check(bool ok, const std::string & what) {
  if (!ok) {
    fprintf(stderr, "FAIL: %s\n", what.c_str());
    ++failures;
  }
}

// Writes everything the callback sends through a blocking pipe-backed
// connection and returns the raw bytes that reached the other end.
template<typename F> static std::string capture(bool chunked, F write) {
  int fds[2];
  if (pipe(fds)) { return ""; }
  std::string out;
  std::thread reader([&]() {
    char buf[65536];
    ssize_t r;
    while ((r = read(fds[0], buf, sizeof(buf))) > 0) { out.append(buf, r); }
  });
  {
    Socket::Connection conn(fds[1], -1);
    conn.setBlocking(true);
    conn.setChunkedMode(chunked);
    write(conn);
    conn.close();
  }
  close(fds[1]);
  reader.join();
  close(fds[0]);
  return out;
}

// Decodes an HTTP/1.1 chunked body; complete is set only when the zero-size
// terminator chunk ends the stream. dataChunks counts the non-terminator chunks.
static std::string dechunk(const std::string & wire, bool & complete, size_t & trailing, size_t & dataChunks) {
  std::string body;
  size_t pos = 0;
  complete = false;
  dataChunks = 0;
  while (pos < wire.size()) {
    size_t eol = wire.find("\r\n", pos);
    if (eol == std::string::npos) { break; }
    size_t len = strtoul(wire.substr(pos, eol - pos).c_str(), 0, 16);
    pos = eol + 2;
    if (!len) {
      complete = true;
      pos += 2;
      break;
    }
    body.append(wire, pos, len);
    ++dataChunks;
    pos += len + 2;
  }
  trailing = wire.size() > pos ? wire.size() - pos : 0;
  return body;
}

class TestMeta : public DTSC::Meta {
  public:
    TestMeta() { reInit("", true); }
};

int main() {
  // Zero-length writes of real data are no-ops; only SendNow(0, 0) ends the body.
  std::string wire = capture(true, [](Socket::Connection & c) {
    c.SendNow("abc", 3);
    c.SendNow(std::string());
    c.SendNow("xyz", 0);
    c.SendNow("de", 2);
    c.SendNow(0, 0);
  });
  check(wire == "3\r\nabc\r\n2\r\nde\r\n0\r\n\r\n", "empty chunked writes must not terminate the body, got: " + wire);

  // A DTSH header with an image track (empty init) must arrive whole over a
  // chunked upload: the relay rejects a body shorter than its declared length.
  TestMeta M;
  M.setVod(true);
  size_t video = M.addTrack();
  M.setType(video, "video");
  M.setCodec(video, "H264");
  M.setInit(video, std::string("\001\144\000\037\377", 5));
  M.setWidth(video, 1920);
  M.setHeight(video, 1080);
  M.setID(video, 1);
  size_t thumbs = M.addTrack();
  M.setType(thumbs, "video");
  M.setCodec(thumbs, "JPEG");
  M.setWidth(thumbs, 160);
  M.setHeight(thumbs, 90);
  M.setID(thumbs, 2);
  for (uint64_t t = 0; t < 10; ++t) {
    M.update(t * 1000, 0, video, 1000, t * 1000, true);
    M.update(t * 1000, 0, thumbs, 100, 100000 + t * 100, true);
  }
  std::set<size_t> tracks = M.getValidTracks();
  check(tracks.size() == 2, "test meta must expose both tracks");

  std::string raw = capture(false, [&](Socket::Connection & c) { M.send(c, false, tracks, false); });
  check(raw.size() == M.getSendLen(false, tracks, false),
        "raw DTSH size " + std::to_string(raw.size()) + " != getSendLen " + std::to_string(M.getSendLen(false, tracks, false)));

  std::string chunked = capture(true, [&](Socket::Connection & c) {
    M.send(c, false, tracks, false);
    c.SendNow(0, 0);
  });
  bool complete = false;
  size_t trailing = 0;
  size_t dataChunks = 0;
  std::string body = dechunk(chunked, complete, trailing, dataChunks);
  check(complete && !trailing, "chunked DTSH upload must end exactly at its terminator");
  check(body == raw,
        "chunked DTSH body (" + std::to_string(body.size()) + " bytes) must equal the raw header (" +
          std::to_string(raw.size()) + " bytes)");
  // The header is well under one 64 KiB block, so it must travel as a single
  // chunk rather than one chunk per serialized field.
  check(dataChunks == 1, "DTSH header must be sent as one chunk, got " + std::to_string(dataChunks));

  // Key byte positions must be read by key index. A live track whose first key
  // was trimmed while its fragment remains has keys starting after fragments.
  TestMeta L;
  L.setLive(true);
  size_t live = L.addTrack();
  L.setType(live, "video");
  L.setCodec(live, "H264");
  L.setInit(live, std::string("\001\144\000\037\377", 5));
  L.setWidth(live, 1280);
  L.setHeight(live, 720);
  L.setID(live, 1);
  for (uint64_t t = 0; t < 12; ++t) { L.update(t * 1000, 0, live, 1000, 5000 + t * 777, true); }
  // Trim the first key record only (the in-memory meta has no data pages for
  // Meta::removeFirstKey to release); the fragment start stays at 0.
  L.keys(live).deleteRecords(1);
  check(L.keys(live).getStartPos() != L.fragments(live).getStartPos(),
        "setup: key and fragment start positions must differ to exercise the bpos index");
  std::set<size_t> liveTracks = L.getValidTracks();
  std::string liveRaw = capture(false, [&](Socket::Connection & c) { L.send(c, false, liveTracks, false); });
  DTSC::Meta parsed("", DTSC::Scan((char *)liveRaw.data() + 8, liveRaw.size() - 8));
  std::set<size_t> parsedTracks = parsed.getValidTracks();
  check(parsedTracks.size() == 1, "parsed live DTSH must expose its track");
  if (parsedTracks.size() == 1) {
    DTSC::Keys sentKeys(L.getKeys(live));
    DTSC::Keys readKeys(parsed.getKeys(*parsedTracks.begin()));
    check(readKeys.getValidCount() == sentKeys.getValidCount(), "parsed key count must match");
    for (size_t i = 0; i < sentKeys.getValidCount() && i < readKeys.getValidCount(); ++i) {
      check(readKeys.getBpos(readKeys.getFirstValid() + i) == sentKeys.getBpos(sentKeys.getFirstValid() + i),
            "key " + std::to_string(i) + " bpos must survive DTSH serialization");
    }
  }

  if (failures) { return 1; }
  fprintf(stderr, "chunked empty writes: OK\n");
  return 0;
}

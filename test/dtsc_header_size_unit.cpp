#include <mist/dtsc.h>
#include <mist/socket.h>
#include <mist/bitfields.h>
#include <cstdio>
#include <unistd.h>

int main() {
  DTSC::Meta meta;
  meta.reInit("", true);
  const size_t full = meta.addTrack();
  meta.setID(full, 1);
  meta.setType(full, "audio");
  meta.setCodec(full, "AAC");
  meta.update(0, 0, full, 16, 0, true, 24);
  const size_t empty = meta.addTrack();
  meta.setID(empty, 2);
  meta.setType(empty, "audio");
  meta.setCodec(empty, "AAC");
  const size_t hidden = meta.addTrack();
  meta.setID(hidden, 3);
  meta.setType(hidden, "audio");
  meta.setCodec(hidden, "AAC");
  meta.validateTrack(hidden, 0);
  for (const std::set<size_t> & selection : {std::set<size_t>(), std::set<size_t>{full, empty}}) {
    int fds[2];
    if (pipe(fds)) { return 2; }
    {
      Socket::Connection writer(fds[1], -1);
      writer.setBlocking(true);
      meta.send(writer, true, selection);
      writer.close();
    }
    std::string wire;
    char data[4096];
    ssize_t size;
    while ((size = read(fds[0], data, sizeof(data))) > 0) { wire.append(data, size); }
    close(fds[0]);
    const uint64_t declared = Bit::btohl(wire.data()+4) + 8;
    if (wire.size() != declared || declared != meta.getSendLen(true, selection)) {
      fprintf(stderr, "DTSH selected/empty-track size mismatch: wrote %zu, declared %llu, measured %llu\n", wire.size(), (unsigned long long)declared, (unsigned long long)meta.getSendLen(true, selection));
      return 1;
    }
  }
  return 0;
}

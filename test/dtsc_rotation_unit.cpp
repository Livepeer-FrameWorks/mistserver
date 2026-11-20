#include <mist/bitfields.h>
#include <mist/dtsc.h>

#include <cassert>
#include <fstream>
#include <iterator>
#include <unistd.h>

class Probe : public DTSC::Meta {
  public:
    void absentField(size_t track) {
      tracks.at(track).trackRotationField = Util::RelAccXFieldData();
      tracks.at(track).trackMirrorField = Util::RelAccXFieldData();
    }
};
int main() {
  Probe meta;
  meta.reInit("", true);
  JSON::Value described;
  described["type"] = "video";
  described["codec"] = "VP9";
  described["width"] = 320;
  described["height"] = 180;
  described["rotation"] = 90;
  described["mirror"] = 1;
  described["trackid"] = 1;
  std::string bytes = described.toPacked();
  meta.addTrackFrom(DTSC::Scan(&bytes[0], bytes.size()));
  const size_t track = *meta.getValidTracks().begin();
  assert(meta.getRotation(track) == 90);
  assert(meta.getMirror(track));
  JSON::Value json;
  meta.toJSON(json);
  bool found = false;
  jsonForEachConst (json["tracks"], it) {
    if ((*it)["rotation"].asInt() == 90 && (*it)["mirror"].asBool()) { found = true; }
  }
  assert(found);
  DTSC::Meta copied;
  copied.reInit("", true);
  copied.merge(meta, false, false);
  assert(copied.getRotation(*copied.getValidTracks().begin()) == 90);
  assert(copied.getMirror(*copied.getValidTracks().begin()));
  meta.resizeTrack(track, 2, 8, 32, 2);
  assert(meta.getRotation(track) == 90);
  assert(meta.getMirror(track));
  DTSC::TrackMetadata trk;
  trk.type = "video";
  trk.codec = "VP9";
  trk.width = 320;
  trk.height = 180;
  trk.rotation = 90;
  trk.mirror = true;
  assert(meta.resumeMismatch(track, trk).empty());
  trk.mirror = false;
  assert(meta.resumeMismatch(track, trk) == "mirror");
  trk.mirror = true;
  trk.rotation = 270;
  assert(meta.resumeMismatch(track, trk) == "rotation");
  const size_t zero = meta.addTrack();
  meta.setType(zero, "video");
  meta.setCodec(zero, "VP9");
  meta.setID(zero, 2);
  char path[] = "/tmp/mist-rotation-unit-XXXXXX";
  int fd = mkstemp(path);
  assert(fd >= 0);
  close(fd);
  meta.toFile(path);
  std::ifstream file(path, std::ios::binary);
  std::string data{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
  assert(data.size() > 8 && Bit::btohl(data.data() + 4) + 8 == data.size());
  DTSC::Packet packet(data.data(), data.size());
  assert(DTSC::headerHasVideoRotation(packet.getScan()));
  DTSC::Meta reread("", std::string(path));
  unlink(path);
  assert(reread.getRotation(reread.trackIDToIndex(1)) == 90);
  assert(reread.getRotation(reread.trackIDToIndex(2)) == 0);
  assert(reread.getMirror(reread.trackIDToIndex(1)));
  assert(!reread.getMirror(reread.trackIDToIndex(2)));
  JSON::Value old;
  old["tracks"]["video"] = described;
  old["tracks"]["video"].removeMember("rotation");
  bytes = old.toPacked();
  assert(!DTSC::headerHasVideoRotation(DTSC::Scan(&bytes[0], bytes.size())));
  old["tracks"]["video"]["rotation"] = 0;
  old["tracks"]["video"].removeMember("mirror");
  bytes = old.toPacked();
  assert(!DTSC::headerHasVideoRotation(DTSC::Scan(&bytes[0], bytes.size())));
  old["tracks"]["video"]["mirror"] = 0;
  bytes = old.toPacked();
  assert(DTSC::headerHasVideoRotation(DTSC::Scan(&bytes[0], bytes.size())));
  meta.absentField(track);
  assert(meta.getRotation(track) == 0);
  meta.setRotation(track, 90);
  assert(meta.getRotation(track) == 0);
  assert(!meta.getMirror(track));
  meta.setMirror(track, true);
  assert(!meta.getMirror(track));
  return 0;
}

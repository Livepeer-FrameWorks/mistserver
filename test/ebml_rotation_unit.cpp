#include <mist/ebml_socketglue.h>

#include <cassert>
#include <fstream>
#include <iterator>
#include <unistd.h>

int main() {
  for (bool mirror : {false, true}) {
    for (uint16_t angle : {0, 90, 180, 270}) {
      if (!angle && !mirror) { continue; }
      char path[] = "/tmp/mist-projection-XXXXXX";
      const int fd = mkstemp(path);
      assert(fd >= 0);
      Socket::Connection connection;
      connection.open(fd);
      EBML::sendVideoProjection(connection, angle, mirror);
      connection.close();
      std::ifstream file(path, std::ios::binary);
      std::string data{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
      unlink(path);
      assert(data.size() == EBML::sizeVideoProjection(angle, mirror));
      EBML::Element projection(data.data());
      assert(projection.getID() == EBML::EID_PROJECTION);
      assert(projection.findChild(EBML::EID_PROJECTIONROLL).getValFloat() == Mist::Orientation(angle, mirror).projectionRoll());
      std::string video;
      EBML::appendElemHead(video, EBML::EID_VIDEO, data.size());
      video += data;
      std::string track;
      EBML::appendElemHead(track, EBML::EID_TRACKENTRY, video.size());
      track += video;
      assert(EBML::videoOrientation(EBML::Element(track.data())) == Mist::Orientation(angle, mirror));
      const double roll = projection.findChild(EBML::EID_PROJECTIONROLL).getValFloat();
      assert(roll > -180 && roll <= 180);
    }
  }
  assert(EBML::sizeVideoProjection(0) == 0);
  for (const char *doctype : {"matroska", "webm"}) {
    char path[] = "/tmp/mist-projection-version-XXXXXX";
    const int fd = mkstemp(path);
    assert(fd >= 0);
    Socket::Connection connection;
    connection.open(fd);
    EBML::sendElemEBML(connection, doctype, true);
    connection.close();
    std::ifstream file(path, std::ios::binary);
    const std::string data{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    unlink(path);
    assert(EBML::Element(data.data()).findChild(EBML::EID_DOCTYPEVERSION).getValUInt() == 4);
  }
  return 0;
}

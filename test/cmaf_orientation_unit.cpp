#include <mist/cmaf.h>

#include <cstdio>

static bool rotationOnly(const char *data, size_t size, unsigned int rotation) {
  if (size < 16) { return false; }
  MP4::Box ftyp(const_cast<char *>(data), false);
  MP4::Box moovBox(const_cast<char *>(data) + ftyp.boxedSize(), false);
  if (!ftyp.isType("ftyp") || !moovBox.isType("moov")) { return false; }
  MP4::MOOV & moov = (MP4::MOOV &)moovBox;
  const std::deque<MP4::TRAK> tracks = moov.getChildren<MP4::TRAK>();
  if (tracks.size() != 1) { return false; }
  MP4::TRAK track = tracks.front();
  MP4::TKHD header = track.getChild<MP4::TKHD>();
  return header.getOrientation() == Mist::Orientation(rotation, false);
}

int main() {
  setbuf(stdout, NULL);
  for (unsigned int rotation : {0u, 90u, 180u, 270u}) {
    for (bool mirror : {false, true}) {
      DTSC::Meta meta;
      meta.reInit("", true);
      const size_t video = meta.addTrack();
      meta.setID(video, 1);
      meta.setType(video, "video");
      meta.setCodec(video, "H264");
      meta.setInit(video, std::string("\x01\x42\x00\x1e\xff\xe0\x00", 7));
      meta.setWidth(video, 320);
      meta.setHeight(video, 180);
      meta.setRotation(video, rotation);
      meta.setMirror(video, mirror);
      meta.update(0, 0, video, 10, 0, true, 40);
      printf("building %u/%d\n", rotation, mirror);
      const std::string single = CMAF::trackHeader(meta, video);
      std::map<size_t, Comms::Users> selection;
      selection[video];
      Util::ResizeablePointer muxed;
      if (!CMAF::header(muxed, meta, selection) || !rotationOnly(single.data(), single.size(), rotation) ||
          !rotationOnly((char *)muxed, muxed.size(), rotation)) {
        fprintf(stderr, "CMAF init lost rotation or wrote a mirrored matrix: %u/%d\n", rotation, mirror);
        return 1;
      }
      printf("PASS single and combined CMAF init rotation=%u mirror=%d\n", rotation, mirror);
    }
  }
}

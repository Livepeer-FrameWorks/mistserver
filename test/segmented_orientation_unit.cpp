#include "../src/input/input_ismv.h"

#include <mist/segmentreader.h>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <unistd.h>

class ISMVProbe : public Mist::InputISMV {
  public:
    ISMVProbe(Util::Config *cfg) : Mist::InputISMV(cfg) {}
    bool check(MP4::MOOV & movie, const Mist::Orientation & expected) {
      meta.reInit("", true);
      parseMoov(movie);
      const std::set<size_t> tracks = M.getValidTracks();
      if (tracks.size() != 1) { return false; }
      const size_t idx = *tracks.begin();
      return M.getRotation(idx) == expected.rotation && M.getMirror(idx) == expected.mirror;
    }
};

MP4::MOOV movieWith(const Mist::Orientation & trackOrientation, const Mist::Orientation & movieOrientation,
                    bool hasMovieHeader = true) {
  MP4::TKHD track(1, 0, 320, 180);
  track.setOrientation(trackOrientation);
  MP4::TRAK trak;
  trak.setContent(track, 0);
  MP4::MDHD timing(0);
  timing.setTimeScale(1000);
  MP4::HDLR handler("video", "orientation");
  MP4::AVC1 video;
  video.setWidth(320);
  video.setHeight(180);
  MP4::STSD sample(0);
  sample.setEntry(video, 0);
  MP4::STBL table;
  table.setContent(sample, 0);
  MP4::MINF mediaInfo;
  mediaInfo.setContent(table, 0);
  MP4::MDIA media;
  media.setContent(timing, 0);
  media.setContent(handler, 1);
  media.setContent(mediaInfo, 2);
  trak.setContent(media, 1);
  MP4::MVHD header(0);
  double a, b, c, d;
  movieOrientation.matrix(a, b, c, d);
  header.setMatrix(a, 0);
  header.setMatrix(b, 1);
  header.setMatrix(c, 3);
  header.setMatrix(d, 4);
  MP4::MOOV movie;
  if (hasMovieHeader) { movie.setContent(header, 0); }
  movie.setContent(trak, hasMovieHeader ? 1 : 0);
  return movie;
}

static const Mist::Orientation orientations[] = {
  Mist::Orientation(),    Mist::Orientation(),          Mist::Orientation(0, true),
  Mist::Orientation(180), Mist::Orientation(180, true), Mist::Orientation(90, true),
  Mist::Orientation(90),  Mist::Orientation(270, true), Mist::Orientation(270)};

int main(int argc, char **argv) {
  assert(argc == 2);
  Util::Config cfg("orientation-probe");
  ISMVProbe ismv(&cfg);
  for (unsigned track = 1; track <= 8; ++track) {
    for (unsigned film = 0; film <= 8; ++film) {
      const Mist::Orientation orientation = orientations[track],
                              movieOrientation = film ? orientations[film] : Mist::Orientation();
      const Mist::Orientation expected = orientation.then(movieOrientation);
      MP4::MOOV movie = movieWith(orientation, movieOrientation, film != 0);
      if (!strcmp(argv[1], "ismv")) {
        if (!ismv.check(movie, expected)) {
          fprintf(stderr, "ISMV lost track/movie orientation %u/%u\n", track, film);
          return 1;
        }
        continue;
      }
      char path[] = "/tmp/mist-segment-orientation-XXXXXX";
      int fd = mkstemp(path);
      assert(fd >= 0);
      MP4::MOOV segmentMovie = !strcmp(argv[1], "init") ? movieWith(Mist::Orientation(), Mist::Orientation()) : movie;
      assert(write(fd, segmentMovie.asBox(), segmentMovie.boxedSize()) == (ssize_t)segmentMovie.boxedSize());
      close(fd);
      Mist::SegmentReader reader;
      Util::ResizeablePointer buffer;
      assert(reader.load(path, 0, 0, 0, 0, &buffer));
      DTSC::Packet packet;
      reader.readNext(packet, 0);
      if (!strcmp(argv[1], "init")) { reader.setInit(std::string(movie.asBox(), movie.boxedSize())); }
      DTSC::Meta meta;
      meta.reInit("", true);
      reader.initializeMetadata(meta, 1, 1);
      unlink(path);
      const std::set<size_t> tracks = meta.getValidTracks();
      if (tracks.size() != 1) {
        fprintf(stderr, "Segment did not initialize video metadata\n");
        return 1;
      }
      const size_t idx = *tracks.begin();
      if (meta.getRotation(idx) != expected.rotation || meta.getMirror(idx) != expected.mirror) {
        fprintf(stderr, "Segmented MP4 lost track/movie orientation %u/%u\n", track, film);
        return 1;
      }
    }
  }
}

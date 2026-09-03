#include "../src/process/thumbnail_artifacts.h"

#include <cassert>
#include <cstdlib>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

namespace {
  std::string readFile(const std::string & path) {
    std::ifstream input(path.c_str(), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  }
} // namespace

int main() {
  char directoryPattern[] = "/tmp/mist-thumbnail-artifacts.XXXXXX";
  char *directory = mkdtemp(directoryPattern);
  assert(directory);

  Mist::ThumbnailArtifacts::Paths paths;
  std::string error;
  assert(Mist::ThumbnailArtifacts::publish(directory, "poster-one", "sprite-one", "WEBVTT\n\none", paths, error));
  assert(readFile(paths.poster) == "poster-one");
  assert(readFile(paths.sprite) == "sprite-one");
  assert(readFile(paths.manifest) == "WEBVTT\n\none");

  assert(!Mist::ThumbnailArtifacts::publish(directory, "poster-two", "", "WEBVTT\n\ntwo", paths, error));
  assert(error == "thumbnail generation is incomplete");
  assert(readFile(paths.poster) == "poster-one");
  assert(readFile(paths.sprite) == "sprite-one");
  assert(readFile(paths.manifest) == "WEBVTT\n\none");

  assert(unlink(paths.sprite.c_str()) == 0);
  assert(mkdir(paths.sprite.c_str(), 0755) == 0);
  assert(!Mist::ThumbnailArtifacts::publish(directory, "poster-two", "sprite-two", "WEBVTT\n\ntwo", paths, error));
  assert(error.find("refusing to replace non-regular thumbnail target") == 0);
  assert(readFile(paths.poster) == "poster-one");
  assert(readFile(paths.manifest) == "WEBVTT\n\none");

  assert(rmdir(paths.sprite.c_str()) == 0);
  assert(unlink(paths.poster.c_str()) == 0);
  assert(unlink(paths.manifest.c_str()) == 0);
  assert(rmdir(directory) == 0);
  return 0;
}

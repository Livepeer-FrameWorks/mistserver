#include "thumbnail_artifacts.h"

#include <mist/util.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace Mist {
  namespace ThumbnailArtifacts {
    namespace {
      struct StagedFile {
          std::string target;
          std::string temporary;
      };

      bool validTarget(const std::string & path, std::string & error) {
        struct stat info;
        if (lstat(path.c_str(), &info) != 0) {
          if (errno == ENOENT) { return true; }
          error = "could not inspect " + path + ": " + strerror(errno);
          return false;
        }
        if (!S_ISREG(info.st_mode)) {
          error = "refusing to replace non-regular thumbnail target " + path;
          return false;
        }
        return true;
      }

      bool stageFile(const std::string & target, const std::string & data, StagedFile & staged, std::string & error) {
        std::string pattern = target + ".tmp.XXXXXX";
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back(0);
        int descriptor = mkstemp(writable.data());
        if (descriptor < 0) {
          error = "could not stage " + target + ": " + strerror(errno);
          return false;
        }
        staged.target = target;
        staged.temporary = writable.data();
        fchmod(descriptor, 0644);

        size_t offset = 0;
        while (offset < data.size()) {
          ssize_t written = write(descriptor, data.data() + offset, data.size() - offset);
          if (written < 0 && errno == EINTR) { continue; }
          if (written <= 0) {
            error = "could not write " + staged.temporary + ": " + strerror(errno);
            close(descriptor);
            unlink(staged.temporary.c_str());
            staged.temporary.clear();
            return false;
          }
          offset += (size_t)written;
        }
        if (close(descriptor) != 0) {
          error = "could not close " + staged.temporary + ": " + strerror(errno);
          unlink(staged.temporary.c_str());
          staged.temporary.clear();
          return false;
        }
        return true;
      }

      void cleanup(std::vector<StagedFile> & staged) {
        for (std::vector<StagedFile>::iterator it = staged.begin(); it != staged.end(); ++it) {
          if (it->temporary.size()) { unlink(it->temporary.c_str()); }
        }
      }
    } // namespace

    bool publish(const std::string & directory, const std::string & posterData, const std::string & spriteData,
                 const std::string & manifestData, Paths & paths, std::string & error) {
      paths.poster = directory + "/poster.jpg";
      paths.sprite = directory + "/sprite.jpg";
      paths.manifest = directory + "/sprite.vtt";
      error.clear();

      if (posterData.empty() || spriteData.empty() || manifestData.empty()) {
        error = "thumbnail generation is incomplete";
        return false;
      }
      if (!Util::createPathFor(paths.poster)) {
        error = "could not create thumbnail directory " + directory;
        return false;
      }

      const std::string targets[] = {paths.poster, paths.sprite, paths.manifest};
      for (size_t i = 0; i < 3; ++i) {
        if (!validTarget(targets[i], error)) { return false; }
      }

      const std::string data[] = {posterData, spriteData, manifestData};
      std::vector<StagedFile> staged(3);
      for (size_t i = 0; i < staged.size(); ++i) {
        if (!stageFile(targets[i], data[i], staged[i], error)) {
          cleanup(staged);
          return false;
        }
      }

      for (size_t i = 0; i < staged.size(); ++i) {
        if (rename(staged[i].temporary.c_str(), staged[i].target.c_str()) != 0) {
          error = "could not publish " + staged[i].target + ": " + strerror(errno);
          cleanup(staged);
          return false;
        }
        staged[i].temporary.clear();
      }
      return true;
    }
  } // namespace ThumbnailArtifacts
} // namespace Mist

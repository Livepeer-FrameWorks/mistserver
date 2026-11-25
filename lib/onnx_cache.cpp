#include "onnx_cache.h"

#include "auth.h"
#include "timing.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <limits>
#include <map>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#ifdef __linux__
#include <linux/fs.h>
#include <sys/ioctl.h>
#endif

namespace ONNX {
  namespace ORTHelpers {
    namespace {
      namespace fs = std::filesystem;
      const uint64_t MAX_CACHE_BYTES = 512ULL * 1024 * 1024;
      const size_t MAX_CACHE_ENTRIES = 64;

      bool controlFile(const fs::path & path) {
        return path == ".lock" || path == ".use" || path == ".lease" || path == ".ready" || path == ".access";
      }

      int lockFile(const fs::path & path, int mode, uint64_t waitMs = 0) {
        const int fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (fd < 0) { return -1; }
        const uint64_t deadline = Util::bootMS() + waitMs;
        while (flock(fd, mode | LOCK_NB)) {
          if ((errno != EWOULDBLOCK && errno != EINTR) || Util::bootMS() >= deadline) {
            close(fd);
            return -1;
          }
          Util::sleep(10);
        }
        // A pruner can remove a lock inode while a process waits for it.
        struct stat held, current;
        if (fstat(fd, &held) || stat(path.c_str(), &current) || held.st_dev != current.st_dev || held.st_ino != current.st_ino) {
          close(fd);
          return -1;
        }
        return fd;
      }

      bool copyFile(const fs::path & source, const fs::path & destination) {
#ifdef __linux__
        const int input = open(source.c_str(), O_RDONLY | O_CLOEXEC);
        const int output = open(destination.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        bool cloned = input >= 0 && output >= 0 && ioctl(output, FICLONE, input) == 0;
        if (input >= 0) { close(input); }
        if (output >= 0) { close(output); }
        if (cloned) { return true; }
#endif
        std::error_code error;
        return fs::copy_file(source, destination, fs::copy_options::overwrite_existing, error) && !error;
      }

      bool copyBlobs(const fs::path & source, const fs::path & destination) {
        std::error_code error;
        fs::recursive_directory_iterator entry(source, error), end;
        while (!error && entry != end) {
          const fs::path relative = entry->path().lexically_relative(source);
          if (controlFile(relative)) {
            entry.increment(error);
            continue;
          }
          if (entry->is_symlink(error)) { return false; }
          if (entry->is_directory(error)) {
            fs::create_directories(destination / relative, error);
          } else if (entry->is_regular_file(error) && !copyFile(entry->path(), destination / relative)) {
            return false;
          }
          entry.increment(error);
        }
        return !error;
      }

      uint64_t blobBytes(const fs::path & directory) {
        std::error_code error;
        uint64_t bytes = 0;
        fs::recursive_directory_iterator entry(directory, error), end;
        while (!error && entry != end) {
          const fs::path relative = entry->path().lexically_relative(directory);
          if (!controlFile(relative) && entry->is_regular_file(error) && !entry->is_symlink(error)) {
            const uint64_t size = entry->file_size(error);
            if (size > MAX_CACHE_BYTES || bytes > MAX_CACHE_BYTES - size) { return MAX_CACHE_BYTES + 1; }
            bytes += size;
          }
          entry.increment(error);
        }
        return error ? MAX_CACHE_BYTES + 1 : bytes;
      }

      std::string fingerprint(const fs::path & directory) {
        std::map<std::string, std::string> files;
        std::error_code error;
        fs::recursive_directory_iterator entry(directory, error), end;
        while (!error && entry != end) {
          const fs::path relative = entry->path().lexically_relative(directory);
          if (!controlFile(relative) && entry->is_regular_file(error) && !entry->is_symlink(error)) {
            struct stat info;
            if (stat(entry->path().c_str(), &info)) { return ""; }
#ifdef __APPLE__
            const struct timespec modified = info.st_mtimespec, changed = info.st_ctimespec;
#else
            const struct timespec modified = info.st_mtim, changed = info.st_ctim;
#endif
            files[relative.string()] = std::to_string(info.st_size) + ":" + std::to_string(modified.tv_sec) + ":" +
              std::to_string(modified.tv_nsec) + ":" + std::to_string(changed.tv_sec) + ":" + std::to_string(changed.tv_nsec);
          }
          entry.increment(error);
        }
        if (error) { return ""; }
        std::string contents;
        for (const auto & file : files) {
          contents += std::to_string(file.first.size()) + ":" + file.first + ":" + file.second + "\n";
        }
        return Secure::sha256(contents);
      }

      bool ready(const fs::path & entry) {
        std::error_code error;
        return fs::is_regular_file(entry / ".ready", error) && !error;
      }

      void touch(const fs::path & entry) {
        const int fd = open((entry / ".access").c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd >= 0) { close(fd); }
      }

      bool removeEntry(const fs::path & path) {
        const int use = lockFile(path / ".use", LOCK_EX);
        if (use < 0) { return false; }
        const int compiler = lockFile(path / ".lock", LOCK_EX);
        if (compiler < 0) {
          close(use);
          return false;
        }
        std::error_code error;
        fs::remove_all(path, error);
        close(compiler);
        close(use);
        return !error;
      }

      void prune(const fs::path & root, const std::string & current = "") {
        const int guard = lockFile(root / ".prune", LOCK_EX);
        if (guard < 0) { return; }
        struct Entry {
            fs::path path;
            fs::file_time_type used;
            uint64_t bytes;
        };
        std::vector<Entry> entries;
        uint64_t total = 0;
        std::error_code error;
        int legacy = -1;
        for (fs::directory_iterator it(root, error), end; !error && it != end; it.increment(error)) {
          if (it->is_symlink(error)) { continue; }
          if (it->is_regular_file(error)) {
            if (it->path().extension() == ".blob" || it->path().extension() == ".cl_cache") {
              if (legacy < 0) { legacy = lockFile(root / ".lock", LOCK_EX); }
              if (legacy >= 0) { fs::remove(it->path(), error); }
            }
            continue;
          }
          if (!it->is_directory(error)) { continue; }
          const std::string name = it->path().filename().string();
          if (name.compare(0, 10, ".creating-") == 0) {
            const auto modified = fs::last_write_time(it->path(), error);
            if (!error && fs::file_time_type::clock::now() - modified > std::chrono::minutes(5)) {
              const int lease = lockFile(it->path() / ".lease", LOCK_EX);
              const int compiler = lockFile(it->path() / ".lock", LOCK_EX);
              if (lease >= 0 && compiler >= 0) { fs::remove_all(it->path(), error); }
              if (lease >= 0) { close(lease); }
              if (compiler >= 0) { close(compiler); }
            }
            continue;
          }
          if (name.compare(0, 9, ".session-") == 0) {
            const int lease = lockFile(it->path() / ".lease", LOCK_EX);
            if (lease >= 0) {
              fs::remove_all(it->path(), error);
              close(lease);
            }
            continue;
          }
          if (name.compare(0, 3, "v2-") != 0) { continue; }
          // Different model versions never share blobs; live sessions own independent copies.
          const bool obsolete = current.size() == 197 && name.size() == 197 &&
            name.substr(0, 68) == current.substr(0, 68) && name.substr(68, 64) != current.substr(68, 64);
          if (obsolete || !ready(it->path())) {
            removeEntry(it->path());
            continue;
          }
          const uint64_t size = blobBytes(it->path());
          fs::path accessed = it->path() / ".access";
          auto used = fs::last_write_time(accessed, error);
          if (error) {
            error.clear();
            used = fs::last_write_time(it->path(), error);
          }
          if (error) {
            error.clear();
            continue;
          }
          entries.push_back({it->path(), used, size});
          total += size;
        }
        std::sort(entries.begin(), entries.end(), [](const Entry & a, const Entry & b) { return a.used < b.used; });
        size_t count = entries.size();
        for (const Entry & entry : entries) {
          if (count <= MAX_CACHE_ENTRIES && total <= MAX_CACHE_BYTES) { break; }
          if (removeEntry(entry.path)) {
            --count;
            total -= entry.bytes;
          }
        }
        if (legacy >= 0) { close(legacy); }
        close(guard);
      }

      std::string temporary(const std::string & root) {
        std::string pattern = root + "/.creating-XXXXXX";
        std::vector<char> buffer(pattern.begin(), pattern.end());
        buffer.push_back(0);
        return mkdtemp(buffer.data()) ? std::string(buffer.data()) : "";
      }

      int createEntry(const std::string & root, const std::string & entry) {
        std::error_code error;
        if (fs::is_directory(entry, error)) { return -1; }
        const std::string staging = temporary(root);
        if (staging.empty()) { return -1; }
        const int compiler = lockFile(fs::path(staging) / ".lock", LOCK_EX);
        if (compiler >= 0 && !rename(staging.c_str(), entry.c_str())) { return compiler; }
        if (compiler >= 0) { close(compiler); }
        fs::remove_all(staging, error);
        return -1;
      }
    } // namespace

    std::unique_ptr<OpenVINOCache> OpenVINOCache::prepare(const std::string & root, const std::string & model,
                                                          const std::string & version, const std::string & recipe,
                                                          std::string & why) {
      if (root.empty() || model.empty() || version.size() != 64 ||
          version.find_first_not_of("0123456789abcdef") != std::string::npos) {
        why = "model/cache identity unavailable";
        return nullptr;
      }
      std::unique_ptr<OpenVINOCache> cache(new OpenVINOCache);
      cache->root_ = root;
      const std::string key = "v2-" + Secure::sha256(model) + "-" + version + "-" + Secure::sha256(recipe);
      cache->entry_ = root + "/" + key;
      prune(root, key);
      std::error_code lookup;
      if (fs::is_symlink(cache->entry_, lookup)) {
        why = "cache entry is a symbolic link";
        return nullptr;
      }
      const bool published = ready(cache->entry_);
      int use = lockFile(fs::path(cache->entry_) / ".use", LOCK_SH);
      if (use < 0 && published) {
        why = "cached snapshot is being replaced";
        return nullptr;
      }
      cache->hit_ = use >= 0 && ready(cache->entry_);
      if (!cache->hit_) {
        if (use >= 0) {
          close(use);
          use = -1;
        }
        cache->compiler_ = createEntry(root, cache->entry_);
        if (cache->compiler_ < 0) { cache->compiler_ = lockFile(fs::path(cache->entry_) / ".lock", LOCK_EX, 120000); }
        if (cache->compiler_ < 0) {
          why = "model compile lock unavailable";
          return nullptr;
        }
        if (ready(cache->entry_)) {
          close(cache->compiler_);
          cache->compiler_ = -1;
          use = lockFile(fs::path(cache->entry_) / ".use", LOCK_SH);
          cache->hit_ = use >= 0 && ready(cache->entry_);
          if (!cache->hit_) {
            if (use >= 0) close(use);
            why = "cache entry changed during lookup";
            return nullptr;
          }
        }
      }
      std::string staging = temporary(root);
      if (!staging.empty()) { cache->lease_ = lockFile(fs::path(staging) / ".lease", LOCK_EX); }
      std::string working;
      if (!staging.empty()) { working = root + "/.session-" + fs::path(staging).filename().string().substr(10); }
      bool ok = !staging.empty() && cache->lease_ >= 0 && !rename(staging.c_str(), working.c_str());
      if (ok) { cache->working_ = working; }
      if (ok && cache->hit_) { ok = copyBlobs(cache->entry_, cache->working_); }
      if (ok && cache->hit_) { cache->initial_ = fingerprint(cache->working_); }
      if (use >= 0) { close(use); }
      if (!ok) {
        std::error_code error;
        if (!staging.empty()) { fs::remove_all(staging, error); }
        why = "cannot create an isolated session cache";
        return nullptr;
      }
      touch(cache->entry_);
      return cache;
    }

    void OpenVINOCache::complete(bool successful) {
      const bool changed = hit_ && successful && !initial_.empty() && initial_ != fingerprint(working_);
      if (changed) { compiler_ = lockFile(fs::path(entry_) / ".lock", LOCK_EX); }
      if (compiler_ >= 0 && successful && blobBytes(working_) <= MAX_CACHE_BYTES) {
        const int use = lockFile(fs::path(entry_) / ".use", LOCK_EX, hit_ ? 0 : 120000);
        if (use >= 0) {
          std::error_code error;
          for (fs::directory_iterator it(entry_, error), end; !error && it != end; it.increment(error)) {
            if (it->path().filename() != ".lock" && it->path().filename() != ".use") {
              fs::remove_all(it->path(), error);
            }
          }
          if (!error && copyBlobs(working_, entry_)) {
            const int readyFile = open((fs::path(entry_) / ".ready").c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
            if (readyFile >= 0) {
              close(readyFile);
              touch(entry_);
            }
          }
          close(use);
        }
      } else if (hit_ && !successful) {
        const int compiler = lockFile(fs::path(entry_) / ".lock", LOCK_EX);
        if (compiler >= 0) {
          unlink((fs::path(entry_) / ".ready").c_str());
          close(compiler);
        }
      }
      if (compiler_ >= 0) {
        close(compiler_);
        compiler_ = -1;
      }
      prune(root_);
    }

    OpenVINOCache::~OpenVINOCache() {
      if (compiler_ >= 0) { close(compiler_); }
      std::error_code error;
      if (!working_.empty()) { fs::remove_all(working_, error); }
      if (lease_ >= 0) { close(lease_); }
      if (!root_.empty()) { prune(root_); }
    }
  } // namespace ORTHelpers
} // namespace ONNX

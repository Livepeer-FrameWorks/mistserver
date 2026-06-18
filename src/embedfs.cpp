/// \file embedfs.cpp
/// Build tool: converts a set of files into an EmbeddedFile[] VFS lookup table.
/// Usage:
///   embedfs --output <out.h> --prefix <varprefix> [--vpath <vpath> <file>] ...
///   embedfs --output <out.h> --prefix <varprefix> --manifest <manifest.txt>
/// Manifest format: one entry per line, "<vpath> <filepath>" (whitespace-separated).
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdint.h>
#include <string>
#include <vector>

struct VFSEntry {
    std::string vpath;
    std::string filepath;
};

// --- MD5 (from lib/auth.cpp, inlined to avoid json.h dependency) ---

static inline void md5_add64(uint32_t *hash, const char *data) {
  uint32_t M[16];
  for (size_t i = 0; i < 16; ++i) {
    M[i] = (uint8_t)data[i << 2] | ((uint8_t)data[(i << 2) + 1] << 8) | ((uint8_t)data[(i << 2) + 2] << 16) |
      ((uint8_t)data[(i << 2) + 3] << 24);
  }
  static unsigned char shift[] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                                  5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                                  4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                  6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
  static uint32_t K[] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
  uint32_t A = hash[0], B = hash[1], C = hash[2], D = hash[3];
  for (size_t i = 0; i < 64; ++i) {
    uint32_t F, g;
    if (i < 16) {
      F = (B & C) | ((~B) & D);
      g = i;
    } else if (i < 32) {
      F = (D & B) | ((~D) & C);
      g = (5 * i + 1) % 16;
    } else if (i < 48) {
      F = B ^ C ^ D;
      g = (3 * i + 5) % 16;
    } else {
      F = C ^ (B | (~D));
      g = (7 * i) % 16;
    }
    uint32_t dTemp = D;
    D = C;
    C = B;
    uint32_t x = A + F + K[i] + M[g];
    B += (x << shift[i] | (x >> (32 - shift[i])));
    A = dTemp;
  }
  hash[0] += A;
  hash[1] += B;
  hash[2] += C;
  hash[3] += D;
}

static std::string md5hex(const std::string & input) {
  uint32_t hash[] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
  const char *data = input.data();
  size_t in_len = input.size();
  size_t offset = 0;
  while (offset + 64 <= in_len) {
    md5_add64(hash, data + offset);
    offset += 64;
  }
  char buffer[64];
  memcpy(buffer, data + offset, in_len - offset);
  offset = in_len - offset;
  buffer[offset] = (char)0x80;
  memset(buffer + offset + 1, 0, 64 - offset - 1);
  if (offset > 55) {
    md5_add64(hash, buffer);
    memset(buffer, 0, 64);
  }
  unsigned long long bit_len = in_len << 3;
  buffer[56] = (bit_len >> 0) & 0xff;
  buffer[57] = (bit_len >> 8) & 0xff;
  buffer[58] = (bit_len >> 16) & 0xff;
  buffer[59] = (bit_len >> 24) & 0xff;
  buffer[60] = (bit_len >> 32) & 0xff;
  buffer[61] = (bit_len >> 40) & 0xff;
  buffer[62] = (bit_len >> 48) & 0xff;
  buffer[63] = (bit_len >> 54) & 0xff;
  md5_add64(hash, buffer);
  char out[16];
  for (int i = 0; i < 4; ++i) {
    out[i * 4 + 0] = (hash[i] >> 0) & 0xff;
    out[i * 4 + 1] = (hash[i] >> 8) & 0xff;
    out[i * 4 + 2] = (hash[i] >> 16) & 0xff;
    out[i * 4 + 3] = (hash[i] >> 24) & 0xff;
  }
  std::stringstream ss;
  for (int i = 0; i < 16; ++i) { ss << std::hex << std::setw(2) << std::setfill('0') << (unsigned)(uint8_t)out[i]; }
  return ss.str();
}

// --- MIME type lookup ---

static const char *mimeForExt(const std::string & path) {
  static const struct {
      const char *ext;
      const char *mime;
  } table[] = {
    {".html", "text/html; charset=utf-8"},
    {".htm", "text/html; charset=utf-8"},
    {".js", "application/javascript; charset=utf-8"},
    {".mjs", "application/javascript; charset=utf-8"},
    {".css", "text/css; charset=utf-8"},
    {".json", "application/json"},
    {".svg", "image/svg+xml"},
    {".png", "image/png"},
    {".jpg", "image/jpeg"},
    {".jpeg", "image/jpeg"},
    {".gif", "image/gif"},
    {".ico", "image/x-icon"},
    {".woff", "font/woff"},
    {".woff2", "font/woff2"},
    {".ttf", "font/ttf"},
    {".map", "application/json"},
    {".swf", "application/x-shockwave-flash"},
    {".xml", "text/xml"},
    {".txt", "text/plain"},
    {".webp", "image/webp"},
  };
  size_t dot = path.rfind('.');
  if (dot == std::string::npos) return "application/octet-stream";
  std::string ext = path.substr(dot);
  for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); ++i) {
    if (ext == table[i].ext) return table[i].mime;
  }
  return "application/octet-stream";
}

// --- File reading ---

static std::string readFile(const std::string & path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    std::cerr << "embedfs: cannot open " << path << std::endl;
    return "";
  }
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// --- C string escaping (from sourcery.cpp) ---

static void writeEscaped(std::ofstream & out, const std::string & data, const std::string & varName) {
  out << "static const char " << varName << "_data[] =" << std::endl << "  \"";
  uint32_t col = 0;
  bool sawQ = false;
  for (size_t pos = 0; pos < data.size(); ++pos) {
    unsigned char c = data[pos];
    switch (c) {
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      case '\\': out << "\\\\"; break;
      case '\"': out << "\\\""; break;
      case '?':
        if (sawQ) out << "\"\"";
        out << "?";
        sawQ = true;
        break;
      default:
        if (c < 32 || c > 126) {
          out << '\\' << std::oct << std::setw(3) << std::setfill('0') << (unsigned)c << std::dec;
        } else {
          out << c;
        }
        sawQ = false;
    }
    ++col;
    if (col >= 80) {
      out << "\" \\" << std::endl << "  \"";
      col = 0;
    }
  }
  out << "\";" << std::endl;
}

// --- Manifest parsing ---

static bool parseManifest(const std::string & path, std::vector<VFSEntry> & entries) {
  std::ifstream f(path);
  if (!f) {
    std::cerr << "embedfs: cannot open manifest " << path << std::endl;
    return false;
  }
  std::string line;
  while (std::getline(f, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream iss(line);
    VFSEntry e;
    if (!(iss >> e.vpath >> e.filepath)) {
      std::cerr << "embedfs: bad manifest line: " << line << std::endl;
      return false;
    }
    entries.push_back(e);
  }
  return true;
}

// --- Main ---

int main(int argc, char *argv[]) {
  std::string outputPath;
  std::string prefix;
  std::string manifestPath;
  std::string basedir;
  std::vector<VFSEntry> entries;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--output" && i + 1 < argc) {
      outputPath = argv[++i];
    } else if (arg == "--prefix" && i + 1 < argc) {
      prefix = argv[++i];
    } else if (arg == "--manifest" && i + 1 < argc) {
      manifestPath = argv[++i];
    } else if (arg == "--basedir" && i + 1 < argc) {
      basedir = argv[++i];
    } else if (arg == "--vpath" && i + 2 < argc) {
      VFSEntry e;
      e.vpath = argv[++i];
      e.filepath = argv[++i];
      entries.push_back(e);
    } else {
      std::cerr << "Usage: " << argv[0]
                << " --output <out.h> --prefix <prefix> [--vpath <vpath> <file>]... [--manifest <file>]" << std::endl;
      return 1;
    }
  }

  if (outputPath.empty() || prefix.empty()) {
    std::cerr << "embedfs: --output and --prefix are required" << std::endl;
    return 1;
  }

  if (!manifestPath.empty()) {
    if (!parseManifest(manifestPath, entries)) return 1;
  }

  if (basedir.size()) {
    for (size_t i = 0; i < entries.size(); ++i) {
      if (entries[i].filepath[0] != '/') { entries[i].filepath = basedir + "/" + entries[i].filepath; }
    }
  }

  if (entries.empty()) {
    std::cerr << "embedfs: no files specified" << std::endl;
    return 1;
  }

  std::ofstream out(outputPath);
  if (!out) {
    std::cerr << "embedfs: cannot write to " << outputPath << std::endl;
    return 1;
  }

  // Read all files upfront
  std::vector<std::string> contents(entries.size());
  for (size_t i = 0; i < entries.size(); ++i) {
    contents[i] = readFile(entries[i].filepath);
    if (contents[i].empty()) {
      std::cerr << "embedfs: warning: " << entries[i].filepath << " is empty or unreadable" << std::endl;
    }
  }

  out << "#include <mist/embedded_fs.h>" << std::endl << std::endl;

  // Write each file's data as a C string
  for (size_t i = 0; i < entries.size(); ++i) {
    std::string varName = prefix + "_" + std::to_string(i);
    writeEscaped(out, contents[i], varName);
    out << std::endl;
  }

  // Write the VFS table
  out << "static const EmbeddedFile " << prefix << "_vfs[] = {" << std::endl;
  for (size_t i = 0; i < entries.size(); ++i) {
    std::string varName = prefix + "_" + std::to_string(i);
    std::string etag = md5hex(contents[i]);
    const char *mime = mimeForExt(entries[i].vpath);
    out << "  {\"" << entries[i].vpath << "\", " << varName << "_data, " << contents[i].size() << ", \"" << mime
        << "\", \"" << etag << "\"}," << std::endl;
  }
  out << "};" << std::endl;
  out << "static const int " << prefix << "_vfs_count = " << entries.size() << ";" << std::endl;

  out.close();
  return 0;
}

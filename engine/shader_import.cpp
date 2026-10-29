// SPDX-License-Identifier: GPL-3.0-or-later
// The device's shader import: see shader_import.h.
#include "engine/shader_import.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <string>

#include "engine/shader_gles3.h"
#include "engine/unity_file.h"

using namespace UnityBridge;

namespace {

void Put(char* log, size_t cap, const std::string& s) {
  if (log && cap) std::snprintf(log, cap, "%s", s.c_str());
}

class Mapped {
public:
  explicit Mapped(const char* path) {
    fd_ = open(path, O_RDONLY);
    struct stat st;
    if (fd_ < 0 || fstat(fd_, &st) != 0 || st.st_size <= 0) return;
    size = size_t(st.st_size);
    void* p = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd_, 0);
    if (p != MAP_FAILED) data = static_cast<const uint8_t*>(p);
  }
  ~Mapped() {
    if (data) munmap(const_cast<uint8_t*>(data), size);
    if (fd_ >= 0) close(fd_);
  }
  const uint8_t* data = nullptr;
  size_t size = 0;

private:
  int fd_ = -1;
};

} // namespace

extern "C" int ub_shader_import_version(void) { return 1; }

extern "C" int ub_shader_import(const char* src, const char* dst, char* log, size_t logcap) {
  Mapped in(src);
  if (!in.data) {
    Put(log, logcap, "cannot read");
    return 0;
  }
  // The metadata alone says whether there is a Shader at all; the data is only read for those files.
  std::string error;
  const auto file = UnityFile::Parse(in.data, in.size, &error);
  if (!file) {
    Put(log, logcap, error);
    return 0;
  }
  if (std::find(file->typeClassIds.begin(), file->typeClassIds.end(), 48) == file->typeClassIds.end()) {
    Put(log, logcap, "no shaders");
    return 0;
  }
  ShaderGles3::Stats stats;
  const auto out = ShaderGles3::ConvertFile(in.data, in.size, false, &stats, &error);
  // The first failure reasons, for the log.
  std::string failures;
  size_t from = 0;
  for (int lines = 0; lines < 3 && from < stats.log.size(); ++lines) {
    const size_t at = stats.log.find("not translated:", from);
    if (at == std::string::npos) break;
    const size_t end = stats.log.find('\n', at);
    failures += "; " + stats.log.substr(at, end - at);
    from = end;
  }
  if (!out) {
    Put(log, logcap, error + failures);
    return 0;
  }
  const std::string tmp = std::string(dst) + ".tmp";
  FILE* f = std::fopen(tmp.c_str(), "wb");
  const bool ok = f && std::fwrite(out->data(), 1, out->size(), f) == out->size();
  if (f && std::fclose(f) != 0) {
    std::remove(tmp.c_str());
    Put(log, logcap, "write failed");
    return -1;
  }
  if (!ok || std::rename(tmp.c_str(), dst) != 0) {
    std::remove(tmp.c_str());
    Put(log, logcap, "write failed");
    return -1;
  }
  Put(log, logcap, std::to_string(stats.shaders) + " shaders, " + std::to_string(stats.programs) +
                       " GLES3 programs, " + std::to_string(stats.failed) + " not translated" + failures);
  return 1;
}

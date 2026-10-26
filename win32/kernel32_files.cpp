// SPDX-License-Identifier: GPL-3.0-or-later
// KERNEL32 file functions over POSIX: paths, files, directories, enumeration and file mappings.
// Synchronous only: overlapped I/O is refused the way Windows refuses it for files opened
// without FILE_FLAG_OVERLAPPED.
#include "common.h"

#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <dirent.h>
#include <fcntl.h>
#include <map>
#include <memory>
#include <mutex>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace UnityBridge::Win32 {

DWORD ErrorFromErrno(int error) {
  switch (error) {
  case ENOENT: return ERROR_FILE_NOT_FOUND;
  case ENOTDIR: return ERROR_PATH_NOT_FOUND;
  case EACCES: case EPERM: case EROFS: return ERROR_ACCESS_DENIED;
  case EEXIST: return ERROR_ALREADY_EXISTS;
  case ENOTEMPTY: return ERROR_DIR_NOT_EMPTY;
  case EMFILE: case ENFILE: return ERROR_TOO_MANY_OPEN_FILES;
  case ENOMEM: return ERROR_NOT_ENOUGH_MEMORY;
  case EBADF: return ERROR_INVALID_HANDLE;
  default: return ERROR_INVALID_PARAMETER;
  }
}

DWORD CopyWideOut(std::u16string_view text, char16_t* out, DWORD capacity) {
  if (out == nullptr || capacity <= text.size()) return static_cast<DWORD>(text.size() + 1);
  std::memcpy(out, text.data(), text.size() * 2);
  out[text.size()] = 0;
  return static_cast<DWORD>(text.size());
}

namespace {

// ---- paths -------------------------------------------------------------------------------

std::mutex g_pathMutex;
std::string g_profileRoot = "/tmp/UnityBridge/Profile";
constexpr std::string_view kProfile = "/users/player";  // C:\Users\Player, compared in lower case

bool StartsWithFolder(const std::string& path, std::string_view folder) {
  return path.size() >= folder.size() && Lower(path.substr(0, folder.size())) == folder &&
         (path.size() == folder.size() || path[folder.size()] == '/');
}

} // namespace

void SetProfileRoot(std::string hostDirectory) {
  std::scoped_lock lock {g_pathMutex};
  while (hostDirectory.size() > 1 && hostDirectory.back() == '/') hostDirectory.pop_back();
  g_profileRoot = std::move(hostDirectory);
}

std::string HostPath(std::u16string_view windowsPath) {
  std::string path = ToUtf8(windowsPath);
  for (auto& ch : path) if (ch == '\\') ch = '/';
  if (path.rfind("//?/", 0) == 0 || path.rfind("//./", 0) == 0) path.erase(0, 4);  // \\?\ prefix
  if (path.size() >= 2 && path[1] == ':' && std::isalpha(static_cast<unsigned char>(path[0]))) path.erase(0, 2);
  if (path.empty()) path = "/";
  std::scoped_lock lock {g_pathMutex};
  if (StartsWithFolder(path, kProfile)) path = g_profileRoot + path.substr(kProfile.size());
  return path;  // a path without a leading slash stays relative to the current directory
}

std::u16string WindowsPath(std::string_view hostPath) {
  std::string path(hostPath);
  {
    std::scoped_lock lock {g_pathMutex};
    if (StartsWithFolder(path, Lower(g_profileRoot)) || path.rfind(g_profileRoot, 0) == 0) {
      path = "/Users/Player" + path.substr(g_profileRoot.size());
    }
  }
  if (!path.empty() && path[0] == '/') path = "C:" + path;
  for (auto& ch : path) if (ch == '/') ch = '\\';
  return ToUtf16(path);
}

namespace {

// mkdir -p
void MakeDirectories(const std::string& path) {
  for (size_t slash = path.find('/', 1); ; slash = path.find('/', slash + 1)) {
    ::mkdir(path.substr(0, slash).c_str(), 0755);
    if (slash == std::string::npos) break;
  }
}

std::string ArgPath(CallContext& c, unsigned index) { return HostPath(ReadWide(c.Ptr<const char16_t>(index))); }

// ---- time and attributes -----------------------------------------------------------------

constexpr DWORD FILE_ATTRIBUTE_READONLY = 0x1, FILE_ATTRIBUTE_DIRECTORY = 0x10, FILE_ATTRIBUTE_NORMAL = 0x80;
constexpr DWORD INVALID_FILE_ATTRIBUTES = 0xffffffffu;

uint64_t FileTime(const timespec& t) {
  return static_cast<uint64_t>(t.tv_sec) * 10'000'000ull + static_cast<uint64_t>(t.tv_nsec) / 100 + 116444736000000000ull;
}

const timespec& ModifiedTime(const struct stat& st) {
#if defined(__APPLE__)
  return st.st_mtimespec;
#else
  return st.st_mtim;
#endif
}

DWORD Attributes(const struct stat& st) {
  DWORD a = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : 0;
  if (!(st.st_mode & S_IWUSR)) a |= FILE_ATTRIBUTE_READONLY;
  return a ? a : FILE_ATTRIBUTE_NORMAL;
}

// WIN32_FILE_ATTRIBUTE_DATA and the first 36 bytes of WIN32_FIND_DATAW share this layout.
void StoreAttributeData(uint64_t out, const struct stat& st) {
  const uint64_t t = FileTime(ModifiedTime(st));
  const auto size = static_cast<uint64_t>(S_ISDIR(st.st_mode) ? 0 : st.st_size);
  Store<DWORD>(out, Attributes(st));
  Store<uint64_t>(out + 4, t);    // creation (not tracked separately)
  Store<uint64_t>(out + 12, t);   // last access
  Store<uint64_t>(out + 20, t);   // last write
  Store<DWORD>(out + 28, static_cast<DWORD>(size >> 32));
  Store<DWORD>(out + 32, static_cast<DWORD>(size));
}

// ---- file objects ------------------------------------------------------------------------

struct File {
  int fd = -1;
  bool directory = false;
  ~File() { if (fd >= 0) ::close(fd); }
};

struct Find {
  DIR* dir = nullptr;
  std::string folder, pattern;  // pattern in lower case, '*' and '?' wildcards
  ~Find() { if (dir) closedir(dir); }
};

struct Mapping {
  int fd = -1;        // -1: backed by memory only
  uint64_t size = 0;
  bool writable = false;
  ~Mapping() { if (fd >= 0) ::close(fd); }
};

std::mutex g_mutex;
std::map<uint64_t, std::shared_ptr<File>> g_files;
std::map<uint64_t, std::shared_ptr<Find>> g_finds;
std::map<uint64_t, std::shared_ptr<Mapping>> g_mappings;
std::map<uint64_t, size_t> g_views;  // address -> length
uint64_t g_nextHandle = 0x4000'0000;  // a range apart from kernel32_sync.cpp's handles

uint64_t NewHandle() {
  const uint64_t h = g_nextHandle;
  g_nextHandle += 4;
  return h;
}

std::shared_ptr<File> FindFile(uint64_t handle) {
  std::scoped_lock lock {g_mutex};
  const auto it = g_files.find(handle);
  return it == g_files.end() ? nullptr : it->second;
}

constexpr DWORD GENERIC_READ = 0x80000000u, GENERIC_WRITE = 0x40000000u, GENERIC_ALL = 0x10000000u;
constexpr DWORD FILE_WRITE_DATA = 0x2, FILE_APPEND_DATA = 0x4;
constexpr DWORD CREATE_NEW = 1, CREATE_ALWAYS = 2, OPEN_EXISTING = 3, OPEN_ALWAYS = 4, TRUNCATE_EXISTING = 5;
constexpr DWORD FILE_FLAG_BACKUP_SEMANTICS = 0x02000000u, FILE_FLAG_OVERLAPPED = 0x40000000u;

uint64_t CreateFileW(CallContext& c) {
  const auto path = ArgPath(c, 0);
  const auto access = static_cast<DWORD>(c.Arg(1));
  const auto disposition = static_cast<DWORD>(c.Arg(4));
  const auto flags = static_cast<DWORD>(c.Arg(5));
  if (flags & FILE_FLAG_OVERLAPPED) return SetError(ERROR_NOT_SUPPORTED, kInvalidHandle);

  struct stat st {};
  const bool existed = ::stat(path.c_str(), &st) == 0;
  if (existed && S_ISDIR(st.st_mode)) {
    // Windows opens a directory only with FILE_FLAG_BACKUP_SEMANTICS.
    if (!(flags & FILE_FLAG_BACKUP_SEMANTICS)) return SetError(ERROR_ACCESS_DENIED, kInvalidHandle);
    auto file = std::make_shared<File>();
    file->fd = ::open(path.c_str(), O_RDONLY);
    file->directory = true;
    if (file->fd < 0) return SetError(ErrorFromErrno(errno), kInvalidHandle);
    std::scoped_lock lock {g_mutex};
    const auto h = NewHandle();
    g_files[h] = std::move(file);
    return SetError(ERROR_SUCCESS, h);
  }

  const bool write = access & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA);
  const bool read = (access & (GENERIC_READ | GENERIC_ALL)) || !write;
  int oflags = write ? (read ? O_RDWR : O_WRONLY) : O_RDONLY;
  switch (disposition) {
  case CREATE_NEW: oflags |= O_CREAT | O_EXCL; break;
  case CREATE_ALWAYS: oflags |= O_CREAT | O_TRUNC; break;
  case OPEN_EXISTING: break;
  case OPEN_ALWAYS: oflags |= O_CREAT; break;
  case TRUNCATE_EXISTING: oflags |= O_TRUNC; break;
  default: return SetError(ERROR_INVALID_PARAMETER, kInvalidHandle);
  }
  const int fd = ::open(path.c_str(), oflags | O_CLOEXEC, 0644);
  if (fd < 0) {
    const DWORD error = (errno == EEXIST && disposition == CREATE_NEW) ? ERROR_FILE_EXISTS : ErrorFromErrno(errno);
    return SetError(error, kInvalidHandle);
  }
  auto file = std::make_shared<File>();
  file->fd = fd;
  std::scoped_lock lock {g_mutex};
  const auto h = NewHandle();
  g_files[h] = std::move(file);
  // CREATE_ALWAYS and OPEN_ALWAYS succeed on an existing file with ERROR_ALREADY_EXISTS set.
  const bool reportExisting = existed && (disposition == CREATE_ALWAYS || disposition == OPEN_ALWAYS);
  return SetError(reportExisting ? ERROR_ALREADY_EXISTS : ERROR_SUCCESS, h);
}

constexpr uint64_t kStdOut = 0x7ffc'0000'0011ull, kStdErr = 0x7ffc'0000'0012ull;  // kernel32.cpp

uint64_t ReadFile(CallContext& c) {
  if (c.Arg(4)) return SetError(ERROR_NOT_SUPPORTED, kFalse);  // OVERLAPPED
  const auto file = FindFile(c.Arg(0));
  if (!file || file->directory) return SetError(ERROR_INVALID_HANDLE, kFalse);
  auto* buffer = c.Ptr<uint8_t>(1);
  size_t want = static_cast<DWORD>(c.Arg(2)), done = 0;
  while (done < want) {
    const auto n = ::read(file->fd, buffer + done, want - done);
    if (n < 0) {
      if (errno == EINTR) continue;
      Store<DWORD>(c.Arg(3), static_cast<DWORD>(done));
      return SetError(ErrorFromErrno(errno), kFalse);
    }
    if (n == 0) break;  // end of file: success with fewer bytes
    done += static_cast<size_t>(n);
  }
  Store<DWORD>(c.Arg(3), static_cast<DWORD>(done));
  return kTrue;
}

uint64_t WriteFile(CallContext& c) {
  if (c.Arg(4)) return SetError(ERROR_NOT_SUPPORTED, kFalse);
  const auto length = static_cast<DWORD>(c.Arg(2));
  if (c.Arg(0) == kStdOut || c.Arg(0) == kStdErr) {
    std::fwrite(c.Ptr<const char>(1), 1, length, stderr);
    Store<DWORD>(c.Arg(3), length);
    return kTrue;
  }
  const auto file = FindFile(c.Arg(0));
  if (!file || file->directory) return SetError(ERROR_INVALID_HANDLE, kFalse);
  const auto* buffer = c.Ptr<const uint8_t>(1);
  size_t done = 0;
  while (done < length) {
    const auto n = ::write(file->fd, buffer + done, length - done);
    if (n < 0) {
      if (errno == EINTR) continue;
      Store<DWORD>(c.Arg(3), static_cast<DWORD>(done));
      return SetError(ErrorFromErrno(errno), kFalse);
    }
    done += static_cast<size_t>(n);
  }
  Store<DWORD>(c.Arg(3), static_cast<DWORD>(done));
  return kTrue;
}

uint64_t SetFilePointerEx(CallContext& c) {
  const auto file = FindFile(c.Arg(0));
  if (!file) return SetError(ERROR_INVALID_HANDLE, kFalse);
  const auto distance = static_cast<int64_t>(c.Arg(1));
  const auto method = static_cast<DWORD>(c.Arg(3));  // FILE_BEGIN 0, FILE_CURRENT 1, FILE_END 2
  if (method > 2) return SetError(ERROR_INVALID_PARAMETER, kFalse);
  const off_t position = ::lseek(file->fd, distance, method == 0 ? SEEK_SET : method == 1 ? SEEK_CUR : SEEK_END);
  if (position < 0) return SetError(ERROR_INVALID_PARAMETER, kFalse);
  Store<int64_t>(c.Arg(2), position);
  return kTrue;
}

uint64_t SetFilePointer(CallContext& c) {
  constexpr DWORD INVALID_SET_FILE_POINTER = 0xffffffffu;
  const auto file = FindFile(c.Arg(0));
  if (!file) return SetError(ERROR_INVALID_HANDLE, INVALID_SET_FILE_POINTER);
  int64_t distance = static_cast<int32_t>(c.Arg(1));
  if (c.Arg(2)) distance = static_cast<int64_t>((uint64_t {Load<uint32_t>(c.Arg(2))} << 32) | static_cast<DWORD>(c.Arg(1)));
  const auto method = static_cast<DWORD>(c.Arg(3));
  if (method > 2) return SetError(ERROR_INVALID_PARAMETER, INVALID_SET_FILE_POINTER);
  const off_t position = ::lseek(file->fd, distance, method == 0 ? SEEK_SET : method == 1 ? SEEK_CUR : SEEK_END);
  if (position < 0) return SetError(ERROR_INVALID_PARAMETER, INVALID_SET_FILE_POINTER);
  if (c.Arg(2)) Store<uint32_t>(c.Arg(2), static_cast<uint32_t>(static_cast<uint64_t>(position) >> 32));
  return SetError(ERROR_SUCCESS, static_cast<DWORD>(position));
}

uint64_t GetFileSizeEx(CallContext& c) {
  const auto file = FindFile(c.Arg(0));
  struct stat st {};
  if (!file || ::fstat(file->fd, &st) != 0) return SetError(ERROR_INVALID_HANDLE, kFalse);
  Store<int64_t>(c.Arg(1), st.st_size);
  return kTrue;
}

uint64_t FlushFileBuffers(CallContext& c) {
  const auto file = FindFile(c.Arg(0));
  if (!file) return SetError(ERROR_INVALID_HANDLE, kFalse);
  ::fsync(file->fd);
  return kTrue;
}

uint64_t SetEndOfFile(CallContext& c) {
  const auto file = FindFile(c.Arg(0));
  if (!file) return SetError(ERROR_INVALID_HANDLE, kFalse);
  const off_t position = ::lseek(file->fd, 0, SEEK_CUR);
  return (position >= 0 && ::ftruncate(file->fd, position) == 0) ? kTrue : SetError(ErrorFromErrno(errno), kFalse);
}

uint64_t GetFileInformationByHandleEx(CallContext& c) {
  // FileStandardInfo (1): AllocationSize, EndOfFile (int64), NumberOfLinks (u32), DeletePending, Directory (u8).
  const auto file = FindFile(c.Arg(0));
  struct stat st {};
  if (!file || ::fstat(file->fd, &st) != 0) return SetError(ERROR_INVALID_HANDLE, kFalse);
  if (static_cast<DWORD>(c.Arg(1)) != 1 || c.Arg(3) < 24) return SetError(ERROR_INVALID_PARAMETER, kFalse);
  const uint64_t out = c.Arg(2);
  Store<int64_t>(out, st.st_size);
  Store<int64_t>(out + 8, st.st_size);
  Store<DWORD>(out + 16, static_cast<DWORD>(st.st_nlink));
  Store<uint8_t>(out + 20, 0);
  Store<uint8_t>(out + 21, S_ISDIR(st.st_mode) ? 1 : 0);
  return kTrue;
}

uint64_t SetFileInformationByHandle(CallContext&) { return SetError(ERROR_NOT_SUPPORTED, kFalse); }
uint64_t GetOverlappedResult(CallContext&) { return SetError(ERROR_INVALID_PARAMETER, kFalse); }
uint64_t CancelIoEx(CallContext&) { return SetError(ERROR_NOT_SUPPORTED, kFalse); }

// ---- attributes and directories ----------------------------------------------------------

uint64_t GetFileAttributesExW(CallContext& c) {
  struct stat st {};
  if (::stat(ArgPath(c, 0).c_str(), &st) != 0) return SetError(ErrorFromErrno(errno), kFalse);
  StoreAttributeData(c.Arg(2), st);
  return kTrue;
}

uint64_t SetFileAttributesW(CallContext& c) {
  struct stat st {};
  return ::stat(ArgPath(c, 0).c_str(), &st) == 0 ? kTrue : SetError(ErrorFromErrno(errno), kFalse);
}

uint64_t CreateDirectoryW(CallContext& c) {
  return ::mkdir(ArgPath(c, 0).c_str(), 0755) == 0 ? kTrue : SetError(ErrorFromErrno(errno), kFalse);
}
uint64_t RemoveDirectoryW(CallContext& c) {
  return ::rmdir(ArgPath(c, 0).c_str()) == 0 ? kTrue : SetError(ErrorFromErrno(errno), kFalse);
}
uint64_t DeleteFileW(CallContext& c) {
  return ::unlink(ArgPath(c, 0).c_str()) == 0 ? kTrue : SetError(ErrorFromErrno(errno), kFalse);
}

uint64_t MoveFileExW(CallContext& c) {
  constexpr DWORD MOVEFILE_REPLACE_EXISTING = 0x1;
  const auto from = ArgPath(c, 0), to = ArgPath(c, 1);
  struct stat st {};
  if (!(static_cast<DWORD>(c.Arg(2)) & MOVEFILE_REPLACE_EXISTING) && ::stat(to.c_str(), &st) == 0) {
    return SetError(ERROR_ALREADY_EXISTS, kFalse);
  }
  return ::rename(from.c_str(), to.c_str()) == 0 ? kTrue : SetError(ErrorFromErrno(errno), kFalse);
}

uint64_t ReplaceFileW(CallContext& c) {
  // ReplaceFileW(replaced, replacement, backup, ...): the backup copy is not made.
  return ::rename(ArgPath(c, 1).c_str(), ArgPath(c, 0).c_str()) == 0 ? kTrue : SetError(ErrorFromErrno(errno), kFalse);
}

uint64_t CopyFileExW(CallContext& c) {
  constexpr DWORD COPY_FILE_FAIL_IF_EXISTS = 0x1;
  const auto from = ArgPath(c, 0), to = ArgPath(c, 1);
  const int in = ::open(from.c_str(), O_RDONLY | O_CLOEXEC);
  if (in < 0) return SetError(ErrorFromErrno(errno), kFalse);
  const int out = ::open(to.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC |
                                         ((static_cast<DWORD>(c.Arg(5)) & COPY_FILE_FAIL_IF_EXISTS) ? O_EXCL : O_TRUNC), 0644);
  if (out < 0) {
    const int error = errno;
    ::close(in);
    return SetError(error == EEXIST ? ERROR_FILE_EXISTS : ErrorFromErrno(error), kFalse);
  }
  char buffer[64 * 1024];
  bool ok = true;
  for (;;) {
    const auto n = ::read(in, buffer, sizeof(buffer));
    if (n == 0) break;
    if (n < 0 || ::write(out, buffer, static_cast<size_t>(n)) != n) { ok = false; break; }
  }
  ::close(in);
  ::close(out);
  return ok ? kTrue : SetError(ERROR_ACCESS_DENIED, kFalse);
}

uint64_t DeleteVolumeMountPointW(CallContext&) { return SetError(ERROR_NOT_SUPPORTED, kFalse); }
uint64_t GetLogicalDrives(CallContext&) { return 1u << 2; }  // C: only

// ---- enumeration -------------------------------------------------------------------------

bool Matches(const char* pattern, const char* name) {  // both lower case
  if (*pattern == 0) return *name == 0;
  if (*pattern == '*') return Matches(pattern + 1, name) || (*name && Matches(pattern, name + 1));
  if (*name == 0) return false;
  return (*pattern == '?' || *pattern == *name) && Matches(pattern + 1, name + 1);
}

// Fills WIN32_FIND_DATAW (592 bytes) with the next matching entry. Returns false at the end.
bool NextEntry(Find& find, uint64_t out) {
  while (dirent* entry = readdir(find.dir)) {
    const std::string name = entry->d_name;
    if (!Matches(find.pattern.c_str(), Lower(name).c_str())) continue;
    struct stat st {};
    if (::stat((find.folder + "/" + name).c_str(), &st) != 0) continue;
    std::memset(reinterpret_cast<void*>(out), 0, 592);
    StoreAttributeData(out, st);
    const auto wide = ToUtf16(name);
    if (wide.size() >= 260) continue;
    std::memcpy(reinterpret_cast<void*>(out + 44), wide.data(), wide.size() * 2);  // cFileName
    return true;
  }
  return false;
}

uint64_t FindFirstFileExW(CallContext& c) {
  // FindFirstFileExW(name, infoLevel, findData, searchOp, filter, flags); FindExInfoBasic and
  // FindExInfoStandard fill the same fields here.
  const auto path = ArgPath(c, 0);
  const auto slash = path.find_last_of('/');
  auto find = std::make_shared<Find>();
  find->folder = slash == std::string::npos ? "." : (slash == 0 ? "/" : path.substr(0, slash));
  find->pattern = Lower(slash == std::string::npos ? path : path.substr(slash + 1));
  if (find->pattern == "*.*") find->pattern = "*";
  find->dir = opendir(find->folder.c_str());
  if (find->dir == nullptr) return SetError(errno == ENOENT ? ERROR_PATH_NOT_FOUND : ErrorFromErrno(errno), kInvalidHandle);
  if (!NextEntry(*find, c.Arg(2))) return SetError(ERROR_FILE_NOT_FOUND, kInvalidHandle);
  std::scoped_lock lock {g_mutex};
  const auto h = NewHandle();
  g_finds[h] = std::move(find);
  return h;
}

uint64_t FindNextFileW(CallContext& c) {
  std::shared_ptr<Find> find;
  {
    std::scoped_lock lock {g_mutex};
    const auto it = g_finds.find(c.Arg(0));
    if (it != g_finds.end()) find = it->second;
  }
  if (!find) return SetError(ERROR_INVALID_HANDLE, kFalse);
  return NextEntry(*find, c.Arg(1)) ? kTrue : SetError(ERROR_NO_MORE_FILES, kFalse);
}

uint64_t FindClose(CallContext& c) {
  std::scoped_lock lock {g_mutex};
  return g_finds.erase(c.Arg(0)) ? kTrue : SetError(ERROR_INVALID_HANDLE, kFalse);
}

// ---- path queries ------------------------------------------------------------------------

uint64_t GetFullPathNameW(CallContext& c) {
  const auto input = ReadWide(c.Ptr<const char16_t>(0));
  std::string host = HostPath(input);
  if (host.empty() || host[0] != '/') {
    char cwd[4096];
    if (::getcwd(cwd, sizeof(cwd)) == nullptr) return SetError(ERROR_INVALID_PARAMETER, 0);
    host = std::string(cwd) + "/" + host;
  }
  // Collapse "." and ".." the way Windows does, without touching the file system.
  std::vector<std::string> parts;
  size_t start = 0;
  while (start <= host.size()) {
    const auto end = host.find('/', start);
    const auto part = host.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (part == "..") { if (!parts.empty()) parts.pop_back(); }
    else if (!part.empty() && part != ".") parts.push_back(part);
    if (end == std::string::npos) break;
    start = end + 1;
  }
  std::string normalized;
  for (const auto& p : parts) normalized += "/" + p;
  if (normalized.empty()) normalized = "/";
  const auto full = WindowsPath(normalized);
  const auto result = CopyWideOut(full, c.Ptr<char16_t>(2), static_cast<DWORD>(c.Arg(1)));
  if (c.Arg(3) && result == full.size()) {  // lpFilePart: the last component
    const auto lastSlash = full.find_last_of(u'\\');
    Store<uint64_t>(c.Arg(3), c.Arg(2) + (lastSlash == std::u16string::npos ? 0 : (lastSlash + 1) * 2));
  }
  return result;
}

uint64_t GetLongPathNameW(CallContext& c) {
  const auto input = ReadWide(c.Ptr<const char16_t>(0));
  struct stat st {};
  if (::stat(HostPath(input).c_str(), &st) != 0) return SetError(ErrorFromErrno(errno), 0);
  return CopyWideOut(input, c.Ptr<char16_t>(1), static_cast<DWORD>(c.Arg(2)));
}

uint64_t GetCurrentDirectoryW(CallContext& c) {
  char cwd[4096];
  if (::getcwd(cwd, sizeof(cwd)) == nullptr) return SetError(ERROR_INVALID_PARAMETER, 0);
  return CopyWideOut(WindowsPath(cwd), c.Ptr<char16_t>(1), static_cast<DWORD>(c.Arg(0)));
}

uint64_t GetTempPathW(CallContext& c) {
  // Windows returns it with a trailing backslash.
  std::string temp;
  {
    std::scoped_lock lock {g_pathMutex};
    temp = g_profileRoot + "/AppData/Local/Temp";
  }
  MakeDirectories(temp);
  return CopyWideOut(WindowsPath(temp) + u"\\", c.Ptr<char16_t>(1), static_cast<DWORD>(c.Arg(0)));
}

// ---- file mappings -----------------------------------------------------------------------

// Views of a mapping without a file (INVALID_HANDLE_VALUE) do not share memory with each other yet.
constexpr DWORD PAGE_READWRITE = 0x04, PAGE_EXECUTE_READWRITE = 0x40;
constexpr DWORD FILE_MAP_WRITE = 0x2, FILE_MAP_COPY = 0x1;

uint64_t CreateFileMappingW(CallContext& c) {
  const uint64_t fileHandle = c.Arg(0);
  const auto protect = static_cast<DWORD>(c.Arg(2)) & 0xff;
  const uint64_t maximum = (uint64_t {static_cast<DWORD>(c.Arg(3))} << 32) | static_cast<DWORD>(c.Arg(4));
  auto mapping = std::make_shared<Mapping>();
  mapping->writable = protect == PAGE_READWRITE || protect == PAGE_EXECUTE_READWRITE;
  if (fileHandle == kInvalidHandle) {
    if (maximum == 0) return SetError(ERROR_INVALID_PARAMETER, 0);
    mapping->size = maximum;  // backed by memory only
  } else {
    const auto file = FindFile(fileHandle);
    struct stat st {};
    if (!file || ::fstat(file->fd, &st) != 0) return SetError(ERROR_INVALID_HANDLE, 0);
    mapping->fd = ::dup(file->fd);
    mapping->size = maximum ? maximum : static_cast<uint64_t>(st.st_size);
    if (maximum > static_cast<uint64_t>(st.st_size) && mapping->writable) ::ftruncate(mapping->fd, static_cast<off_t>(maximum));
  }
  std::scoped_lock lock {g_mutex};
  const auto h = NewHandle();
  g_mappings[h] = std::move(mapping);
  return h;
}

uint64_t OpenFileMappingW(CallContext&) { return SetError(ERROR_FILE_NOT_FOUND, 0); }  // named mappings: none

uint64_t MapViewOfFile(CallContext& c) {
  std::shared_ptr<Mapping> mapping;
  {
    std::scoped_lock lock {g_mutex};
    const auto it = g_mappings.find(c.Arg(0));
    if (it != g_mappings.end()) mapping = it->second;
  }
  if (!mapping) return SetError(ERROR_INVALID_HANDLE, 0);
  const auto access = static_cast<DWORD>(c.Arg(1));
  const uint64_t offset = (uint64_t {static_cast<DWORD>(c.Arg(2))} << 32) | static_cast<DWORD>(c.Arg(3));
  uint64_t length = c.Arg(4);
  if (offset > mapping->size) return SetError(ERROR_INVALID_PARAMETER, 0);
  if (length == 0) length = mapping->size - offset;
  const bool write = access & (FILE_MAP_WRITE | FILE_MAP_COPY);
  if (write && !mapping->writable && !(access & FILE_MAP_COPY)) return SetError(ERROR_ACCESS_DENIED, 0);
  const int prot = PROT_READ | (write ? PROT_WRITE : 0);
  void* p;
  if (mapping->fd < 0) {
    p = mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_ANON | MAP_SHARED, -1, 0);
  } else {
    const int share = (access & FILE_MAP_COPY) ? MAP_PRIVATE : MAP_SHARED;
    p = mmap(nullptr, length, prot, share, mapping->fd, static_cast<off_t>(offset));
  }
  if (p == MAP_FAILED) return SetError(ErrorFromErrno(errno), 0);
  std::scoped_lock lock {g_mutex};
  g_views[reinterpret_cast<uint64_t>(p)] = length;
  return reinterpret_cast<uint64_t>(p);
}

uint64_t UnmapViewOfFile(CallContext& c) {
  std::scoped_lock lock {g_mutex};
  const auto it = g_views.find(c.Arg(0));
  if (it == g_views.end()) return SetError(ERROR_INVALID_PARAMETER, kFalse);
  munmap(reinterpret_cast<void*>(it->first), it->second);
  g_views.erase(it);
  return kTrue;
}

} // namespace

bool CloseFileObject(uint64_t handle) {
  std::scoped_lock lock {g_mutex};
  return g_files.erase(handle) || g_mappings.erase(handle) || g_finds.erase(handle);
}

void RegisterKernel32Files(std::vector<Function>& out) {
  const std::string k = "kernel32.dll";
#define UB_FN(name) out.push_back({k, #name, &name})
  UB_FN(CreateFileW); UB_FN(ReadFile); UB_FN(WriteFile); UB_FN(SetFilePointer); UB_FN(SetFilePointerEx);
  UB_FN(GetFileSizeEx); UB_FN(FlushFileBuffers); UB_FN(SetEndOfFile); UB_FN(GetFileInformationByHandleEx);
  UB_FN(SetFileInformationByHandle); UB_FN(GetOverlappedResult); UB_FN(CancelIoEx);
  UB_FN(GetFileAttributesExW); UB_FN(SetFileAttributesW); UB_FN(CreateDirectoryW); UB_FN(RemoveDirectoryW);
  UB_FN(DeleteFileW); UB_FN(MoveFileExW); UB_FN(ReplaceFileW); UB_FN(CopyFileExW);
  UB_FN(DeleteVolumeMountPointW); UB_FN(GetLogicalDrives);
  UB_FN(FindFirstFileExW); UB_FN(FindNextFileW); UB_FN(FindClose);
  UB_FN(GetFullPathNameW); UB_FN(GetLongPathNameW); UB_FN(GetCurrentDirectoryW); UB_FN(GetTempPathW);
  UB_FN(CreateFileMappingW); UB_FN(OpenFileMappingW); UB_FN(MapViewOfFile); UB_FN(UnmapViewOfFile);
#undef UB_FN
}

} // namespace UnityBridge::Win32

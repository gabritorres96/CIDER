// POSIX subset of CIDER's libSystem: file descriptors, files, time and process identity, with Darwin's constants,
// structure layouts (x86_64, 64-bit inode variant) and errno values translated onto Win32. NOTHING here hands a Win32
// HANDLE, a CRT file descriptor, a Windows `struct _stat` or a Windows error code to the guest.
//
// Guest paths: the guest sees a POSIX tree whose "/" is RuntimeContext::root (a host directory chosen with
// `cider run --root`). Paths are normalised lexically ('.', '..' never leave the root), Windows-only syntax (drive
// letters, backslashes, streams, wildcards, reserved device names) is refused, and every existing path is checked
// again after resolving links so a junction/symlink cannot lead outside the root. This confines honest programs;
// it is NOT a security sandbox (guest machine code runs natively, see docs/SECURITY.md).
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstring>
#include <ctime>

#include "cider/libsystem.hpp"
#include "cider/log.hpp"
#include "cider/util.hpp"
#include "internal.hpp"

namespace cider::libsystem {

RuntimeContext::RuntimeContext() {
    fds.resize(3);
    for (int i = 0; i < 3; ++i) {
        fds[static_cast<size_t>(i)].kind = GuestFd::Kind::Console;
        fds[static_cast<size_t>(i)].console = i;
        fds[static_cast<size_t>(i)].readable = i == 0;
        fds[static_cast<size_t>(i)].writable = i != 0;
    }
}

namespace {

using abi::GuestRegs;
using abi::RegsReader;
using detail::fail_errno;
namespace fs = std::filesystem;

// ---- Darwin constants -----------------------------------------------------------------------------------------
constexpr int kO_ACCMODE = 3, kO_RDONLY = 0, kO_WRONLY = 1, kO_RDWR = 2, kO_NONBLOCK = 0x4, kO_APPEND = 0x8, kO_NOFOLLOW = 0x100,
              kO_CREAT = 0x200, kO_TRUNC = 0x400, kO_EXCL = 0x800, kO_CLOEXEC = 0x1000000;
constexpr int kS_IFMT = 0170000, kS_IFDIR = 0040000, kS_IFREG = 0100000, kS_IFCHR = 0020000;
constexpr int kF_OK = 0, kX_OK = 1, kW_OK = 2, kR_OK = 4;
constexpr int kSEEK_SET = 0, kSEEK_CUR = 1, kSEEK_END = 2;
constexpr int kCLOCK_REALTIME = 0, kCLOCK_MONOTONIC_RAW = 4, kCLOCK_MONOTONIC = 6, kCLOCK_UPTIME_RAW = 8, kCLOCK_PROCESS_CPUTIME_ID = 12,
              kCLOCK_THREAD_CPUTIME_ID = 16;
constexpr size_t kMaxGuestFds = 256;

#ifdef _WIN32
int errno_from_win32(DWORD e) {
    switch (e) {
        case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: case ERROR_INVALID_NAME: case ERROR_BAD_PATHNAME: return detail::kENOENT;
        case ERROR_ACCESS_DENIED: case ERROR_SHARING_VIOLATION: case ERROR_LOCK_VIOLATION: case ERROR_NETWORK_ACCESS_DENIED: return detail::kEACCES;
        case ERROR_FILE_EXISTS: case ERROR_ALREADY_EXISTS: return detail::kEEXIST;
        case ERROR_DIRECTORY: return detail::kENOTDIR;
        case ERROR_TOO_MANY_OPEN_FILES: return detail::kEMFILE;
        case ERROR_NOT_ENOUGH_MEMORY: case ERROR_OUTOFMEMORY: return detail::kENOMEM;
        case ERROR_DISK_FULL: case ERROR_HANDLE_DISK_FULL: return detail::kENOSPC;
        case ERROR_INVALID_HANDLE: return detail::kEBADF;
        case ERROR_INVALID_PARAMETER: case ERROR_NEGATIVE_SEEK: return detail::kEINVAL;
        case ERROR_FILENAME_EXCED_RANGE: return detail::kENAMETOOLONG;
        case ERROR_DIR_NOT_EMPTY: return detail::kENOTEMPTY;
        case ERROR_BROKEN_PIPE: case ERROR_NO_DATA: return detail::kEPIPE;
        case ERROR_WRITE_PROTECT: return detail::kEROFS;
        default: return detail::kEIO;
    }
}

// ---- guest path -> host path ---------------------------------------------------------------------------------------
bool reserved_device_name(std::string name) {
    for (auto& c : name) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    const auto dot = name.find('.');
    if (dot != std::string::npos) name.resize(dot);
    while (!name.empty() && name.back() == ' ') name.pop_back();
    static const char* reserved[] = {"CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
                                     "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9", "CONIN$", "CONOUT$"};
    for (auto r : reserved)
        if (name == r) return true;
    return false;
}

// Returns 0 and the host path, or a Darwin errno.
int resolve_path(const char* guest, fs::path& host) {
    RuntimeContext* c = context();
    if (!guest) return detail::kEFAULT;
    if (!c || c->root.empty()) return detail::kEACCES;                 // file access is off unless a root was given
    const size_t len = strnlen(guest, 4097);
    if (len == 0) return detail::kENOENT;
    if (len > 4096) return detail::kENAMETOOLONG;
    std::string_view p(guest, len);
    for (unsigned char ch : p)
        if (ch < 0x20 || ch == '\\' || ch == ':' || ch == '*' || ch == '?' || ch == '"' || ch == '<' || ch == '>' || ch == '|' || ch == 0x7f) return detail::kEINVAL;
    if (sanitize_utf8(p) != p) return detail::kEINVAL;

    std::vector<std::string> parts;
    auto walk = [&](std::string_view path) {
        size_t i = 0;
        while (i <= path.size()) {
            size_t j = path.find('/', i);
            if (j == std::string_view::npos) j = path.size();
            std::string_view seg = path.substr(i, j - i);
            if (seg == "..") { if (!parts.empty()) parts.pop_back(); }
            else if (!seg.empty() && seg != ".") parts.emplace_back(seg);
            i = j + 1;
        }
    };
    walk(p);
    fs::path out = c->root;
    for (auto& seg : parts) {
        if (reserved_device_name(seg) || seg.size() > 255) return seg.size() > 255 ? detail::kENAMETOOLONG : detail::kEACCES;
        if (seg.back() == ' ' || seg.back() == '.') return detail::kEINVAL;   // Windows silently strips these: refuse instead
        out /= path_from_utf8(seg);
    }
    // Links/junctions inside the root must not lead outside it.
    std::error_code ec;
    fs::path canon = fs::weakly_canonical(out, ec);
    fs::path root_canon = fs::weakly_canonical(c->root, ec);
    if (!is_inside(root_canon, canon)) return detail::kEACCES;
    host = out;
    return 0;
}

GuestFd* fd_entry(int fd) {
    RuntimeContext* c = context();
    if (!c || fd < 0 || static_cast<size_t>(fd) >= c->fds.size() || c->fds[static_cast<size_t>(fd)].kind == GuestFd::Kind::Closed) return nullptr;
    return &c->fds[static_cast<size_t>(fd)];
}

int allocate_fd(RuntimeContext& c) {
    for (size_t i = 0; i < c.fds.size(); ++i)
        if (c.fds[i].kind == GuestFd::Kind::Closed) return static_cast<int>(i);
    if (c.fds.size() >= kMaxGuestFds) return -1;
    c.fds.emplace_back();
    return static_cast<int>(c.fds.size() - 1);
}

// ---- open / close / read / write / lseek ---------------------------------------------------------------------------------
// open(path, flags, ...)  -- variadic in C (mode is read only with O_CREAT)
int open_impl(const char* path, int flags);
void w_open(GuestRegs* r) {
    RegsReader a(*r);
    const char* path = reinterpret_cast<const char*>(a.next_int());
    const int flags = static_cast<int>(static_cast<int32_t>(a.next_int()));
    r->ret_rax = static_cast<uint64_t>(static_cast<int64_t>(open_impl(path, flags)));
}
// Opens `path` with Darwin flags; returns the guest descriptor, or -1 with errno set. Binary mode always.
int open_impl(const char* path, int flags) {
    RuntimeContext* c = context();
    if (!c) return static_cast<int>(fail_errno(detail::kEFAULT));
    const int known = kO_ACCMODE | kO_NONBLOCK | kO_APPEND | kO_NOFOLLOW | kO_CREAT | kO_TRUNC | kO_EXCL | kO_CLOEXEC;
    const int acc = flags & kO_ACCMODE;
    if ((flags & ~known) != 0 || acc == 3 || ((flags & kO_EXCL) && !(flags & kO_CREAT))) return static_cast<int>(fail_errno(detail::kEINVAL));
    fs::path host;
    if (int e = resolve_path(path, host)) return static_cast<int>(fail_errno(e));
    std::error_code ec;
    if (fs::is_directory(host, ec)) return static_cast<int>(fail_errno(detail::kEISDIR));   // no readdir support

    DWORD access = 0;
    if (acc == kO_RDONLY || acc == kO_RDWR) access |= GENERIC_READ;
    if (acc == kO_WRONLY || acc == kO_RDWR) access |= GENERIC_WRITE;
    DWORD disp;
    if (flags & kO_CREAT) disp = (flags & kO_EXCL) ? CREATE_NEW : ((flags & kO_TRUNC) ? CREATE_ALWAYS : OPEN_ALWAYS);
    else disp = (flags & kO_TRUNC) ? TRUNCATE_EXISTING : OPEN_EXISTING;
    if ((flags & kO_TRUNC) && acc == kO_RDONLY) return static_cast<int>(fail_errno(detail::kEINVAL));
    const int fd = allocate_fd(*c);
    if (fd < 0) return static_cast<int>(fail_errno(detail::kEMFILE));
    HANDLE h = CreateFileW(extended_path(host).c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, disp,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return static_cast<int>(fail_errno(errno_from_win32(GetLastError())));
    GuestFd& g = c->fds[static_cast<size_t>(fd)];
    g.kind = GuestFd::Kind::File;
    g.handle = h;
    g.readable = acc != kO_WRONLY;
    g.writable = acc != kO_RDONLY;
    g.append = (flags & kO_APPEND) != 0;
    return fd;
}

int w_close(int fd) {
    RuntimeContext* c = context();
    GuestFd* g = fd_entry(fd);
    if (!c || !g) return static_cast<int>(fail_errno(detail::kEBADF));
    if (g->kind == GuestFd::Kind::File && g->handle) CloseHandle(static_cast<HANDLE>(g->handle));
    *g = GuestFd{};
    return 0;
}

int64_t w_read(int fd, void* buf, uint64_t n) {
    GuestFd* g = fd_entry(fd);
    if (!g || !g->readable) return fail_errno(detail::kEBADF);
    if (!buf && n) return fail_errno(detail::kEFAULT);
    if (n == 0) return 0;
    HANDLE h = g->kind == GuestFd::Kind::Console ? GetStdHandle(STD_INPUT_HANDLE) : static_cast<HANDLE>(g->handle);
    if (!h || h == INVALID_HANDLE_VALUE) return 0;                      // no stdin: end of file
    DWORD got = 0;
    const DWORD want = static_cast<DWORD>(std::min<uint64_t>(n, 1u << 30));
    if (!ReadFile(h, buf, want, &got, nullptr)) {
        const DWORD e = GetLastError();
        if (e == ERROR_BROKEN_PIPE || e == ERROR_HANDLE_EOF) return 0;
        return fail_errno(errno_from_win32(e));
    }
    return got;
}

int64_t w_write_fd(int fd, const void* buf, uint64_t n) {
    GuestFd* g = fd_entry(fd);
    if (!g || !g->writable) return fail_errno(detail::kEBADF);
    if (!buf && n) return fail_errno(detail::kEFAULT);
    if (g->kind == GuestFd::Kind::Console) { console_write(g->console, static_cast<const char*>(buf), n); return static_cast<int64_t>(n); }
    HANDLE h = static_cast<HANDLE>(g->handle);
    if (g->append) { LARGE_INTEGER z{}; SetFilePointerEx(h, z, nullptr, FILE_END); }
    uint64_t done = 0;
    while (done < n) {
        DWORD w = 0;
        const DWORD chunk = static_cast<DWORD>(std::min<uint64_t>(n - done, 1u << 30));
        if (!WriteFile(h, static_cast<const char*>(buf) + done, chunk, &w, nullptr)) return done ? static_cast<int64_t>(done) : fail_errno(errno_from_win32(GetLastError()));
        if (w == 0) break;
        done += w;
    }
    return static_cast<int64_t>(done);
}

int64_t w_lseek(int fd, int64_t off, int whence) {
    GuestFd* g = fd_entry(fd);
    if (!g) return fail_errno(detail::kEBADF);
    if (g->kind != GuestFd::Kind::File) return fail_errno(detail::kESPIPE);
    DWORD method;
    switch (whence) {
        case kSEEK_SET: method = FILE_BEGIN; break;
        case kSEEK_CUR: method = FILE_CURRENT; break;
        case kSEEK_END: method = FILE_END; break;
        default: return fail_errno(detail::kEINVAL);
    }
    LARGE_INTEGER in, out;
    in.QuadPart = off;
    if (whence == kSEEK_SET && off < 0) return fail_errno(detail::kEINVAL);
    if (!SetFilePointerEx(static_cast<HANDLE>(g->handle), in, &out, method)) return fail_errno(errno_from_win32(GetLastError()));
    return out.QuadPart;
}

// ---- stat -------------------------------------------------------------------------------------------------------------------
// Darwin x86_64 `struct stat` (the 64-bit-inode layout, 144 bytes).
struct DarwinStat {
    int32_t st_dev; uint16_t st_mode; uint16_t st_nlink; uint64_t st_ino; uint32_t st_uid; uint32_t st_gid; int32_t st_rdev; int32_t pad0;
    int64_t atime_sec, atime_nsec, mtime_sec, mtime_nsec, ctime_sec, ctime_nsec, birth_sec, birth_nsec;
    int64_t st_size; int64_t st_blocks; int32_t st_blksize; uint32_t st_flags; uint32_t st_gen; int32_t st_lspare; int64_t st_qspare[2];
};
static_assert(sizeof(DarwinStat) == 144 && offsetof(DarwinStat, st_ino) == 8 && offsetof(DarwinStat, atime_sec) == 32 &&
              offsetof(DarwinStat, st_size) == 96 && offsetof(DarwinStat, st_blksize) == 112, "Darwin struct stat layout");

void filetime_to_unix(const FILETIME& ft, int64_t& sec, int64_t& nsec) {
    const uint64_t t = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;   // 100 ns since 1601
    const uint64_t kEpochDiff = 116444736000000000ull;
    if (t < kEpochDiff) { sec = 0; nsec = 0; return; }
    const uint64_t u = t - kEpochDiff;
    sec = static_cast<int64_t>(u / 10000000ull);
    nsec = static_cast<int64_t>((u % 10000000ull) * 100);
}

int fill_stat_from_handle(HANDLE h, DarwinStat* st) {
    BY_HANDLE_FILE_INFORMATION bi{};
    if (!GetFileInformationByHandle(h, &bi)) return errno_from_win32(GetLastError());
    std::memset(st, 0, sizeof *st);
    const bool dir = (bi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    const bool ro = (bi.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0;
    st->st_mode = static_cast<uint16_t>(dir ? (kS_IFDIR | (ro ? 0555 : 0755)) : (kS_IFREG | (ro ? 0444 : 0644)));
    st->st_dev = static_cast<int32_t>(bi.dwVolumeSerialNumber);
    st->st_nlink = static_cast<uint16_t>(std::min<DWORD>(bi.nNumberOfLinks, 0xffff));
    st->st_ino = (static_cast<uint64_t>(bi.nFileIndexHigh) << 32) | bi.nFileIndexLow;
    filetime_to_unix(bi.ftLastAccessTime, st->atime_sec, st->atime_nsec);
    filetime_to_unix(bi.ftLastWriteTime, st->mtime_sec, st->mtime_nsec);
    st->ctime_sec = st->mtime_sec; st->ctime_nsec = st->mtime_nsec;
    filetime_to_unix(bi.ftCreationTime, st->birth_sec, st->birth_nsec);
    st->st_size = dir ? 0 : static_cast<int64_t>((static_cast<uint64_t>(bi.nFileSizeHigh) << 32) | bi.nFileSizeLow);
    st->st_blksize = 4096;
    st->st_blocks = (st->st_size + 511) / 512;
    return 0;
}

int64_t w_stat(const char* path, DarwinStat* st) {
    if (!st) return fail_errno(detail::kEFAULT);
    fs::path host;
    if (int e = resolve_path(path, host)) return fail_errno(e);
    HANDLE h = CreateFileW(extended_path(host).c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return fail_errno(errno_from_win32(GetLastError()));
    const int e = fill_stat_from_handle(h, st);
    CloseHandle(h);
    return e ? fail_errno(e) : 0;
}

int64_t w_fstat(int fd, DarwinStat* st) {
    GuestFd* g = fd_entry(fd);
    if (!g) return fail_errno(detail::kEBADF);
    if (!st) return fail_errno(detail::kEFAULT);
    if (g->kind == GuestFd::Kind::Console) {
        std::memset(st, 0, sizeof *st);
        st->st_mode = static_cast<uint16_t>(kS_IFCHR | 0620);
        st->st_nlink = 1;
        st->st_blksize = 1024;
        return 0;
    }
    const int e = fill_stat_from_handle(static_cast<HANDLE>(g->handle), st);
    return e ? fail_errno(e) : 0;
}

int64_t w_access(const char* path, int mode) {
    if (mode & ~(kR_OK | kW_OK | kX_OK)) return fail_errno(detail::kEINVAL);
    fs::path host;
    if (int e = resolve_path(path, host)) return fail_errno(e);
    const DWORD attr = GetFileAttributesW(extended_path(host).c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return fail_errno(errno_from_win32(GetLastError()));
    const bool dir = (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if ((mode & kW_OK) && (attr & FILE_ATTRIBUTE_READONLY) && !dir) return fail_errno(detail::kEACCES);
    if ((mode & kX_OK) && !dir) return fail_errno(detail::kEACCES);       // Windows files carry no execute bit
    return 0;
}

char* w_getcwd(char* buf, uint64_t size) {
    static const char cwd[] = "/";                                          // CIDER has no chdir: the guest lives at its root
    if (!buf) {
        const uint64_t n = size ? size : sizeof cwd;
        if (n < sizeof cwd) { fail_errno(detail::kERANGE); return nullptr; }
        buf = static_cast<char*>(detail::guest_malloc(n));
        if (!buf) { fail_errno(detail::kENOMEM); return nullptr; }
    } else if (size < sizeof cwd) {
        fail_errno(detail::kERANGE);
        return nullptr;
    }
    std::memcpy(buf, cwd, sizeof cwd);
    return buf;
}

// ---- time -------------------------------------------------------------------------------------------------------------------
struct DarwinTimeval { int64_t tv_sec; int32_t tv_usec; int32_t pad; };
struct DarwinTimespec { int64_t tv_sec; int64_t tv_nsec; };
static_assert(sizeof(DarwinTimeval) == 16 && sizeof(DarwinTimespec) == 16, "Darwin time structures");

uint64_t unix_time_100ns() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    const uint64_t t = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return t - 116444736000000000ull;
}

int64_t w_gettimeofday(DarwinTimeval* tv, int32_t* tz) {
    if (tv) {
        const uint64_t u = unix_time_100ns();
        tv->tv_sec = static_cast<int64_t>(u / 10000000ull);
        tv->tv_usec = static_cast<int32_t>((u % 10000000ull) / 10);
        tv->pad = 0;
    }
    if (tz) { tz[0] = 0; tz[1] = 0; }    // struct timezone { int tz_minuteswest; int tz_dsttime; }
    return 0;
}

uint64_t qpc_ns() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const uint64_t whole = static_cast<uint64_t>(now.QuadPart) / static_cast<uint64_t>(freq.QuadPart);
    const uint64_t rest = static_cast<uint64_t>(now.QuadPart) % static_cast<uint64_t>(freq.QuadPart);
    return whole * 1000000000ull + rest * 1000000000ull / static_cast<uint64_t>(freq.QuadPart);
}

uint64_t filetime_ns(const FILETIME& ft) { return ((static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime) * 100; }

// Darwin clock ids (not Linux': CLOCK_MONOTONIC is 6 here).
int64_t w_clock_gettime(int clk, DarwinTimespec* ts) {
    if (!ts) return fail_errno(detail::kEFAULT);
    uint64_t ns;
    switch (clk) {
        case kCLOCK_REALTIME: ns = unix_time_100ns() * 100; break;
        case kCLOCK_MONOTONIC: case kCLOCK_MONOTONIC_RAW: case kCLOCK_UPTIME_RAW: ns = qpc_ns(); break;
        case kCLOCK_PROCESS_CPUTIME_ID: {
            FILETIME c, e, k, u;
            if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return fail_errno(detail::kEINVAL);
            ns = filetime_ns(k) + filetime_ns(u);
            break;
        }
        case kCLOCK_THREAD_CPUTIME_ID: {
            FILETIME c, e, k, u;
            if (!GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u)) return fail_errno(detail::kEINVAL);
            ns = filetime_ns(k) + filetime_ns(u);
            break;
        }
        default: return fail_errno(detail::kEINVAL);
    }
    ts->tv_sec = static_cast<int64_t>(ns / 1000000000ull);
    ts->tv_nsec = static_cast<int64_t>(ns % 1000000000ull);
    return 0;
}

int64_t w_time(int64_t* out) {
    const int64_t t = static_cast<int64_t>(unix_time_100ns() / 10000000ull);
    if (out) *out = t;
    return t;
}

int64_t w_nanosleep(const DarwinTimespec* req, DarwinTimespec* rem) {
    if (!req) return fail_errno(detail::kEFAULT);
    if (req->tv_sec < 0 || req->tv_nsec < 0 || req->tv_nsec >= 1000000000) return fail_errno(detail::kEINVAL);
    const uint64_t ms = static_cast<uint64_t>(req->tv_sec) * 1000 + (static_cast<uint64_t>(req->tv_nsec) + 999999) / 1000000;
    // Sleep in slices so a very long request cannot exceed DWORD range.
    uint64_t left = ms;
    while (left) {
        const DWORD slice = static_cast<DWORD>(std::min<uint64_t>(left, 1u << 20));
        Sleep(slice);
        left -= slice;
    }
    if (rem) { rem->tv_sec = 0; rem->tv_nsec = 0; }
    return 0;
}
int w_usleep(uint32_t us) { Sleep((us + 999) / 1000); return 0; }
uint32_t w_sleep(uint32_t s) { Sleep(s * 1000u); return 0; }

// ---- process identity ---------------------------------------------------------------------------------------------------------
int w_getppid() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 1;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof pe;
    DWORD parent = 0;
    const DWORD me = GetCurrentProcessId();
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        if (pe.th32ProcessID == me) { parent = pe.th32ParentProcessID; break; }
    CloseHandle(snap);
    return parent ? static_cast<int>(parent) : 1;
}
void w_exit_now(int code) {
    if (RuntimeContext* c = context()) c->skip_atexit = true;   // _exit: no atexit handlers
    abi::request_guest_exit(static_cast<uint32_t>(code));
}   // _exit: the guest has no atexit handlers anyway

int* w_errno_location() {
    static int fallback = 0;
    RuntimeContext* c = context();
    return c ? &c->errno_value : &fallback;
}

template <class F> void* addr(F* f) { return reinterpret_cast<void*>(f); }

#else   // !_WIN32: the runtime is Windows-only (see CIDER_HAS_RUNTIME)
#endif

}  // namespace

RuntimeContext::~RuntimeContext() {
#ifdef _WIN32
    for (auto& f : fds)
        if (f.kind == GuestFd::Kind::File && f.handle) CloseHandle(static_cast<HANDLE>(f.handle));
    if (heap) HeapDestroy(heap);
#endif
}

namespace detail {
int fd_open(const char* guest_path, int flags) { return open_impl(guest_path, flags); }
int fd_close(int fd) { return w_close(fd); }
int64_t fd_read(int fd, void* buf, uint64_t n) { return w_read(fd, buf, n); }
int64_t fd_write(int fd, const void* buf, uint64_t n) { return w_write_fd(fd, buf, n); }
int64_t fd_seek(int fd, int64_t off, int whence) { return w_lseek(fd, off, whence); }

void add_posix_symbols(std::vector<HostSymbol>& t) {
#ifdef _WIN32
    t.push_back({"_open", addr(w_open), true});
    t.push_back({"_close", addr(w_close)});
    t.push_back({"_write", addr(w_write_fd)});
    t.push_back({"_read", addr(w_read)});
    t.push_back({"_lseek", addr(w_lseek)});
    t.push_back({"_stat", addr(w_stat)});
    t.push_back({"_stat$INODE64", addr(w_stat)});
    t.push_back({"_fstat", addr(w_fstat)});
    t.push_back({"_fstat$INODE64", addr(w_fstat)});
    t.push_back({"_access", addr(w_access)});
    t.push_back({"_getcwd", addr(w_getcwd)});
    t.push_back({"_gettimeofday", addr(w_gettimeofday)});
    t.push_back({"_clock_gettime", addr(w_clock_gettime)});
    t.push_back({"_time", addr(w_time)});
    t.push_back({"_nanosleep", addr(w_nanosleep)});
    t.push_back({"_usleep", addr(w_usleep)});
    t.push_back({"_sleep", addr(w_sleep)});
    t.push_back({"_getppid", addr(w_getppid)});
    t.push_back({"__exit", addr(w_exit_now)});
    t.push_back({"___error", addr(w_errno_location)});
#else
    (void)t;
#endif
}
}  // namespace detail

}  // namespace cider::libsystem

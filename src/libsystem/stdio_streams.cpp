// FILE streams for guest code (ORVYN 0.4.1).
//
// Darwin's FILE is a public struct (_p, _r, _w, _flags, _file, ...) that some macros read directly; ORVYN does NOT emulate that
// layout. A guest FILE* is an OPAQUE pointer to a 64-byte, read-only slot in a pool owned by the libSystem provider (magic + slot
// index). The state lives in the host, in RuntimeContext::streams, on top of the guest descriptor table: so path confinement
// (--root), the descriptor limit and cleanup at exit are shared with open()/close(). A guest pointer is only ever compared against
// the pool range and looked up by index, never dereferenced: a forged, misaligned, stale (fclose'd) or NULL FILE* fails with EBADF.
//
// Files are opened in BINARY mode (no CRLF translation, no Ctrl-Z): bytes written are the bytes on disk. There is no write buffering
// (every fwrite reaches the OS), so fflush has nothing to do; reads are buffered (4 KiB) and the buffer is discarded by fseek/rewind
// and rolled back by writes, so ftell and the file position stay consistent.
#include <algorithm>
#include <cstring>

#include "orvyn/libsystem.hpp"
#include "internal.hpp"

namespace orvyn::libsystem {
namespace {

using detail::fail_errno;

constexpr int kEOF = -1;
constexpr int kO_RDONLY = 0, kO_WRONLY = 1, kO_RDWR = 2, kO_APPEND = 8, kO_CREAT = 0x200, kO_TRUNC = 0x400, kO_EXCL = 0x800;
constexpr int kSEEK_SET = 0, kSEEK_CUR = 1, kSEEK_END = 2;
constexpr size_t kReadChunk = 4096;

Stream* stream_at(int idx) {
    RuntimeContext* c = context();
    if (!c || idx < 0 || static_cast<size_t>(idx) >= c->streams.size() || !c->streams[static_cast<size_t>(idx)].in_use) return nullptr;
    return &c->streams[static_cast<size_t>(idx)];
}

// Validates a guest FILE*; sets errno EBADF and returns nullptr for anything that is not a live stream.
Stream* stream_of_file(uint64_t f) {
    const int idx = detail::stream_index(f);
    Stream* s = stream_at(idx);
    if (!s) detail::fail_errno(detail::kEBADF);
    return s;
}

size_t buffered(const Stream& s) { return s.rbuf.size() - s.rpos + (s.ungot >= 0 ? 1 : 0); }

// Puts the OS file position where the program believes it is (read-ahead given back).
void drop_read_ahead(Stream& s) {
    const size_t pending = s.rbuf.size() - s.rpos;
    if (pending && s.fd >= 3) detail::fd_seek(s.fd, -static_cast<int64_t>(pending), kSEEK_CUR);
    s.rbuf.clear();
    s.rpos = 0;
    s.ungot = -1;
}

// Refills the read buffer. Returns false at end of file or on error (flags set).
bool fill(Stream& s) {
    s.rbuf.assign(kReadChunk, 0);
    s.rpos = 0;
    const int64_t n = detail::fd_read(s.fd, s.rbuf.data(), kReadChunk);
    if (n < 0) { s.rbuf.clear(); s.error = true; return false; }
    s.rbuf.resize(static_cast<size_t>(n));
    if (n == 0) { s.eof = true; return false; }
    return true;
}

// ---- open / close -----------------------------------------------------------------------------------------------------
uint64_t w_fopen(const char* path, const char* mode) {
    RuntimeContext* c = context();
    if (!c || !path || !mode) { fail_errno(detail::kEFAULT); return 0; }
    // mode: (r|w|a)[+][b][x] in any order after the first character; anything else is EINVAL
    int flags = 0;
    bool plus = false, excl = false;
    switch (mode[0]) {
        case 'r': flags = kO_RDONLY; break;
        case 'w': flags = kO_WRONLY | kO_CREAT | kO_TRUNC; break;
        case 'a': flags = kO_WRONLY | kO_CREAT | kO_APPEND; break;
        default: fail_errno(detail::kEINVAL); return 0;
    }
    for (const char* m = mode + 1; *m; ++m) {
        if (*m == '+') plus = true;
        else if (*m == 'b' || *m == 'e') continue;                  // 'e' (close-on-exec, DARWIN_EXTSN) has no meaning without exec
        else if (*m == 'x' && mode[0] == 'w') excl = true;
        else { fail_errno(detail::kEINVAL); return 0; }
    }
    if (plus) flags = (flags & ~3) | kO_RDWR;
    if (excl) flags |= kO_EXCL;
    // find a free slot (the first three belong to stdin/stdout/stderr)
    size_t slot = c->streams.size();
    for (size_t i = 3; i < c->streams.size(); ++i)
        if (!c->streams[i].in_use) { slot = i; break; }
    if (slot == c->streams.size()) { fail_errno(detail::kEMFILE); return 0; }
    const int fd = detail::fd_open(path, flags);
    if (fd < 0) return 0;                                         // errno set by the open path
    Stream& s = c->streams[slot];
    s = Stream{};
    s.in_use = true;
    s.fd = fd;
    s.readable = (flags & 3) != kO_WRONLY;
    s.writable = (flags & 3) != kO_RDONLY;
    s.append = (flags & kO_APPEND) != 0;
    return c->stream_base + slot * kFileObjectSize;
}

// FILE *fdopen(int fd, const char *mode): wraps an open guest descriptor; fclose() will close the descriptor. The mode must not ask for more than the descriptor allows.
uint64_t w_fdopen(int fd, const char* mode) {
    RuntimeContext* c = context();
    if (!c || !mode) { fail_errno(detail::kEFAULT); return 0; }
    if (fd < 0 || static_cast<size_t>(fd) >= c->fds.size() || c->fds[static_cast<size_t>(fd)].kind == GuestFd::Kind::Closed) { fail_errno(detail::kEBADF); return 0; }
    bool want_read = false, want_write = false, append = false;
    switch (mode[0]) {
        case 'r': want_read = true; break;
        case 'w': want_write = true; break;
        case 'a': want_write = true; append = true; break;
        default: fail_errno(detail::kEINVAL); return 0;
    }
    for (const char* m = mode + 1; *m; ++m) {
        if (*m == '+') want_read = want_write = true;
        else if (*m != 'b' && *m != 'e' && *m != 'x') { fail_errno(detail::kEINVAL); return 0; }       // 'e' (close-on-exec) is the DARWIN_EXTSN extension; irrelevant without exec
    }
    const GuestFd& g = c->fds[static_cast<size_t>(fd)];
    if ((want_read && !g.readable) || (want_write && !g.writable)) { fail_errno(detail::kEINVAL); return 0; }
    size_t slot = c->streams.size();
    for (size_t i = 3; i < c->streams.size(); ++i)
        if (!c->streams[i].in_use) { slot = i; break; }
    if (slot == c->streams.size()) { fail_errno(detail::kEMFILE); return 0; }
    Stream& s = c->streams[slot];
    s = Stream{};
    s.in_use = true;
    s.fd = fd;
    s.readable = want_read;
    s.writable = want_write;
    s.append = append;
    return c->stream_base + slot * kFileObjectSize;
}

int w_fclose(uint64_t f) {
    Stream* s = stream_of_file(f);
    if (!s) return kEOF;
    drop_read_ahead(*s);
    const int rc = detail::fd_close(s->fd);
    *s = Stream{};
    return rc < 0 ? kEOF : 0;
}

int w_fileno(uint64_t f) {
    Stream* s = stream_of_file(f);
    return s ? s->fd : -1;
}

int w_setvbuf(uint64_t f, void*, int, uint64_t) { return stream_of_file(f) ? 0 : -1; }   // streams are unbuffered for writing: accepted, no-op

// ---- reading -------------------------------------------------------------------------------------------------------------
uint64_t w_fread(uint8_t* buf, uint64_t size, uint64_t count, uint64_t f) {
    Stream* s = stream_of_file(f);
    if (!s) return 0;
    if (size == 0 || count == 0) return 0;
    if (count > UINT64_MAX / size) { fail_errno(detail::kEINVAL); s->error = true; return 0; }
    if (!s->readable) { fail_errno(detail::kEBADF); s->error = true; return 0; }
    if (!buf) { fail_errno(detail::kEFAULT); s->error = true; return 0; }
    const uint64_t want = size * count;
    uint64_t got = 0;
    if (s->ungot >= 0) { buf[got++] = static_cast<uint8_t>(s->ungot); s->ungot = -1; }
    while (got < want) {
        if (s->rpos < s->rbuf.size()) {
            const size_t n = static_cast<size_t>(std::min<uint64_t>(want - got, s->rbuf.size() - s->rpos));
            std::memcpy(buf + got, s->rbuf.data() + s->rpos, n);
            s->rpos += n;
            got += n;
        } else if (want - got >= kReadChunk) {                    // big read: straight into the guest buffer
            const int64_t n = detail::fd_read(s->fd, buf + got, std::min<uint64_t>(want - got, 1u << 30));
            if (n < 0) { s->error = true; break; }
            if (n == 0) { s->eof = true; break; }
            got += static_cast<uint64_t>(n);
        } else if (!fill(*s)) {
            break;
        }
    }
    return got / size;
}

int w_fgetc(uint64_t f) {
    Stream* s = stream_of_file(f);
    if (!s) return kEOF;
    if (!s->readable) { fail_errno(detail::kEBADF); s->error = true; return kEOF; }
    if (s->ungot >= 0) { const int c = s->ungot; s->ungot = -1; return c; }
    if (s->rpos >= s->rbuf.size() && !fill(*s)) return kEOF;
    return s->rbuf[s->rpos++];
}

int w_ungetc(int c, uint64_t f) {
    Stream* s = stream_of_file(f);
    if (!s || c == kEOF || !s->readable || s->ungot >= 0) return kEOF;   // one character of push-back, like the C minimum
    s->ungot = c & 0xff;
    s->eof = false;
    return c & 0xff;
}

char* w_fgets(char* buf, int n, uint64_t f) {
    Stream* s = stream_of_file(f);
    if (!s) return nullptr;
    if (n <= 0 || !buf) { fail_errno(detail::kEINVAL); return nullptr; }
    if (!s->readable) { fail_errno(detail::kEBADF); s->error = true; return nullptr; }
    int i = 0;
    while (i < n - 1) {
        const int c = w_fgetc(f);
        if (c == kEOF) break;
        buf[i++] = static_cast<char>(c);
        if (c == '\n') break;
    }
    if (i == 0) return nullptr;                                  // end of file (or error) before any character
    buf[i] = 0;
    return buf;
}

// ---- state ----------------------------------------------------------------------------------------------------------------
int w_feof(uint64_t f) { Stream* s = stream_of_file(f); return s && s->eof ? 1 : 0; }
int w_ferror(uint64_t f) { Stream* s = stream_of_file(f); return s && s->error ? 1 : 0; }
void w_clearerr(uint64_t f) { if (Stream* s = stream_of_file(f)) { s->eof = false; s->error = false; } }

int64_t w_ftell(uint64_t f) {
    Stream* s = stream_of_file(f);
    if (!s) return -1;
    const int64_t pos = detail::fd_seek(s->fd, 0, kSEEK_CUR);
    if (pos < 0) return -1;
    return pos - static_cast<int64_t>(buffered(*s));
}

int w_fseek(uint64_t f, int64_t off, int whence) {
    Stream* s = stream_of_file(f);
    if (!s) return -1;
    if (whence != kSEEK_SET && whence != kSEEK_CUR && whence != kSEEK_END) { fail_errno(detail::kEINVAL); return -1; }
    int64_t target = off;
    int origin = whence;
    if (whence == kSEEK_CUR) {                                    // relative to the LOGICAL position (read-ahead excluded)
        const int64_t here = w_ftell(f);
        if (here < 0) return -1;
        target = here + off;
        origin = kSEEK_SET;
    }
    s->rbuf.clear();
    s->rpos = 0;
    s->ungot = -1;
    if (detail::fd_seek(s->fd, target, origin) < 0) return -1;
    s->eof = false;
    return 0;
}

int w_rewind_impl(uint64_t f) {
    Stream* s = stream_of_file(f);
    if (!s) return -1;
    const int rc = w_fseek(f, 0, kSEEK_SET);
    s->error = false;
    return rc;
}
void w_rewind(uint64_t f) { w_rewind_impl(f); }

template <class F> void* addr(F* f) { return reinterpret_cast<void*>(f); }

}  // namespace

namespace detail {

// index of the stream slot a guest FILE* designates, or -1. Pure address arithmetic: the pointer is never dereferenced.
int stream_index(uint64_t f) {
    RuntimeContext* c = context();
    if (!c || c->stream_base == 0 || f < c->stream_base) return -1;
    const uint64_t off = f - c->stream_base;
    if (off % kFileObjectSize != 0 || off / kFileObjectSize >= c->streams.size()) return -1;
    return static_cast<int>(off / kFileObjectSize);
}

int64_t stream_write(int idx, const void* data, size_t n) {
    Stream* s = stream_at(idx);
    if (!s) return fail_errno(kEBADF);
    if (!s->writable) { s->error = true; return fail_errno(kEBADF); }
    if (s->fd == 1 || s->fd == 2) {
        // stdout/stderr go through the sink even when the program closed and reopened descriptors: only the original console fds
        console_write(s->fd, static_cast<const char*>(data), n);
        return static_cast<int64_t>(n);
    }
    drop_read_ahead(*s);                                          // writing after reading: put the OS position back first
    const int64_t w = fd_write(s->fd, data, n);
    if (w < 0 || static_cast<size_t>(w) != n) { s->error = true; return w < 0 ? w : fail_errno(kEIO); }
    return w;
}

void add_stream_symbols(std::vector<HostSymbol>& t) {
    t.push_back({"_fopen", addr(w_fopen)});
    t.push_back({"_fclose", addr(w_fclose)});
    t.push_back({"_fdopen", addr(w_fdopen)});
    t.push_back({"_fdopen$DARWIN_EXTSN", addr(w_fdopen)});          // same function: the extension only adds mode letters (e, x)
    t.push_back({"_fopen$DARWIN_EXTSN", addr(w_fopen)});            // fopen accepts the extension letters (x, e)
    t.push_back({"_fread", addr(w_fread)});
    t.push_back({"_fgetc", addr(w_fgetc)});
    t.push_back({"_getc", addr(w_fgetc)});
    t.push_back({"_ungetc", addr(w_ungetc)});
    t.push_back({"_fgets", addr(w_fgets)});
    t.push_back({"_feof", addr(w_feof)});
    t.push_back({"_ferror", addr(w_ferror)});
    t.push_back({"_clearerr", addr(w_clearerr)});
    t.push_back({"_ftell", addr(w_ftell)});
    t.push_back({"_ftello", addr(w_ftell)});
    t.push_back({"_fseek", addr(w_fseek)});
    t.push_back({"_fseeko", addr(w_fseek)});
    t.push_back({"_rewind", addr(w_rewind)});
    t.push_back({"_fileno", addr(w_fileno)});
    t.push_back({"_setvbuf", addr(w_setvbuf)});
}

}  // namespace detail
}  // namespace orvyn::libsystem

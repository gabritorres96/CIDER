#pragma once
// CIDER's libSystem: the subset of Darwin's libc that guest code can call, implemented on the host with the Windows
// ABI and exposed to guest code through System V thunks (see abi.hpp). It is an ADAPTATION layer, not Apple's libc:
// every function re-implements the Darwin contract (structure layouts, constants, errno values, flag bits) on top of
// Win32, and anything that is not listed in docs/DARWIN-RUNTIME.md is NOT implemented. A binary that imports an
// unimplemented symbol is refused at load time with "unresolved symbol".
//
// 0.2 subset:  puts putchar write exit getpid getenv strlen strcmp strncmp memcpy memmove memset memcmp abs labs
//              malloc calloc realloc free sqrt fabs floor ceil pow  + a trapping dyld_stub_binder
// 0.3 additions: see docs/DARWIN-RUNTIME.md (printf family, FILE-less stdio, file descriptors, time, process ids,
//              more string and math functions, aggregate-returning div/ldiv, errno through ___error).
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "cider/abi.hpp"
#include "cider/fixups.hpp"
#include "cider/result.hpp"

namespace cider::libsystem {

inline constexpr const char* kInstallName = "/usr/lib/libSystem.B.dylib";
inline constexpr const char* kObjcInstallName = "/usr/lib/libobjc.A.dylib";
inline constexpr const char* kFoundationInstallName = "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation";
inline constexpr const char* kCoreFoundationInstallName = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";
// Which provided library a symbol belongs to. CoreFoundation and Foundation are one group: CIDER's Foundation subset serves both.
enum class Lib : uint8_t { System = 0, Objc = 1, Foundation = 2 };

// A file descriptor of the guest. These are NOT Windows handles and NOT the host's CRT descriptors: the guest sees a
// small integer table starting at 0 (stdin), 1 (stdout), 2 (stderr), like a Darwin process.
struct GuestFd {
    enum class Kind { Closed, Console, File } kind = Kind::Closed;
    int console = -1;            // Console: 0 stdin, 1 stdout, 2 stderr
    void* handle = nullptr;      // File: Win32 handle (owned)
    bool readable = false, writable = false, append = false;
};

class GuestThreads;
}
namespace cider::objc {
class Runtime;
}
namespace cider::libsystem {

// Thread-local storage of one image (Mach-O TLV protocol, see src/libsystem/tls.cpp).
struct TlsImage {
    uint64_t lo = 0, hi = 0;             // the image's mapped range: its TLV descriptors live inside
    uint64_t tmpl = 0;                   // address of the template (start of the first __thread_data/__thread_bss section)
    uint64_t tmpl_size = 0;              // template size: __thread_data + padding + __thread_bss
    uint64_t init_size = 0;              // bytes copied from the template (the __thread_data part); the rest is zero
    uint64_t align = 16;
    uint8_t* block = nullptr;            // this (single) guest thread's storage, created on first use in the guest heap
};

// State of one guest FILE stream (the guest only ever holds an opaque pointer to a pool slot, see stdio_streams.cpp).
struct Stream {
    bool in_use = false;
    int fd = -1;                      // guest descriptor underneath
    bool readable = false, writable = false, append = false;
    bool eof = false, error = false;
    std::vector<uint8_t> rbuf;        // read-ahead
    size_t rpos = 0;
    int ungot = -1;                   // ungetc character
};
inline constexpr uint64_t kFileObjectSize = 64;   // stride of the FILE objects handed to the guest
inline constexpr size_t kMaxStreams = 64;         // slots 0-2 are stdin/stdout/stderr

// Per-guest-process state seen by the host functions (single guest at a time).
struct RuntimeContext {
    RuntimeContext();
    ~RuntimeContext();                      // destroys the guest heap and closes guest files: nothing can leak into CIDER
    RuntimeContext(const RuntimeContext&) = delete;
    RuntimeContext& operator=(const RuntimeContext&) = delete;
    void* heap = nullptr;          // private Windows heap behind malloc/calloc/realloc/free (created lazily)
    std::vector<std::string> env;  // "NAME=value" entries returned by getenv()
    // Receives everything the guest writes to fd 1 / fd 2. Default (empty): the real stdout/stderr.
    std::function<void(int fd, const char* data, size_t size)> sink;
    std::set<void*> allocations;   // live malloc blocks (free() of anything else is refused)
    std::string fault;             // set when the guest hits a runtime trap (e.g. lazy binding, unsupported printf)
    uint64_t bytes_out = 0, bytes_err = 0, calls = 0;

    // ---- POSIX layer (0.3) ------------------------------------------------------------------------------
    int errno_value = 0;                    // storage behind ___error()
    std::filesystem::path root;             // host directory the guest's "/" maps to; empty: file access is refused (EACCES)
    std::vector<GuestFd> fds;               // index = guest descriptor
    uint64_t file_stdin = 0, file_stdout = 0, file_stderr = 0;   // guest addresses of the three FILE objects
    std::unique_ptr<objc::Runtime> objc;     // Objective-C runtime (created by the runner before linking)
    std::unique_ptr<GuestThreads> threads;  // guest threads (pthreads) and the cooperative guest lock; created by the runner
    std::vector<Stream> streams;            // FILE slots (kMaxStreams); the guest FILE* of slot i is stream_base + i * kFileObjectSize
    uint64_t stream_base = 0;
    std::vector<TlsImage> tls_images;       // images with thread-local storage (filled by the dynamic linker)
    // ---- callbacks (0.4) -------------------------------------------------------------------------------------
    std::vector<std::pair<uint64_t, uint64_t>> code_ranges;   // [lo, hi) executable guest code a callback may target (+ libSystem thunks)
    std::vector<uint64_t> atexit_handlers;  // guest functions registered with atexit(), run in reverse order
    bool skip_atexit = false;               // _exit(): handlers do not run
};

void set_context(RuntimeContext* ctx);
// True when `fn` lies in a registered executable guest range: the only addresses the host may call back into.
bool is_valid_callback(uint64_t fn);
RuntimeContext* context();

// Writes to the guest's stdout (fd 1) or stderr (fd 2): through RuntimeContext::sink, else the process handles.
void console_write(int fd, const char* data, size_t n);

struct HostSymbol {
    const char* name;  // as it appears in the Mach-O (leading underscore)
    void* fn;          // Windows x64 function (positional thunk) or abi::RawHostFn (raw thunk)
    bool raw = false;  // true: fn receives abi::GuestRegs* (variadic, mixed, stack arguments, aggregate returns)
    bool preserve_all = false;   // true: Darwin TLV convention, every register except rax survives (tlv_get_addr)
    bool direct = false;         // true: `fn` is guest-callable machine code (objc_msgSend...): its address IS the symbol, no thunk
    Lib lib = Lib::System;       // library that exports the symbol
};
const std::vector<HostSymbol>& implemented_symbols();
bool is_implemented(std::string_view mach_o_symbol);

// Data symbols exported by libSystem (variables the guest binds to): the three stdio streams.
struct DataSymbol {
    const char* name;
};
const std::vector<DataSymbol>& implemented_data_symbols();

// Resolves symbols of /usr/lib/libSystem.B.dylib to System V thunks. Owns the thunk pages: keep it alive
// for as long as the guest can run.
class LibSystemProvider final : public dyld::SymbolProvider {
public:
    static Result<std::unique_ptr<LibSystemProvider>> create();
    std::optional<dyld::ResolvedSymbol> lookup(std::string_view library, std::string_view symbol) override;
    bool provides_library(std::string_view install_name) const override;
    // Symbols whose addresses only exist at run time (Objective-C classes, empty caches...): looked up after the static tables.
    using DynamicLookup = std::function<std::optional<uint64_t>(std::string_view symbol)>;
    void set_dynamic_data(DynamicLookup f) { dynamic_ = std::move(f); }
    // Addresses of the FILE objects behind stdin/stdout/stderr (to be stored in the RuntimeContext).
    void install_stdio(RuntimeContext& ctx) const;
    // [lo, hi) of the thunk table: guest code may legitimately pass libSystem functions (strcmp...) as callbacks.
    std::pair<uint64_t, uint64_t> thunk_range() const { return {table_.address(0), table_.address(0) + table_.size() * abi::ThunkTable::kStride}; }

private:
    abi::ThunkTable table_;
    memory::Region data_;              // stdio variables and FILE objects (read-only after initialisation)
    std::map<std::string, size_t, std::less<>> index_;
    std::map<std::string, uint64_t, std::less<>> data_index_;
    std::map<std::string, uint64_t, std::less<>> direct_index_;    // name -> address of machine code
    std::map<std::string, Lib, std::less<>> lib_of_;
    DynamicLookup dynamic_;
    uint64_t files_[3] = {};
};

}  // namespace cider::libsystem

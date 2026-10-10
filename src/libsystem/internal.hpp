#pragma once
// Private to src/libsystem: how the implementation files contribute to the symbol table.
#include <cstdint>
#include <vector>

#include "cider/guest_threads.hpp"
#include "cider/libsystem.hpp"

namespace cider::libsystem::detail {

void add_stdio_symbols(std::vector<HostSymbol>& out);   // stdio.cpp : printf family, fwrite, fputs, ...
void add_posix_symbols(std::vector<HostSymbol>& out);   // posix.cpp : files, time, process ids, errno
void add_misc_symbols(std::vector<HostSymbol>& out);    // misc.cpp  : more string and math functions, div/ldiv
void add_stream_symbols(std::vector<HostSymbol>& out);   // stdio_streams.cpp: fopen, fclose, fread, fseek, ...
void add_pthread_symbols(std::vector<HostSymbol>& out);  // pthread_bridge.cpp
void add_callback_symbols(std::vector<HostSymbol>& out); // callbacks.cpp: qsort, bsearch, atexit
void add_tls_symbols(std::vector<HostSymbol>& out);     // tls.cpp   : __tlv_bootstrap / tlv_get_addr

// Darwin errno values (<sys/errno.h>). They differ from Windows' and from Linux' (EAGAIN is 35 here, 11 on Linux).
enum DarwinErrno : int {
    kEPERM = 1, kENOENT = 2, kEINTR = 4, kEIO = 5, kEBADF = 9, kENOMEM = 12, kEACCES = 13, kEFAULT = 14, kEEXIST = 17,
    kENOTDIR = 20, kEISDIR = 21, kEINVAL = 22, kEMFILE = 24, kEFBIG = 27, kENOSPC = 28, kESPIPE = 29, kEROFS = 30,
    kEPIPE = 32, kERANGE = 34, kEAGAIN = 35, kENAMETOOLONG = 63, kENOTEMPTY = 66, kENOSYS = 78
};

// Records `e` in the guest's errno and returns -1 (the POSIX failure value).
inline int& errno_ref() {                       // errno is per guest thread on Darwin
    static int detached = 0;
    if (ThreadState* t = GuestThreads::current()) return t->errno_value;
    RuntimeContext* c = context();
    return c ? c->errno_value : detached;
}
inline int64_t fail_errno(int e) {
    errno_ref() = e;
    return -1;
}

// Guest descriptor primitives (posix.cpp) used by the FILE layer: -1 and errno on failure.
int fd_open(const char* guest_path, int darwin_flags);
int fd_close(int fd);
int64_t fd_read(int fd, void* buf, uint64_t n);
int64_t fd_write(int fd, const void* buf, uint64_t n);
int64_t fd_seek(int fd, int64_t off, int whence);
// FILE layer (stdio_streams.cpp)
int stream_index(uint64_t guest_file);                       // slot of a guest FILE*, -1 when it is not a pool address
int64_t stream_write(int slot, const void* data, size_t n);  // bytes written, -1 + errno on failure

// malloc from the guest's private heap (used by getcwd(NULL, 0), strdup).
void* guest_malloc(uint64_t n);

// A runtime trap: the guest asked for something CIDER refuses to emulate. Marks the run as faulted and ends it.
void trap(const std::string& what);

}  // namespace cider::libsystem::detail

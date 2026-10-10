#pragma once
// Private to src/libsystem: how the implementation files contribute to the symbol table.
#include <cstdint>
#include <vector>

#include "cider/libsystem.hpp"

namespace cider::libsystem::detail {

void add_stdio_symbols(std::vector<HostSymbol>& out);   // stdio.cpp : printf family, fwrite, fputs, ...
void add_posix_symbols(std::vector<HostSymbol>& out);   // posix.cpp : files, time, process ids, errno
void add_misc_symbols(std::vector<HostSymbol>& out);    // misc.cpp  : more string and math functions, div/ldiv
void add_tls_symbols(std::vector<HostSymbol>& out);     // tls.cpp   : __tlv_bootstrap / tlv_get_addr

// Darwin errno values (<sys/errno.h>). They differ from Windows' and from Linux' (EAGAIN is 35 here, 11 on Linux).
enum DarwinErrno : int {
    kEPERM = 1, kENOENT = 2, kEINTR = 4, kEIO = 5, kEBADF = 9, kENOMEM = 12, kEACCES = 13, kEFAULT = 14, kEEXIST = 17,
    kENOTDIR = 20, kEISDIR = 21, kEINVAL = 22, kEMFILE = 24, kEFBIG = 27, kENOSPC = 28, kESPIPE = 29, kEROFS = 30,
    kEPIPE = 32, kERANGE = 34, kEAGAIN = 35, kENAMETOOLONG = 63, kENOTEMPTY = 66, kENOSYS = 78
};

// Records `e` in the guest's errno and returns -1 (the POSIX failure value).
inline int64_t fail_errno(int e) {
    if (RuntimeContext* c = context()) c->errno_value = e;
    return -1;
}

// malloc from the guest's private heap (used by getcwd(NULL, 0), strdup).
void* guest_malloc(uint64_t n);

// A runtime trap: the guest asked for something CIDER refuses to emulate. Marks the run as faulted and ends it.
void trap(const std::string& what);

}  // namespace cider::libsystem::detail

#include "cider/macho.hpp"
#include "cider/util.hpp"

namespace cider::macho {

std::string cpu_name(uint32_t cputype, uint32_t cpusubtype) {
    uint32_t sub = cpusubtype & 0x00ffffffu;  // top byte holds capability bits (e.g. LIB64)
    switch (cputype) {
        case kCpuX86: return "i386";
        case kCpuX86_64: return sub == 8 ? "x86_64h" : "x86_64";
        case kCpuArm: return "arm";
        case kCpuArm64: return sub == 2 ? "arm64e" : "arm64";
        case 0x0200000c: return "arm64_32";
        case kCpuPpc: return "ppc";
        case kCpuPpc64: return "ppc64";
        default: return "unknown(" + hex(cputype) + ")";
    }
}

const char* filetype_name(uint32_t t) noexcept {
    switch (t) {
        case kFileObject: return "object";
        case kFileExecute: return "executable";
        case kFileFvmLib: return "fixed VM library";
        case kFileCore: return "core";
        case kFilePreload: return "preloaded executable";
        case kFileDylib: return "dynamic library";
        case kFileDylinker: return "dynamic linker";
        case kFileBundle: return "bundle";
        case kFileDylibStub: return "dylib stub";
        case kFileDsym: return "dSYM companion";
        case kFileKextBundle: return "kext bundle";
        case kFileSet: return "file set";
        default: return "unknown";
    }
}

const char* platform_name(uint32_t p) noexcept {
    switch (p) {
        case 1: return "macOS";
        case 2: return "iOS";
        case 3: return "tvOS";
        case 4: return "watchOS";
        case 5: return "bridgeOS";
        case 6: return "Mac Catalyst";
        case 7: return "iOS Simulator";
        case 8: return "tvOS Simulator";
        case 9: return "watchOS Simulator";
        case 10: return "DriverKit";
        case 11: return "visionOS";
        case 12: return "visionOS Simulator";
        default: return "unknown";
    }
}

const char* dylib_kind_name(DylibKind k) noexcept {
    switch (k) {
        case DylibKind::Load: return "load";
        case DylibKind::Weak: return "weak";
        case DylibKind::Reexport: return "reexport";
        case DylibKind::Lazy: return "lazy";
        case DylibKind::Upward: return "upward";
    }
    return "?";
}

std::vector<std::string> flag_names(uint32_t f) {
    static constexpr struct { uint32_t bit; const char* name; } table[] = {
        {0x1, "NOUNDEFS"},        {0x2, "INCRLINK"},        {0x4, "DYLDLINK"},
        {0x8, "BINDATLOAD"},      {0x10, "PREBOUND"},       {0x20, "SPLIT_SEGS"},
        {0x80, "TWOLEVEL"},       {0x100, "FORCE_FLAT"},    {0x200, "NOMULTIDEFS"},
        {0x400, "NOFIXPREBINDING"}, {0x2000, "SUBSECTIONS_VIA_SYMBOLS"}, {0x8000, "WEAK_DEFINES"},
        {0x10000, "BINDS_TO_WEAK"}, {0x20000, "ALLOW_STACK_EXECUTION"}, {0x40000, "ROOT_SAFE"},
        {0x80000, "SETUID_SAFE"}, {0x100000, "NO_REEXPORTED_DYLIBS"}, {0x200000, "PIE"},
        {0x400000, "DEAD_STRIPPABLE_DYLIB"}, {0x800000, "HAS_TLV_DESCRIPTORS"},
        {0x1000000, "NO_HEAP_EXECUTION"}, {0x2000000, "APP_EXTENSION_SAFE"},
    };
    std::vector<std::string> out;
    for (auto& e : table)
        if (f & e.bit) out.emplace_back(e.name);
    return out;
}

const Segment* Image::find_segment(const std::string& name) const {
    for (auto& s : segments)
        if (s.name == name) return &s;
    return nullptr;
}

const Segment* Image::segment_for_fileoff(uint64_t off) const {
    for (auto& s : segments)
        if (s.filesize && off >= s.fileoff && off - s.fileoff < s.filesize) return &s;
    return nullptr;
}

const Segment* Image::segment_for_vmaddr(uint64_t addr) const {
    for (auto& s : segments)
        if (s.vmsize && addr >= s.vmaddr && addr - s.vmaddr < s.vmsize) return &s;
    return nullptr;
}

const Slice* File::find_slice(uint32_t cputype) const {
    for (auto& s : slices)
        if (s.cputype == cputype) return &s;
    return nullptr;
}

}  // namespace cider::macho

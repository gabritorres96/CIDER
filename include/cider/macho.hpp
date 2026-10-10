#pragma once
// Static Mach-O / FAT parser. Pure analysis: never maps or executes anything.
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "cider/bytes.hpp"
#include "cider/result.hpp"

namespace cider::macho {

// ---- constants -------------------------------------------------------------------------
inline constexpr uint32_t kCpuArchABI64 = 0x01000000;
inline constexpr uint32_t kCpuX86 = 7;
inline constexpr uint32_t kCpuX86_64 = kCpuX86 | kCpuArchABI64;
inline constexpr uint32_t kCpuArm = 12;
inline constexpr uint32_t kCpuArm64 = kCpuArm | kCpuArchABI64;
inline constexpr uint32_t kCpuPpc = 18;
inline constexpr uint32_t kCpuPpc64 = kCpuPpc | kCpuArchABI64;

inline constexpr uint32_t kFileObject = 1, kFileExecute = 2, kFileFvmLib = 3, kFileCore = 4, kFilePreload = 5,
                          kFileDylib = 6, kFileDylinker = 7, kFileBundle = 8, kFileDylibStub = 9,
                          kFileDsym = 10, kFileKextBundle = 11, kFileSet = 12;

inline constexpr uint32_t kFlagTwoLevel = 0x80, kFlagAllowStackExecution = 0x20000, kFlagPie = 0x200000,
                          kFlagNoHeapExecution = 0x1000000;

// ---- data model ------------------------------------------------------------------------
enum class Format { Unknown, MachO64, MachO32, MachO64Swapped, MachO32Swapped, Fat, Fat64 };

struct Section {
    std::string name, segment;
    uint64_t addr = 0, size = 0;
    uint32_t offset = 0, align = 0, reloff = 0, nreloc = 0, flags = 0;
};

struct Segment {
    std::string name;
    uint64_t vmaddr = 0, vmsize = 0, fileoff = 0, filesize = 0;
    int32_t maxprot = 0, initprot = 0;
    uint32_t flags = 0;
    std::vector<Section> sections;
};

enum class DylibKind { Load, Weak, Reexport, Lazy, Upward };

struct Dylib {
    DylibKind kind = DylibKind::Load;
    std::string path;
    uint32_t timestamp = 0, current_version = 0, compat_version = 0;
    uint32_t ordinal = 0;  // 1-based, in load order (LC_ID_DYLIB has ordinal 0)
};

struct EntryPoint {
    enum class Kind { Main, UnixThread } kind = Kind::Main;
    uint64_t file_offset = 0;  // LC_MAIN only
    uint64_t vmaddr = 0;       // resolved virtual address (header-relative image base not applied)
    uint64_t stack_size = 0;   // LC_MAIN only
    std::string segment;       // segment that contains the entry
};

struct BuildVersion {
    uint32_t platform = 0, minos = 0, sdk = 0;
    std::vector<std::pair<uint32_t, uint32_t>> tools;  // (tool, version)
    bool legacy = false;                               // came from LC_VERSION_MIN_*
};

struct LinkeditRange {
    uint32_t offset = 0, size = 0;
};

struct SymtabInfo {
    uint32_t symoff = 0, nsyms = 0, stroff = 0, strsize = 0;
};

struct DysymtabInfo {
    uint32_t ilocalsym = 0, nlocalsym = 0, iextdefsym = 0, nextdefsym = 0, iundefsym = 0, nundefsym = 0;
    uint32_t nindirectsyms = 0;
};

struct DyldInfo {
    LinkeditRange rebase, bind, weak_bind, lazy_bind, exports;
    bool only = false;  // LC_DYLD_INFO_ONLY
};

struct Symbol {
    std::string name;
    uint8_t type = 0, section = 0;
    uint16_t desc = 0;
    uint64_t value = 0;
    bool external = false, undefined = false, debug = false;
    uint8_t library_ordinal = 0;  // two-level namespace ordinal for undefined symbols (0 = self)
};

struct CodeSignature {
    uint32_t dataoff = 0, datasize = 0;
    bool blob_parsed = false;  // SuperBlob + CodeDirectory structurally parsed
    bool has_code_directory = false;
    bool has_entitlements = false;
    bool has_requirements = false;
    bool has_cms_signature = false;  // CMS blob present => not ad-hoc
    bool adhoc = false;
    bool hardened_runtime = false;
    uint32_t cd_version = 0, cd_flags = 0, code_slots = 0;
    uint8_t hash_type = 0;
    std::string identifier, team_id;
    // NOTE: the signature is parsed, NOT cryptographically verified.
};

struct Image {
    // header
    uint32_t cputype = 0, cpusubtype = 0, filetype = 0, ncmds = 0, sizeofcmds = 0, flags = 0;
    uint64_t slice_size = 0;
    std::vector<Segment> segments;
    std::vector<Dylib> dylibs;           // LC_LOAD_DYLIB & friends
    std::optional<Dylib> id_dylib;       // LC_ID_DYLIB
    std::vector<std::string> rpaths;
    std::string dylinker;                // LC_LOAD_DYLINKER path
    std::optional<EntryPoint> entry;
    std::vector<BuildVersion> build_versions;
    std::optional<std::string> uuid;     // canonical text form
    std::optional<SymtabInfo> symtab;
    std::optional<DysymtabInfo> dysymtab;
    std::optional<DyldInfo> dyld_info;
    std::optional<LinkeditRange> chained_fixups, exports_trie, function_starts;
    std::optional<CodeSignature> code_signature;
    bool encrypted = false;              // LC_ENCRYPTION_INFO_64 with cryptid != 0
    std::vector<Symbol> symbols;         // only filled when ParseOptions::parse_symbols
    std::vector<std::string> warnings;   // non-fatal oddities
    std::vector<uint32_t> load_commands; // command ids, in order

    bool is_64() const noexcept { return (cputype & kCpuArchABI64) != 0; }
    const Segment* find_segment(const std::string& name) const;
    const Segment* segment_for_fileoff(uint64_t off) const;
    const Segment* segment_for_vmaddr(uint64_t addr) const;
};

struct Slice {
    uint32_t cputype = 0, cpusubtype = 0;
    uint64_t offset = 0, size = 0;
    uint32_t align = 0;
    std::optional<Image> image;   // present if the slice parsed
    std::optional<Error> error;   // present if it did not
};

struct File {
    bool fat = false, fat64 = false;
    uint64_t file_size = 0;
    std::vector<Slice> slices;  // thin files have exactly one slice at offset 0
    std::vector<std::string> warnings;

    const Slice* find_slice(uint32_t cputype) const;
};

// ---- options & API ---------------------------------------------------------------------
struct Limits {
    uint64_t max_file_size = 512ull << 20;  // used by callers that read files
    uint32_t max_load_commands = 65535;
    uint32_t max_sizeofcmds = 16u << 20;
    uint32_t max_fat_archs = 64;
    uint32_t max_symbols = 1u << 22;
};

struct ParseOptions {
    bool parse_symbols = false;
    Limits limits;
};

Format identify(ByteView data) noexcept;
Result<File> parse_file(ByteView data, const ParseOptions& opts = {});
Result<Image> parse_image(ByteView slice, const ParseOptions& opts = {});

// ---- names -----------------------------------------------------------------------------
std::string cpu_name(uint32_t cputype, uint32_t cpusubtype);
const char* filetype_name(uint32_t filetype) noexcept;
const char* platform_name(uint32_t platform) noexcept;
const char* dylib_kind_name(DylibKind k) noexcept;
std::vector<std::string> flag_names(uint32_t flags);

}  // namespace cider::macho

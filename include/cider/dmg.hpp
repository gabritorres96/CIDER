#pragma once
// cider-dmg: DMG (Apple disk image) detection, UDIF parsing and optional 7-Zip extraction.
//
// Structure analysis (udif.cpp) is pure C++ and reads only the parts of the file it needs.
// Extraction (sevenzip.cpp) is delegated to an EXTERNAL, OPTIONAL 7-Zip executable; CIDER does
// not decompress HFS+/APFS itself. Nothing extracted is ever executed.
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "cider/result.hpp"

namespace cider::dmg {

// ---- structure ---------------------------------------------------------------------------
enum class Kind {
    Unknown,    // not recognised as a disk image
    Udif,       // 'koly' trailer present (the usual .dmg)
    Encrypted,  // encrcdsa / cdsaencr header: password protected
    Sparse,     // sparse image ('sprs')
    RawImage,   // headerless raw disk image (filesystem or partition scheme found at the start)
};
const char* to_string(Kind k) noexcept;

enum class Compression { Zero, Raw, Ignore, Adc, Zlib, Bzip2, Lzfse, Lzma, Unknown };
const char* to_string(Compression c) noexcept;

struct UdifTrailer {
    uint32_t version = 0, header_size = 0, flags = 0;
    uint64_t running_data_fork_offset = 0, data_fork_offset = 0, data_fork_length = 0;
    uint64_t rsrc_fork_offset = 0, rsrc_fork_length = 0;
    uint32_t segment_number = 0, segment_count = 0;
    uint32_t data_checksum_type = 0, master_checksum_type = 0, image_variant = 0;
    uint64_t xml_offset = 0, xml_length = 0, sector_count = 0;
    bool flattened() const noexcept { return (flags & 1) != 0; }
};

struct ChunkStats {
    uint64_t chunks = 0;
    uint64_t sectors = 0;
};

struct Partition {
    std::string name;            // e.g. "disk image (Apple_HFS : 2)"
    std::string id;
    std::string declared_type;   // "Apple_HFS", "Apple_APFS", ... parsed from the name (a hint only)
    uint64_t sector_count = 0;
    std::vector<std::pair<Compression, ChunkStats>> compression;  // in first-seen order
    std::string filesystem;      // "" when unknown
    bool filesystem_verified = false;  // true: signature read from the data; false: inferred from the name
};

struct Info {
    Kind kind = Kind::Unknown;
    uint64_t file_size = 0;
    std::optional<UdifTrailer> udif;
    std::vector<Partition> partitions;
    std::vector<Compression> compressions;  // union over partitions
    std::vector<std::string> filesystems;   // union, e.g. "HFS+ (verified)"
    std::string raw_description;            // RawImage: what was recognised
    std::vector<std::string> warnings;
};

struct InspectLimits {
    uint64_t max_xml_bytes = 64ull << 20;
    uint32_t max_partitions = 256;
    uint32_t max_chunks_per_partition = 1u << 20;
};

// Detects the image by content (never by extension) and parses what is parseable.
// Returns an error only for corrupt/unreadable files; unsupported kinds (encrypted, sparse,
// unknown) are reported through Info::kind.
Result<Info> inspect_file(const std::filesystem::path& image, const InspectLimits& limits = {});

// Filesystem signature scan over the first bytes of a volume (exposed for tests).
std::string detect_filesystem(const uint8_t* data, size_t size);

// ---- 7-Zip backend -------------------------------------------------------------------------
struct Backend {
    std::filesystem::path exe;
    std::string banner;  // first line printed by 7-Zip
    int major = 0, minor = 0;
};

// Looks for 7-Zip: explicit path, $CIDER_7Z, PATH (absolute entries only), Program Files.
// The candidate is run once without arguments to confirm it is really 7-Zip.
Result<Backend> locate_7zip(const std::filesystem::path& explicit_path = {});

struct ArchiveItem {
    std::string path;       // normalised with '/'
    bool is_dir = false;
    bool encrypted = false;
    uint64_t size = 0;
};

struct Listing {
    std::string type;       // "Dmg", "HFS", "Iso", ... as reported by 7-Zip ("" if unknown)
    std::vector<ArchiveItem> items;
    uint64_t total_size = 0;
};

struct ExtractLimits {
    uint64_t max_total_bytes = 8ull << 30;
    uint64_t max_files = 200000;
};

// Rejects absolute paths, drive letters, '..', ':' (alternate streams), reserved device names and
// listings that exceed the limits. Pure function: no filesystem access.
Status validate_listing(const std::vector<ArchiveItem>& items, const ExtractLimits& limits);
Status validate_item_path(const std::string& path);

// type_switch (e.g. "dmg") forces one 7-Zip handler (-t<type>) so nested archives are not listed too.
Result<Listing> list_archive(const Backend& be, const std::filesystem::path& archive,
                             std::chrono::milliseconds timeout = std::chrono::minutes(5), const std::string& type_switch = {});

struct ExtractedPartition {
    std::string name;       // directory name inside the output directory
    std::string type;       // 7-Zip archive type of the partition ("HFS", "Iso", ...)
    uint64_t files = 0, bytes = 0;
};

struct ExtractOptions {
    ExtractLimits limits;
    std::chrono::milliseconds timeout = std::chrono::minutes(15);
    std::filesystem::path temp_base;   // where the private work directory is created (default: system temp)
};

struct ExtractReport {
    std::filesystem::path output;
    std::vector<ExtractedPartition> partitions;
    std::vector<std::string> apps;      // relative to `output`, '/' separated
    std::vector<std::string> warnings;
    uint64_t total_bytes = 0, total_files = 0, links_removed = 0;
};

// Extracts every filesystem found in the image into `output` (which must not exist or be empty)
// and locates *.app bundles. Intermediate files live in a private temporary directory that is
// removed before returning, also on failure.
Result<ExtractReport> extract_image(const Backend& be, const std::filesystem::path& image,
                                    const std::filesystem::path& output, const ExtractOptions& opts = {});

struct TreeStats {
    uint64_t files = 0, bytes = 0, links_removed = 0;
};

// Post-extraction check: deletes symlinks/reparse points (never followed; each is reported in
// `notes`), rejects entries outside `root` and enforces the limits. Returns file/byte totals.
Result<TreeStats> verify_extracted_tree(const std::filesystem::path& root, const ExtractLimits& limits,
                                        std::vector<std::string>* notes = nullptr);

// *.app directories below `root` (not descending into found apps), relative '/' paths.
std::vector<std::string> find_apps(const std::filesystem::path& root, size_t max_depth = 8);

// Private temporary directory, removed (best effort, read-only attributes cleared) on destruction.
class TempDir {
public:
    explicit TempDir(const std::filesystem::path& base = {});
    ~TempDir();
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    const std::filesystem::path& path() const noexcept { return path_; }
    bool ok() const noexcept { return !path_.empty(); }
    bool remove();  // true when nothing is left behind

private:
    std::filesystem::path path_;
};

}  // namespace cider::dmg

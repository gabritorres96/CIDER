#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "orvyn/result.hpp"

namespace orvyn {

inline constexpr const char* kVersion = "0.5.0";

std::string hex(uint64_t value);  // "0x1f"
std::string version_string(uint32_t packed_xxxx_yy_zz);  // 0x000a0f00 -> "10.15"
std::string sanitize_utf8(std::string_view s);           // invalid sequences -> U+FFFD
std::string to_lower_ascii(std::string_view s);
bool starts_with(std::string_view s, std::string_view prefix) noexcept;
bool ends_with_icase(std::string_view s, std::string_view suffix) noexcept;

std::filesystem::path path_from_utf8(std::string_view s);
// Windows: absolute extended-length form (\\?\C:\...) so paths longer than MAX_PATH (260) work
// without the OS "long paths" setting. No-op elsewhere. path_to_utf8() strips the prefix again, so
// displayed paths and paths handed to external tools stay normal.
std::filesystem::path extended_path(const std::filesystem::path& p);
std::string path_to_utf8(const std::filesystem::path& p);

// Reads a whole regular file, refusing files larger than max_size.
Result<std::vector<uint8_t>> read_file(const std::filesystem::path& p, uint64_t max_size);

// Environment variable with a legacy name. ORVYN was called CIDER until 0.5.x; the user-facing variables ORVYN_LOG_FILE, ORVYN_TEMP_DIR and ORVYN_7Z
// still honour CIDER_LOG_FILE, CIDER_TEMP_DIR and CIDER_7Z when the new name is unset or empty (documented temporary alias, see docs/research/CIDER-TO-ORVYN-MIGRATION.md).
const char* getenv_compat(const char* name, const char* legacy_name);

// True when `child` is lexically/physically inside `root` (both resolved with weakly_canonical).
bool is_inside(const std::filesystem::path& root, const std::filesystem::path& child);

}  // namespace orvyn

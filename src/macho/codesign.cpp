#include "codesign.hpp"

#include <format>

namespace cider::macho {
namespace {
constexpr uint32_t kSuperBlob = 0xfade0cc0, kCodeDirectory = 0xfade0c02, kRequirements = 0xfade0c01,
                   kEntitlements = 0xfade7171, kDerEntitlements = 0xfade7172, kCms = 0xfade0b01;
constexpr uint32_t kSlotCodeDirectory = 0, kSlotCms = 0x10000;
constexpr uint32_t kFlagAdhoc = 0x2, kFlagRuntime = 0x10000;

void parse_code_directory(ByteView cd, CodeSignature& out, std::vector<std::string>& warnings) {
    if (cd.size() < 44) { warnings.emplace_back("code signature: CodeDirectory too small"); return; }
    out.has_code_directory = true;
    out.cd_version = cd.peek_u32(8, true);
    out.cd_flags = cd.peek_u32(12, true);
    uint32_t ident_off = cd.peek_u32(20, true);
    out.code_slots = cd.peek_u32(28, true);
    out.hash_type = static_cast<uint8_t>(cd.u8(37).ok() ? cd.u8(37).value() : 0);
    out.adhoc = (out.cd_flags & kFlagAdhoc) != 0;
    out.hardened_runtime = (out.cd_flags & kFlagRuntime) != 0;
    if (auto id = cd.cstring(ident_off, cd.size()); id.ok()) out.identifier = id.value();
    else warnings.push_back("code signature: invalid identifier offset");
    if (out.cd_version >= 0x20200 && cd.size() >= 52) {
        uint32_t team_off = cd.peek_u32(48, true);
        if (team_off) {
            if (auto t = cd.cstring(team_off, cd.size()); t.ok()) out.team_id = t.value();
            else warnings.push_back("code signature: invalid team id offset");
        }
    }
}
}  // namespace

void parse_code_signature(ByteView blob, CodeSignature& out, std::vector<std::string>& warnings) {
    if (blob.size() < 12) { warnings.emplace_back("code signature: blob smaller than SuperBlob header"); return; }
    if (blob.peek_u32(0, true) != kSuperBlob) {
        warnings.emplace_back("code signature: bad SuperBlob magic");
        return;
    }
    uint32_t length = blob.peek_u32(4, true), count = blob.peek_u32(8, true);
    if (length < 12 || length > blob.size()) { warnings.emplace_back("code signature: invalid SuperBlob length"); return; }
    auto table_bytes = checked_mul(count, 8);
    if (!table_bytes || *table_bytes > length - 12) {
        warnings.emplace_back("code signature: SuperBlob index exceeds blob");
        return;
    }
    auto sb = blob.slice(0, length).value();
    out.blob_parsed = true;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t type = sb.peek_u32(12 + 8ull * i, true);
        uint32_t off = sb.peek_u32(12 + 8ull * i + 4, true);
        if (!sb.contains(off, 8)) { warnings.push_back(std::format("code signature: blob #{} offset out of range", i)); continue; }
        uint32_t magic = sb.peek_u32(off, true), len = sb.peek_u32(off + 4ull, true);
        if (len < 8 || !sb.contains(off, len)) { warnings.push_back(std::format("code signature: blob #{} length invalid", i)); continue; }
        auto b = sb.slice(off, len).value();
        if (type == kSlotCodeDirectory && magic == kCodeDirectory) parse_code_directory(b, out, warnings);
        else if (magic == kRequirements) out.has_requirements = true;
        else if (magic == kEntitlements || magic == kDerEntitlements) out.has_entitlements = true;
        else if (type == kSlotCms && magic == kCms) out.has_cms_signature = true;
    }
    if (!out.has_code_directory) warnings.emplace_back("code signature: no CodeDirectory found");
}

}  // namespace cider::macho

// cider-dmg: structure parsing, validation, process runner and (when 7-Zip is installed) extraction.
#include <algorithm>
#include <cstdlib>
#include <optional>
#include <random>

#include "cider/dmg.hpp"
#include "cider/process.hpp"
#include "cider/util.hpp"
#include "dmg_builder.hpp"
#include "test_framework.hpp"

using namespace cider;
using namespace cider::testing;
namespace fs = std::filesystem;

namespace {

dmg::Info inspect_bytes(const TempDir& t, const std::vector<uint8_t>& b, const char* name = "x.dmg") {
    auto p = t.write(name, b);
    auto r = dmg::inspect_file(p);
    if (!r.ok()) { report_failure(__FILE__, __LINE__, "inspect_file failed: " + r.error().message); throw AbortTest{}; }
    return r.value();
}

Result<dmg::Info> try_inspect(const TempDir& t, const std::vector<uint8_t>& b) {
    return dmg::inspect_file(t.write("y.dmg", b));
}

std::vector<uint8_t> iso_image() { return build_iso(example_app_tree()); }

const std::optional<dmg::Backend>& backend() {
    static std::optional<dmg::Backend> be = [] {
        auto r = dmg::locate_7zip();
        return r.ok() ? std::optional<dmg::Backend>(r.value()) : std::nullopt;
    }();
    return be;
}

#define NEED_7ZIP() do { if (!backend()) SKIP("7-Zip not installed"); } while (0)

dmg::ExtractOptions opts_in(const TempDir& base) {
    dmg::ExtractOptions o;
    o.temp_base = base.path();
    return o;
}

size_t count_entries(const fs::path& p) {
    std::error_code ec;
    size_t n = 0;
    for ([[maybe_unused]] auto& e : fs::directory_iterator(p, ec)) ++n;
    return n;
}

}  // namespace

// ---- detection & UDIF structure -------------------------------------------------------------
CIDER_TEST(udif_raw_image_is_parsed) {
    TempDir t;
    auto info = inspect_bytes(t, build_dmg(iso_image()).bytes);
    CHECK(info.kind == dmg::Kind::Udif);
    REQUIRE(info.udif.has_value());
    CHECK_EQ(info.udif->version, 4u);
    CHECK(info.udif->flattened());
    CHECK_EQ(info.udif->sector_count, uint64_t{200});
    REQUIRE_EQ(info.partitions.size(), size_t{1});
    CHECK_EQ(info.partitions[0].declared_type, std::string("CIDER_ISO"));
    REQUIRE_EQ(info.compressions.size(), size_t{1});
    CHECK(info.compressions[0] == dmg::Compression::Raw);
    CHECK_EQ(info.partitions[0].filesystem, std::string("ISO 9660 + Joliet"));
    CHECK(info.partitions[0].filesystem_verified);
    CHECK(info.warnings.empty());
}

CIDER_TEST(udif_zlib_image_reports_compression_and_no_verified_fs) {
    TempDir t;
    DmgOptions o;
    o.codec = DmgCodec::ZlibStored;
    auto info = inspect_bytes(t, build_dmg(iso_image(), o).bytes);
    REQUIRE_EQ(info.compressions.size(), size_t{1});
    CHECK(info.compressions[0] == dmg::Compression::Zlib);
    CHECK(info.partitions[0].filesystem.empty());  // compressed data is not decompressed by CIDER
    CHECK(!info.partitions[0].filesystem_verified);
}

CIDER_TEST(filesystem_inferred_from_partition_name_is_marked_as_inferred) {
    TempDir t;
    for (auto [name, expect] : {std::pair{"disk image (Apple_HFS : 1)", "HFS+"}, std::pair{"disk image (Apple_APFS : 1)", "APFS"},
                                std::pair{"hfsx (Apple_HFSX : 2)", "HFSX"}}) {
        DmgOptions o;
        o.codec = DmgCodec::ZlibStored;
        o.partition_name = name;
        auto info = inspect_bytes(t, build_dmg(iso_image(), o).bytes);
        CHECK_EQ(info.partitions[0].filesystem, std::string(expect));
        CHECK(!info.partitions[0].filesystem_verified);
        CHECK(info.filesystems[0].find("inferred") != std::string::npos);
    }
}

CIDER_TEST(detection_is_by_content_not_extension) {
    TempDir t;
    auto dmgbytes = build_dmg(iso_image()).bytes;
    CHECK(inspect_bytes(t, dmgbytes, "renamed.txt").kind == dmg::Kind::Udif);
    CHECK(inspect_bytes(t, dmgbytes, "noextension").kind == dmg::Kind::Udif);
    std::vector<uint8_t> junk(5000, 0x41);
    CHECK(inspect_bytes(t, junk, "fake.dmg").kind == dmg::Kind::Unknown);
}

CIDER_TEST(encrypted_sparse_and_raw_images) {
    TempDir t;
    std::vector<uint8_t> enc(4096, 0);
    std::copy_n("encrcdsa", 8, enc.begin());
    CHECK(inspect_bytes(t, enc).kind == dmg::Kind::Encrypted);
    std::copy_n("cdsaencr", 8, enc.begin());
    CHECK(inspect_bytes(t, enc).kind == dmg::Kind::Encrypted);
    std::vector<uint8_t> sparse(4096, 0);
    std::copy_n("sprs", 4, sparse.begin());
    CHECK(inspect_bytes(t, sparse).kind == dmg::Kind::Sparse);

    auto raw_iso = inspect_bytes(t, iso_image(), "raw.img");
    CHECK(raw_iso.kind == dmg::Kind::RawImage);
    CHECK_EQ(raw_iso.raw_description, std::string("ISO 9660 + Joliet"));

    std::vector<uint8_t> hfs(8192, 0);
    hfs[1024] = 'H'; hfs[1025] = '+';
    CHECK_EQ(inspect_bytes(t, hfs).raw_description, std::string("HFS+"));
    std::vector<uint8_t> apfs(8192, 0);
    std::copy_n("NXSB", 4, apfs.begin() + 32);
    CHECK_EQ(inspect_bytes(t, apfs).raw_description, std::string("APFS"));
    std::vector<uint8_t> gpt(8192, 0);
    std::copy_n("EFI PART", 8, gpt.begin() + 512);
    CHECK_EQ(inspect_bytes(t, gpt).raw_description, std::string("GPT partition table"));
}

CIDER_TEST(filesystem_signature_scan) {
    std::vector<uint8_t> v(40000, 0);
    CHECK(dmg::detect_filesystem(v.data(), v.size()).empty());
    std::copy_n("NTFS    ", 8, v.begin() + 3);
    CHECK_EQ(dmg::detect_filesystem(v.data(), v.size()), std::string("NTFS"));
    CHECK(dmg::detect_filesystem(v.data(), 5).empty());  // never reads past the buffer
    CHECK(dmg::detect_filesystem(nullptr, 0).empty());
}

// ---- malformed images -----------------------------------------------------------------------
CIDER_TEST(empty_and_tiny_files) {
    TempDir t;
    CHECK_ERR(try_inspect(t, {}), ErrorCode::Truncated);
    CHECK(inspect_bytes(t, {1, 2, 3}).kind == dmg::Kind::Unknown);
    CHECK_ERR(dmg::inspect_file(t.path() / "missing.dmg"), ErrorCode::NotFound);
}

CIDER_TEST(bad_koly_fields_rejected) {
    TempDir t;
    auto good = build_dmg(iso_image()).bytes;
    auto b = good;
    patch_koly32(b, 8, 100);  // header size
    CHECK_ERR(try_inspect(t, b), ErrorCode::Malformed);
    b = good;
    patch_koly32(b, 4, 9);  // version: warning only
    auto r = try_inspect(t, b);
    REQUIRE(r.ok());
    CHECK(!r.value().warnings.empty());
    b = good;
    patch_koly32(b, 60, 3);  // segment count
    CHECK_ERR(try_inspect(t, b), ErrorCode::Unsupported);
    b = good;
    patch_koly64(b, 32, 0x7fffffffffffffffull);  // data fork length
    CHECK_ERR(try_inspect(t, b), ErrorCode::OutOfBounds);
    b = good;
    patch_koly64(b, 32, 0xffffffffffffff00ull);  // overflow
    CHECK_ERR(try_inspect(t, b), ErrorCode::OutOfBounds);
    b = good;
    patch_koly64(b, 216, good.size());  // xml offset past the file
    CHECK_ERR(try_inspect(t, b), ErrorCode::OutOfBounds);
    b = good;
    patch_koly64(b, 216, 0xfffffffffffffff0ull);  // xml offset overflow
    CHECK_ERR(try_inspect(t, b), ErrorCode::OutOfBounds);
    b = good;
    patch_koly64(b, 224, 100ull << 20);  // xml too large
    CHECK_ERR(try_inspect(t, b), ErrorCode::LimitExceeded);
}

CIDER_TEST(garbage_xml_and_missing_blkx_rejected) {
    TempDir t;
    auto good = build_dmg(iso_image());
    auto b = good.bytes;
    std::string text(b.begin() + static_cast<std::ptrdiff_t>(good.xml_offset), b.begin() + static_cast<std::ptrdiff_t>(good.xml_offset + good.xml_length));
    size_t at = text.find("<array>");
    REQUIRE(at != std::string::npos);
    std::copy_n("<<<<<<<", 7, b.begin() + static_cast<std::ptrdiff_t>(good.xml_offset + at));  // break the plist structure
    CHECK_ERR(try_inspect(t, b), ErrorCode::Malformed);
    b = good.bytes;
    size_t k = text.find("blkx");
    REQUIRE(k != std::string::npos);
    std::copy_n("nope", 4, b.begin() + static_cast<std::ptrdiff_t>(good.xml_offset + k));  // valid plist, no blkx array
    CHECK_ERR(try_inspect(t, b), ErrorCode::Malformed);

    DmgOptions o;
    o.include_blkx = false;
    auto none = inspect_bytes(t, build_dmg(iso_image(), o).bytes);
    CHECK(none.kind == dmg::Kind::Udif);
    CHECK(none.partitions.empty());
}

CIDER_TEST(corrupt_block_tables_rejected) {
    TempDir t;
    struct { DmgCorrupt c; ErrorCode e; } cases[] = {
        {DmgCorrupt::BadMishMagic, ErrorCode::Malformed},
        {DmgCorrupt::TruncatedMish, ErrorCode::Malformed},
        {DmgCorrupt::ChunkPastFork, ErrorCode::OutOfBounds},
        {DmgCorrupt::ChunkSectorsPastPartition, ErrorCode::OutOfBounds},
        {DmgCorrupt::HugeChunkCount, ErrorCode::LimitExceeded},
    };
    for (auto& c : cases) {
        DmgOptions o;
        o.corrupt = c.c;
        auto r = try_inspect(t, build_dmg(iso_image(), o).bytes);
        CHECK(!r.ok());
        if (!r.ok()) CHECK(r.error().code == c.e);
    }
}

CIDER_TEST(truncated_dmg_never_crashes) {
    TempDir t;
    auto good = build_dmg(iso_image()).bytes;
    for (size_t cut = 0; cut < good.size(); cut += 4099) {
        std::vector<uint8_t> c(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(cut));
        auto r = try_inspect(t, c);
        if (r.ok()) CHECK(r.value().kind != dmg::Kind::Udif);  // the koly trailer is gone
    }
    // cutting only the trailer's tail keeps the file from being recognised as UDIF
    std::vector<uint8_t> nohead(good.begin(), good.end() - 3);
    CHECK(try_inspect(t, nohead).ok() ? try_inspect(t, nohead).value().kind != dmg::Kind::Udif : true);
}

CIDER_TEST(mutated_dmg_trailer_and_plist_never_crash) {
    TempDir t;
    auto g = build_dmg(iso_image());
    std::mt19937_64 rng(99);
    for (int i = 0; i < 400; ++i) {
        auto b = g.bytes;
        for (int e = 0; e < 3; ++e) {
            size_t region_start = (rng() & 1) ? b.size() - 512 : g.xml_offset;
            size_t pos = region_start + rng() % (b.size() - region_start);
            b[pos] = static_cast<uint8_t>(rng());
        }
        (void)try_inspect(t, b);
    }
    CHECK(true);
}

// ---- path & listing validation ------------------------------------------------------------------
CIDER_TEST(item_path_validation) {
    for (const char* ok : {"a", "dir/file.txt", "dir\\file.txt", "Example.app/Contents/MacOS/Example", "a/./b", "dir/", "..hidden", "a..b",
                           "file."})
        CHECK(dmg::validate_item_path(ok).ok());
    for (const char* bad : {"", "/etc/passwd", "\\windows\\x", "C:\\x", "C:x", "../x", "a/../../x", "a\\..\\..\\x", "..", "a/..", ".. ", "... ",
                            "a/b:stream", "con", "CON.txt", "dir/nul", "dir/COM1.log", "lpt9", "a/\xE2\x80\xAE/.."})
        CHECK(!dmg::validate_item_path(bad).ok());
    CHECK(!dmg::validate_item_path(std::string("a\0b", 3)).ok());
}

CIDER_TEST(listing_limits_and_encryption) {
    dmg::ExtractLimits lim;
    lim.max_total_bytes = 1000;
    lim.max_files = 3;
    CHECK(dmg::validate_listing({{"a", false, false, 400}, {"b", false, false, 500}}, lim).ok());
    CHECK_ERR(dmg::validate_listing({{"a", false, false, 600}, {"b", false, false, 500}}, lim), ErrorCode::LimitExceeded);
    CHECK_ERR(dmg::validate_listing({{"a", false, false, UINT64_MAX}, {"b", false, false, 2}}, lim), ErrorCode::LimitExceeded);
    CHECK_ERR(dmg::validate_listing({{"a", true, false, 0}, {"b", true, false, 0}, {"c", true, false, 0}, {"d", true, false, 0}}, lim),
              ErrorCode::LimitExceeded);
    CHECK_ERR(dmg::validate_listing({{"a", false, true, 1}}, lim), ErrorCode::Unsupported);
    CHECK_ERR(dmg::validate_listing({{"ok", false, false, 1}, {"../evil", false, false, 1}}, lim), ErrorCode::Malformed);
}

CIDER_TEST(find_apps_locates_bundles_without_descending_into_them) {
    TempDir t;
    t.write("vol/Applications/One.app/Contents/Info.plist", std::string("x"));
    t.write("vol/Applications/One.app/Contents/Frameworks/Nested.app/Contents/Info.plist", std::string("x"));
    t.write("vol/Two.APP/Contents/Info.plist", std::string("x"));
    t.write("vol/Docs/readme.txt", std::string("x"));
    t.write("vol/Docs/NotAnApp.txt", std::string("x"));
    auto apps = dmg::find_apps(t.path());
    REQUIRE_EQ(apps.size(), size_t{2});
    CHECK_EQ(apps[0], std::string("vol/Applications/One.app"));
    CHECK_EQ(apps[1], std::string("vol/Two.APP"));
    CHECK(dmg::find_apps(t.path() / "missing").empty());
}

CIDER_TEST(tempdir_is_private_and_removed_even_with_readonly_files) {
    fs::path where;
    {
        dmg::TempDir d;
        REQUIRE(d.ok());
        where = d.path();
        CHECK(fs::exists(where));
        fs::create_directories(where / "sub");
        std::ofstream(where / "sub" / "ro.txt") << "x";
        std::error_code ec;
        fs::permissions(where / "sub" / "ro.txt", fs::perms::owner_write, fs::perm_options::remove, ec);
    }
    CHECK(!fs::exists(where));
    dmg::TempDir a, b;
    CHECK(a.path() != b.path());
}

// ---- process runner ---------------------------------------------------------------------------------
CIDER_TEST(windows_argument_quoting) {
    CHECK_EQ(quote_windows_arg("plain"), std::string("plain"));
    CHECK_EQ(quote_windows_arg("a b"), std::string("\"a b\""));
    CHECK_EQ(quote_windows_arg(""), std::string("\"\""));
    CHECK_EQ(quote_windows_arg("say \"hi\""), std::string("\"say \\\"hi\\\"\""));
    CHECK_EQ(quote_windows_arg("C:\\dir with space\\"), std::string("\"C:\\dir with space\\\\\""));
}

#ifdef _WIN32
static fs::path system_exe(const char* name) {
    const char* root = std::getenv("SystemRoot");
    return fs::path(root ? root : "C:\\Windows") / "System32" / name;
}

CIDER_TEST(process_runner_captures_output_and_exit_code) {
    auto r = run_process(system_exe("cmd.exe"), {"/c", "echo hello & exit 7"}, std::chrono::seconds(20));
    REQUIRE(r.ok());
    if (r.value().exit_code != 7) std::printf("    cmd output: [%s]\n", r.value().output.c_str());
    CHECK_EQ(r.value().exit_code, 7);
    CHECK(r.value().output.find("hello") != std::string::npos);
    CHECK(!r.value().timed_out);
}

CIDER_TEST(process_runner_enforces_timeout_and_absolute_path) {
    auto r = run_process(system_exe("ping.exe"), {"-n", "30", "127.0.0.1"}, std::chrono::milliseconds(400));
    REQUIRE(r.ok());
    CHECK(r.value().timed_out);
    CHECK_ERR(run_process("cmd.exe", {"/c", "echo x"}, std::chrono::seconds(5)), ErrorCode::InvalidArgument);
    CHECK(!run_process(system_exe("does-not-exist.exe"), {}, std::chrono::seconds(5)).ok());
}

CIDER_TEST(process_runner_caps_output) {
    auto r = run_process(system_exe("cmd.exe"), {"/c", "for /l %i in (1,1,5000) do @echo line %i"}, std::chrono::seconds(30), 1024);
    CHECK_ERR(r, ErrorCode::LimitExceeded);
}
#endif

// ---- 7-Zip backend --------------------------------------------------------------------------------------
CIDER_TEST(backend_rejects_non_7zip_and_relative_paths) {
#ifdef _WIN32
    CHECK(!dmg::locate_7zip(system_exe("ping.exe")).ok());
#endif
    CHECK_ERR(dmg::locate_7zip("relative/7z.exe"), ErrorCode::InvalidArgument);
    TempDir t;
    CHECK_ERR(dmg::locate_7zip(t.path() / "missing-7z.exe"), ErrorCode::NotFound);
}

CIDER_TEST(backend_located_and_lists_the_image) {
    NEED_7ZIP();
    CHECK(backend()->major >= 9);
    TempDir t;
    auto img = t.write("a.dmg", build_dmg(iso_image()).bytes);
    auto l = dmg::list_archive(*backend(), img);
    REQUIRE(l.ok());
    CHECK_EQ(l.value().type, std::string("Dmg"));
    CHECK(l.value().items.size() >= 2);
    CHECK(dmg::list_archive(*backend(), t.path() / "missing.dmg").ok() == false);
}

CIDER_TEST(extract_raw_dmg_finds_and_exposes_the_app) {
    NEED_7ZIP();
    TempDir t;
    auto img = t.write("raw.dmg", build_dmg(iso_image()).bytes);
    auto rep = dmg::extract_image(*backend(), img, t.path() / "out", opts_in(t));
    REQUIRE(rep.ok());
    REQUIRE_EQ(rep.value().partitions.size(), size_t{1});
    CHECK_EQ(rep.value().partitions[0].type, std::string("Iso"));
    REQUIRE_EQ(rep.value().apps.size(), size_t{1});
    CHECK(rep.value().apps[0].find("Example.app") != std::string::npos);
    CHECK(fs::exists(t.path() / "out" / path_from_utf8(rep.value().apps[0]) / "Contents" / "MacOS" / "Example"));
    CHECK(rep.value().total_files >= 5);
    // no intermediate work directories are left in the temp base
    size_t leftovers = 0;
    for (auto& e : fs::directory_iterator(t.path()))
        if (path_to_utf8(e.path().filename()).rfind("cider-", 0) == 0) ++leftovers;
    CHECK_EQ(leftovers, size_t{0});
}

CIDER_TEST(extract_zlib_dmg_works) {
    NEED_7ZIP();
    TempDir t;
    DmgOptions o;
    o.codec = DmgCodec::ZlibStored;
    auto img = t.write("z.dmg", build_dmg(iso_image(), o).bytes);
    auto rep = dmg::extract_image(*backend(), img, t.path() / "out", opts_in(t));
    REQUIRE(rep.ok());
    CHECK_EQ(rep.value().apps.size(), size_t{1});
}

CIDER_TEST(extract_raw_iso_without_udif_wrapper) {
    NEED_7ZIP();
    TempDir t;
    auto img = t.write("plain.iso", iso_image());
    auto rep = dmg::extract_image(*backend(), img, t.path() / "out", opts_in(t));
    REQUIRE(rep.ok());
    CHECK_EQ(rep.value().apps.size(), size_t{1});
}

CIDER_TEST(extract_respects_size_limit_and_cleans_up) {
    NEED_7ZIP();
    TempDir t;
    auto img = t.write("big.dmg", build_dmg(iso_image()).bytes);
    auto o = opts_in(t);
    o.limits.max_total_bytes = 2000;  // image expands to ~100 KiB
    auto rep = dmg::extract_image(*backend(), img, t.path() / "out", o);
    CHECK_ERR(rep, ErrorCode::LimitExceeded);
    CHECK(!fs::exists(t.path() / "out"));
    for (auto& e : fs::directory_iterator(t.path())) CHECK(path_to_utf8(e.path().filename()).rfind("cider-", 0) != 0);

    auto o2 = opts_in(t);
    o2.limits.max_files = 3;
    CHECK_ERR(dmg::extract_image(*backend(), img, t.path() / "out2", o2), ErrorCode::LimitExceeded);
    CHECK(!fs::exists(t.path() / "out2"));
}

CIDER_TEST(extract_refuses_non_empty_output_and_keeps_it_untouched) {
    NEED_7ZIP();
    TempDir t;
    auto img = t.write("a.dmg", build_dmg(iso_image()).bytes);
    t.write("out/keep.txt", std::string("precious"));
    CHECK_ERR(dmg::extract_image(*backend(), img, t.path() / "out", opts_in(t)), ErrorCode::InvalidArgument);
    CHECK(fs::exists(t.path() / "out" / "keep.txt"));
    t.write("file-not-dir", std::string("x"));
    CHECK_ERR(dmg::extract_image(*backend(), img, t.path() / "file-not-dir", opts_in(t)), ErrorCode::InvalidArgument);
}

CIDER_TEST(extract_into_existing_empty_output_dir_and_failure_keeps_dir) {
    NEED_7ZIP();
    TempDir t;
    auto img = t.write("a.dmg", build_dmg(iso_image()).bytes);
    fs::create_directories(t.path() / "empty-out");
    CHECK(dmg::extract_image(*backend(), img, t.path() / "empty-out", opts_in(t)).ok());
    CHECK(fs::exists(t.path() / "empty-out"));

    fs::create_directories(t.path() / "empty-out2");
    auto o = opts_in(t);
    o.limits.max_total_bytes = 10;
    CHECK(!dmg::extract_image(*backend(), img, t.path() / "empty-out2", o).ok());
    CHECK(fs::exists(t.path() / "empty-out2"));          // the caller's directory is kept...
    CHECK_EQ(count_entries(t.path() / "empty-out2"), size_t{0});  // ...but emptied
}

CIDER_TEST(corrupt_payload_with_valid_trailer_fails_cleanly) {
    NEED_7ZIP();
    TempDir t;
    auto good = build_dmg(iso_image()).bytes;
    for (size_t i = 0; i < 2000; ++i) good[3000 + i] ^= 0xA5;  // damage the partition data, trailer stays valid
    auto img = t.write("c.dmg", good);
    auto rep = dmg::extract_image(*backend(), img, t.path() / "out", opts_in(t));
    // Either 7-Zip reports an error or the damaged region simply holds no .app: never a crash/hang.
    if (rep.ok()) CHECK(rep.value().apps.size() <= 1);
    for (auto& e : fs::directory_iterator(t.path())) CHECK(path_to_utf8(e.path().filename()).rfind("cider-", 0) != 0);
}

CIDER_TEST(garbage_with_dmg_extension_is_not_extracted) {
    NEED_7ZIP();
    TempDir t;
    auto img = t.write("junk.dmg", std::vector<uint8_t>(10000, 0x55));
    auto rep = dmg::extract_image(*backend(), img, t.path() / "out", opts_in(t));
    CHECK(!rep.ok());
    CHECK(!fs::exists(t.path() / "out"));
}

CIDER_TEST(path_traversal_names_never_escape_the_output_directory) {
    NEED_7ZIP();
    TempDir t;
    FsNode root = dir("", {file("..\\..\\evil.txt", "pwned"), file("../escape2.txt", "pwned"), file("ok.txt", "fine"),
                           dir("sub", {file("..\\up.txt", "pwned")})});
    DmgOptions o;
    auto img = t.write("evil.dmg", build_dmg(build_iso(root), o).bytes);
    auto rep = dmg::extract_image(*backend(), img, t.path() / "deep" / "out", opts_in(t));
    // Whether CIDER rejects the image or 7-Zip neutralises the names, nothing may land outside.
    for (const char* leak : {"evil.txt", "escape2.txt", "up.txt"}) {
        CHECK(!fs::exists(t.path() / leak));
        CHECK(!fs::exists(t.path() / "deep" / leak));
    }
    if (rep.ok()) {
        std::error_code ec;
        for (auto& e : fs::recursive_directory_iterator(t.path() / "deep" / "out", ec)) CHECK(is_inside(t.path() / "deep" / "out", e.path()));
    }
}

// ---- regressions from validating real hdiutil images (CIDER 0.1.1) -------------------------------------------------
// Real images wrap the filesystem in an MBR + GPT header/table + free-space partitions. Extracting
// those as "filesystems" made 7-Zip fail ("Unexpected end of data").
CIDER_TEST(regression_real_layout_with_gpt_metadata_partitions_extracts_cleanly) {
    TempDir t;
    DmgOptions o;
    o.real_layout = true;
    o.codec = DmgCodec::ZlibStored;
    o.partition_name = "disk image (Apple_HFS : 3)";
    auto bytes = build_dmg(iso_image(), o).bytes;
    auto info = inspect_bytes(t, bytes);
    CHECK_EQ(info.partitions.size(), size_t{5});
    CHECK(std::find(info.compressions.begin(), info.compressions.end(), dmg::Compression::Zlib) != info.compressions.end());
    NEED_7ZIP();
    auto img = t.write("real.dmg", bytes);
    auto rep = dmg::extract_image(*backend(), img, t.path() / "out", opts_in(t));
    REQUIRE(rep.ok());
    CHECK_EQ(rep.value().partitions.size(), size_t{1});
    CHECK_EQ(rep.value().apps.size(), size_t{1});
    CHECK(rep.value().warnings.empty());  // small metadata partitions are ignored silently
}

// Real apps nest deeper than MAX_PATH (260) once extracted (MonitorControl.app failed with
// "cannot stat extracted entry"): CIDER now uses extended-length paths for its own file operations.
CIDER_TEST(regression_extraction_deeper_than_max_path) {
    NEED_7ZIP();
    TempDir t;
    // Deep.app nested under six 60-character directories (> 360 characters before the app name)
    FsNode node = dir("Deep.app", {dir("Contents", {file("Info.plist", xml_plist({{"CFBundleExecutable", "x"}})),
                                                     dir("MacOS", {file("x", build_macho({}).bytes)})})});
    for (int i = 5; i >= 0; --i) node = dir(std::string(59, 'd') + std::to_string(i), {node});
    FsNode tree = dir("", {node});
    auto img = t.write("deep.dmg", build_dmg(build_iso(tree)).bytes);
    fs::path out = t.path() / "out";
    auto rep = dmg::extract_image(*backend(), img, out, opts_in(t));
    REQUIRE(rep.ok());
    REQUIRE_EQ(rep.value().apps.size(), size_t{1});
    CHECK(rep.value().apps[0].size() > 300);  // longer than MAX_PATH even without the output prefix
    CHECK(fs::exists(extended_path(out) / path_from_utf8(rep.value().apps[0]) / "Contents" / "Info.plist"));
}

CIDER_TEST(extended_path_helper) {
#ifdef _WIN32
    auto e = extended_path("C:\\Windows");
    CHECK_EQ(path_to_utf8(e), std::string("C:/Windows"));       // prefix is stripped for display / external tools
    CHECK(e.native().rfind(L"\\\\?\\", 0) == 0);
    CHECK(extended_path(e).native() == e.native());            // idempotent
    CHECK_EQ(path_to_utf8(extended_path("\\\\server\\share\\x")), std::string("//server/share/x"));
#endif
    CHECK_EQ(path_to_utf8(fs::path("a/b")), std::string("a/b"));
}

// Real images contain symlinks (the /Applications alias, framework Versions/Current). They are removed,
// never followed, and the target stays untouched.
CIDER_TEST(regression_verify_removes_links_without_touching_their_targets) {
    TempDir t;
    t.write("tree/keep.txt", std::string("data"));
    t.write("outside/precious.txt", std::string("precious"));
    std::error_code ec;
    fs::create_directory_symlink(t.path() / "outside", t.path() / "tree" / "Applications", ec);
    if (ec) SKIP("cannot create symlinks on this machine (needs Developer Mode or admin)");
    fs::create_symlink(t.path() / "outside" / "precious.txt", t.path() / "tree" / "file-link", ec);
    std::vector<std::string> notes;
    auto st = dmg::verify_extracted_tree(extended_path(t.path() / "tree"), {}, &notes);
    REQUIRE(st.ok());
    CHECK_EQ(st.value().files, uint64_t{1});
    CHECK(st.value().links_removed >= 1);
    CHECK(!notes.empty());
    CHECK(!fs::exists(fs::symlink_status(t.path() / "tree" / "Applications")));
    CHECK(fs::exists(t.path() / "outside" / "precious.txt"));
    CHECK(fs::exists(t.path() / "tree" / "keep.txt"));
    // limits still apply
    dmg::ExtractLimits tiny;
    tiny.max_total_bytes = 2;
    CHECK_ERR(dmg::verify_extracted_tree(t.path() / "tree", tiny, nullptr), ErrorCode::LimitExceeded);
}

// ---- real-world images (optional) ------------------------------------------------------------------------------
// Set CIDER_REAL_DMG_DIR to a directory with genuine DMGs (tools/validate-real-dmgs.ps1 downloads
// three official open-source releases there). Nothing is downloaded or executed by the test-suite.
CIDER_TEST(real_world_dmgs_are_parsed_and_extracted) {
    const char* dirp = std::getenv("CIDER_REAL_DMG_DIR");
    if (!dirp) SKIP("CIDER_REAL_DMG_DIR not set");
    size_t seen = 0;
    for (auto& e : fs::directory_iterator(path_from_utf8(dirp))) {
        if (!e.is_regular_file() || !ends_with_icase(path_to_utf8(e.path()), ".dmg")) continue;
        ++seen;
        std::string label = path_to_utf8(e.path().filename());
        auto info = dmg::inspect_file(e.path());
        if (!info.ok()) { report_failure(__FILE__, __LINE__, label + ": " + info.error().message); continue; }
        CHECK(info.value().kind == dmg::Kind::Udif);
        CHECK(info.value().warnings.empty());
        CHECK(info.value().partitions.size() >= 1);
        bool has_fs_partition = false;
        for (auto& p : info.value().partitions) has_fs_partition |= p.declared_type == "Apple_HFS" || p.declared_type == "Apple_APFS";
        CHECK(has_fs_partition);
        CHECK(std::find(info.value().compressions.begin(), info.value().compressions.end(), dmg::Compression::Zlib) != info.value().compressions.end());
        uint64_t sectors = 0;
        for (auto& p : info.value().partitions) sectors += p.sector_count;
        CHECK_EQ(sectors, info.value().udif->sector_count);  // partition table accounts for the whole image

        if (!backend()) continue;
        TempDir t;
        auto rep = dmg::extract_image(*backend(), e.path(), t.path() / "out", opts_in(t));
        if (!rep.ok()) { report_failure(__FILE__, __LINE__, label + ": extraction failed: " + rep.error().message); continue; }
        CHECK(rep.value().apps.size() >= 1);
        CHECK(rep.value().total_files > 10);
        CHECK(rep.value().links_removed >= 1);  // the /Applications alias at least
        std::printf("    %s: %zu partition(s), %zu app(s), %llu files, %llu link(s) not recreated\n", label.c_str(), info.value().partitions.size(),
                    rep.value().apps.size(), static_cast<unsigned long long>(rep.value().total_files), static_cast<unsigned long long>(rep.value().links_removed));
    }
    CHECK(seen > 0);
}

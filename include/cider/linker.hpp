#pragma once
// CIDER 0.3 dynamic linker: loads a Mach-O executable together with every Mach-O dylib it needs and links
// them with each other.
//
//  * dependency search follows dyld's install-name rules for absolute paths, @executable_path, @loader_path
//    and @rpath (LC_RPATH of the loader first, then of the images that loaded it);
//  * every image file is loaded ONCE however many images depend on it; cycles are detected;
//  * libraries are real Mach-O dylibs: CIDER never substitutes Windows DLLs, and never loads macOS system
//    libraries (those exist only as CIDER's own libSystem subset);
//  * all images are mapped first, then rebased/bound (symbols are resolved through the dependency's export
//    trie, following LC_REEXPORT_DYLIB and re-export entries), then protected (W^X), and only then are
//    Mach-O initializers (__mod_init_func / __init_offsets) run, dependencies first, each exactly once;
//  * destroying the Program unmaps every image.
//
// Not a general dyld: no dlopen, no DYLD_* variables, no shared cache, no interposing, no weak-symbol
// coalescing across images, no two-level hints. See docs/DYNAMIC-LINKING.md.
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cider/dyld.hpp"
#include "cider/exports.hpp"
#include "cider/fixups.hpp"
#include "cider/loader.hpp"
#include "cider/macho.hpp"

namespace cider::linker {

inline constexpr int kProvidedDep = -2;   // dependency implemented by CIDER itself (libSystem)
inline constexpr int kAbsentDep = -1;     // weak dependency that was not found

enum class InitState { NotStarted, Running, Done };

struct ImageRecord {
    size_t index = 0;
    bool is_main = false;
    std::string name;                 // install name (dylib) or the main executable's file name
    std::string leaf;                 // file name, for messages
    std::filesystem::path path;       // empty for an executable given only as bytes
    macho::Image info;
    std::vector<uint8_t> owned_bytes; // file contents (dependencies); released once linking is done
    ByteView slice;                   // the x86_64 slice; valid only while linking
    loader::LoadedImage mapped;
    std::vector<int> deps;            // one per LC_LOAD_* command (info.dylibs): image index, kProvidedDep or kAbsentDep
    std::vector<dyld::ExportedSymbol> exports;
    std::map<std::string, size_t, std::less<>> export_index;
    std::vector<uint64_t> initializers;   // absolute addresses, in call order
    InitState init_state = InitState::NotStarted;
    uint64_t header = 0;              // address of the Mach-O header in memory
    struct Tls {
        uint64_t tmpl = 0, tmpl_size = 0, init_size = 0, align = 1;   // thread-local template (run-time addresses)
        uint64_t descriptors = 0;                                      // number of TLV descriptors
        bool present() const noexcept { return tmpl_size != 0 || descriptors != 0; }
    } tls;
    size_t rebases = 0, binds = 0;
};

struct LinkOptions {
    std::filesystem::path executable_path;  // where the main executable lives (anchors for @executable_path/@rpath)
    loader::ValidatorOptions validator;     // limits; the dynamic-linking flags are forced on
    bool reject_cycles = false;             // default: cycles are legal (dyld accepts them) but reported
    size_t max_images = 128;
    size_t max_depth = 64;
    uint64_t max_file_size = 512ull << 20;
    // Called once per image after every bind was applied and BEFORE the final W^X protections (the data segments are still
    // writable): the Objective-C runtime uses it to uniquify selector references and slide instance-variable offsets.
    // Return false (and fill `error`) to abort the link.
    std::function<bool(ImageRecord& image, std::string& error)> before_protect;
};

struct LinkError {
    loader::LoadFailure kind = loader::LoadFailure::Internal;
    std::string message;
    std::vector<dyld::Unresolved> unresolved;
    std::vector<std::string> required_libs;  // install names seen so far
    std::string missing_library;             // MissingDependency: the install name that could not be found
};

// Result of the callback that runs one initializer on the guest stack.
struct InitCall {
    bool exited = false;        // the initializer called exit()
    uint64_t exit_code = 0;
};
using InitCaller = std::function<InitCall(uint64_t function, const ImageRecord& image)>;

struct InitResult {
    enum class Status { Ok, Exited, Failed } status = Status::Ok;
    uint64_t exit_code = 0;     // Exited
    std::string message;        // Failed
    size_t calls = 0;           // initializers actually executed
};

class Program {
public:
    ~Program();
    Program(const Program&) = delete;
    Program& operator=(const Program&) = delete;

    // Loads `main_file` and everything it depends on. nullptr on failure, with `error` filled in; nothing
    // stays mapped after a failure. `libsystem` stands in for /usr/lib/libSystem.B.dylib and must outlive
    // the returned Program.
    static std::unique_ptr<Program> link(ByteView main_file, const LinkOptions& opts, dyld::SymbolProvider& libsystem, LinkError& error);

    const std::vector<std::unique_ptr<ImageRecord>>& images() const noexcept { return images_; }
    ImageRecord& main() { return *images_.front(); }
    const ImageRecord& main() const { return *images_.front(); }
    uint64_t entry_address() const { return main().mapped.entry_address(); }
    const std::vector<std::string>& required_libs() const noexcept { return required_libs_; }
    const std::vector<std::string>& cycles() const noexcept { return cycles_; }

    // Symbol exported by image `image` (following re-exports); nullopt when it is not exported.
    std::optional<uint64_t> find_symbol(size_t image, std::string_view name) const;
    // Looks `name` up in the main executable, then every library in load order.
    std::optional<uint64_t> find_symbol_anywhere(std::string_view name) const;

    // Dependencies first (post-order over the load commands), the main executable last; back edges of a cycle are skipped.
    std::vector<size_t> initialization_order() const;
    // Runs every initializer once. Stops at the first exit() request. Never re-enters an image that is already running.
    InitResult run_initializers(const InitCaller& call);

    // Releases every image now (also done by the destructor). Safe to call twice.
    void unload();

private:
    Program() = default;
    std::vector<std::unique_ptr<ImageRecord>> images_;
    std::vector<std::string> required_libs_;
    std::vector<std::string> cycles_;
    dyld::SymbolProvider* libsystem_ = nullptr;

    friend class Linker;
    struct Lookup;
    Lookup lookup(size_t image, std::string_view name, std::vector<bool>& visited, int depth) const;
};

}  // namespace cider::linker

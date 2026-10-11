#include "orvyn/guest.hpp"
#include "orvyn/log.hpp"
#include "orvyn/util.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace orvyn::guest {

int worker_main(const std::filesystem::path& image, const std::vector<std::string>& guest_args, const std::filesystem::path& result_file,
                const std::filesystem::path& root) {
#ifdef _WIN32
    // No Windows Error Reporting / critical-error dialogs if the guest crashes: the supervisor sees the exit code.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#endif
    Outcome out;
    auto bytes = read_file(image, 512u << 20);
    if (!bytes.ok()) {
        out.state = State::LoadFailed;
        out.load_failure = loader::LoadFailure::Incompatible;
        out.message = bytes.error().message;
    } else {
        log::info(log::Category::Process, "worker loading {}", path_to_utf8(image));
        GuestConfig cfg;
        cfg.args.push_back(path_to_utf8(image));
        cfg.args.insert(cfg.args.end(), guest_args.begin(), guest_args.end());
        cfg.env = {"ORVYN_RUNTIME=1"};
        cfg.image_path = image;
        cfg.root = root;
        out = run_in_process(ByteView(bytes.value()), cfg);
    }
    if (result_file.empty()) return 0;
    if (!write_result_file(result_file, out)) {
        log::error("cannot write the worker result file");
        return 70;
    }
    log::info(log::Category::Process, "cleanup completed");
    return 0;
}

}  // namespace orvyn::guest

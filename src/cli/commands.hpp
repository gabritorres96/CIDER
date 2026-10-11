#pragma once
#include <string>
#include <vector>

namespace orvyn::cli {

struct Options {
    bool json = false;
    bool symbols = false;
    std::string output;        // --output / -o
    std::string seven_zip;     // --7z <path>
    std::string temp_dir;      // --temp-dir <path> (private work directories are created here)
    std::vector<std::string> guest_args;   // after "--": argv[1..] of the guest
    std::string result_file;               // internal: worker result path
    std::string root;                      // run --root: host directory the guest sees as "/"
    unsigned long long timeout_s = 0;      // --timeout for run (0 = default)
    unsigned long long max_size_mib = 0;  // --max-size <MiB>; 0 = default
    std::vector<std::string> positional;
};

// Exit codes: 0 success, 1 analysis failed / file invalid, 2 usage error or unavailable command,
// 3 partial result (e.g. disk image structure analysed but the 7-Zip backend is unavailable).
int cmd_inspect(const Options& o);
int cmd_dmg_info(const Options& o);
int cmd_extract(const Options& o);
int cmd_dependencies(const Options& o);
int cmd_compatibility(const Options& o);
int cmd_run(const Options& o);
int cmd_worker(const Options& o);
int cmd_logs(const Options& o);
int cmd_doctor(const Options& o);
int cmd_sdk_scan(const Options& o);

}  // namespace orvyn::cli

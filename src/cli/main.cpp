#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#endif

#include "orvyn/log.hpp"
#include "orvyn/util.hpp"
#include "commands.hpp"

namespace {

const char* kUsage =
    "ORVYN - Darwin Executable Runtime " "0.2.0\n"
    "\n"
    "Usage: orvyn <command> [options]\n"
    "\n"
    "Commands:\n"
    "  inspect <program|App.app|x.dmg>  Static Mach-O / bundle / disk image inspection (auto-detected)\n"
    "  dmg-info <image.dmg>             Disk image structure, compression, filesystem, 7-Zip status\n"
    "  extract <image.dmg> -o <dir>     Extract a disk image with 7-Zip and locate .app bundles\n"
    "  dependencies <program|App.app>   Dynamic library dependencies and their resolution\n"
    "  compatibility <program|App.app>  Static compatibility assessment\n"
    "  run <program|App.app>            Run an x86_64 Mach-O (dylibs, initializers, TLS) in a job-restricted worker\n"
    "  logs [file]                      Show the tail of a ORVYN log file\n"
    "  doctor                           Check the environment\n"
    "\n"
    "Options:\n"
    "  --json                 Machine-readable output\n"
    "  --symbols              Include the symbol table (inspect)\n"
    "  --timeout <seconds>    run: kill the guest after this many seconds (default 60)\n"
    "  --root <dir>           run: host directory the guest sees as \"/\" (default: current directory)\n"
    "  -o, --output <dir>    Output directory (extract; must not exist or be empty)\n"
    "  --7z <path>            Absolute path of 7z.exe (default: $ORVYN_7Z, PATH, Program Files)\n"
    "  --max-size <MiB>       Maximum unpacked size accepted when extracting (default 8192)\n"
    "  --temp-dir <dir>       Where private temporary work directories are created\n"
    "  --log-level <level>    trace|debug|info|warn|error|fatal|off (default: warn)\n"
    "  --log-file <path>      Also append log lines to this file\n"
    "  --version              Print the version\n"
    "  --help                 Print this help\n";

std::vector<std::string> utf8_args(int argc, char** argv) {
#ifdef _WIN32
    (void)argc; (void)argv;
    int n = 0;
    LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &n);
    std::vector<std::string> out;
    for (int i = 0; w && i < n; ++i) {
        int len = WideCharToMultiByte(CP_UTF8, 0, w[i], -1, nullptr, 0, nullptr, nullptr);
        std::string s(len > 0 ? static_cast<size_t>(len - 1) : 0, '\0');
        if (len > 1) WideCharToMultiByte(CP_UTF8, 0, w[i], -1, s.data(), len, nullptr, nullptr);
        out.push_back(std::move(s));
    }
    if (w) LocalFree(w);
    return out;
#else
    return std::vector<std::string>(argv, argv + argc);
#endif
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    auto args = utf8_args(argc, argv);
    orvyn::cli::Options opt;
    std::string command;
    std::string log_file;
    orvyn::log::Level level = orvyn::log::Level::Warn;
    bool level_set = false;

    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--") {  // everything after is passed to the guest program (orvyn run <program> -- args...)
            opt.guest_args.assign(args.begin() + static_cast<std::ptrdiff_t>(i) + 1, args.end());
            break;
        }
        if (a == "--version" || a == "-V") { std::printf("orvyn %s\n", orvyn::kVersion); return 0; }
        if (a == "--help" || a == "-h") { std::fputs(kUsage, stdout); return 0; }
        if (a == "--json") opt.json = true;
        else if (a == "--symbols") opt.symbols = true;
        else if (a == "--verbose" || a == "-v") { level = orvyn::log::Level::Debug; level_set = true; }
        else if (a == "--output" || a == "-o" || a == "--7z" || a == "--temp-dir" || a == "--max-size" || a == "--timeout" || a == "--result-file" || a == "--root") {
            if (i + 1 >= args.size()) { std::fprintf(stderr, "error: %s needs a value\n", a.c_str()); return 2; }
            const std::string& v = args[++i];
            if (a == "--output" || a == "-o") opt.output = v;
            else if (a == "--7z") opt.seven_zip = v;
            else if (a == "--temp-dir") opt.temp_dir = v;
            else if (a == "--result-file") opt.result_file = v;
            else if (a == "--root") opt.root = v;
            else if (a == "--timeout") {
                char* end = nullptr;
                opt.timeout_s = std::strtoull(v.c_str(), &end, 10);
                if (v.empty() || *end != '\0' || opt.timeout_s == 0) {
                    std::fprintf(stderr, "error: --timeout expects a positive number of seconds\n");
                    return 2;
                }
            } else {
                char* end = nullptr;
                opt.max_size_mib = std::strtoull(v.c_str(), &end, 10);
                if (v.empty() || *end != '\0' || opt.max_size_mib == 0) {
                    std::fprintf(stderr, "error: --max-size expects a positive number of MiB\n");
                    return 2;
                }
            }
        }
        else if (a == "--log-level" || a == "--log-file") {
            if (i + 1 >= args.size()) { std::fprintf(stderr, "error: %s needs a value\n", a.c_str()); return 2; }
            const std::string& v = args[++i];
            if (a == "--log-file") log_file = v;
            else if (!orvyn::log::parse_level(v, level)) { std::fprintf(stderr, "error: unknown log level '%s'\n", v.c_str()); return 2; }
            else level_set = true;
        } else if (a.size() > 1 && a[0] == '-' && a != "-") {
            std::fprintf(stderr, "error: unknown option '%s' (try --help)\n", a.c_str());
            return 2;
        } else if (command.empty()) command = a;
        else opt.positional.push_back(a);
    }

    if (command == "run" && !level_set && !opt.json) level = orvyn::log::Level::Info;  // run shows what the runtime does
    orvyn::log::set_level(level);
    if (log_file.empty()) if (const char* e = orvyn::getenv_compat("ORVYN_LOG_FILE", "CIDER_LOG_FILE")) log_file = e;
    if (!log_file.empty() && command != "logs" && !orvyn::log::set_file(log_file))
        std::fprintf(stderr, "warning: cannot open log file '%s'\n", log_file.c_str());
    // The worker's stderr belongs to the guest: runtime log lines go to the log file only.
    if (command == "__worker" && !log_file.empty()) orvyn::log::set_stderr_enabled(false);

    if (command.empty()) { std::fputs(kUsage, stderr); return 2; }
    int rc;
    if (command == "inspect") rc = orvyn::cli::cmd_inspect(opt);
    else if (command == "dmg-info") rc = orvyn::cli::cmd_dmg_info(opt);
    else if (command == "extract") rc = orvyn::cli::cmd_extract(opt);
    else if (command == "dependencies") rc = orvyn::cli::cmd_dependencies(opt);
    else if (command == "compatibility") rc = orvyn::cli::cmd_compatibility(opt);
    else if (command == "run") rc = orvyn::cli::cmd_run(opt);
    else if (command == "__worker") rc = orvyn::cli::cmd_worker(opt);  // internal: started by `orvyn run`
    else if (command == "logs") rc = orvyn::cli::cmd_logs(opt);
    else if (command == "doctor") rc = orvyn::cli::cmd_doctor(opt);
    else if (command == "sdk-scan") rc = orvyn::cli::cmd_sdk_scan(opt);
    else { std::fprintf(stderr, "error: unknown command '%s' (try --help)\n", command.c_str()); rc = 2; }
    orvyn::log::close_file();
    return rc;
}

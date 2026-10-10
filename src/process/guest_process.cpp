#include <format>

#include "cider/abi.hpp"
#include "cider/guest.hpp"
#include "cider/libsystem.hpp"
#include "cider/log.hpp"

namespace cider::guest {

const char* to_string(State s) noexcept {
    switch (s) {
        case State::NotRun: return "not-run";
        case State::LoadFailed: return "load-failed";
        case State::Exited: return "exited";
        case State::Faulted: return "faulted";
    }
    return "?";
}

namespace {
Outcome load_failed(const loader::LoadError& e) {
    Outcome o;
    o.state = State::LoadFailed;
    o.load_failure = e.kind;
    o.message = e.message;
    o.required_libs = e.report.required_libs;
    o.unresolved = e.report.unresolved;
    return o;
}
}  // namespace

Outcome run_in_process(ByteView file, const GuestConfig& cfg, const loader::ValidatorOptions& vopts) {
    Outcome out;
    auto provider = libsystem::LibSystemProvider::create();
    if (!provider.ok()) {
        out.state = State::LoadFailed;
        out.load_failure = loader::LoadFailure::Internal;
        out.message = "cannot initialise libSystem: " + provider.error().message;
        return out;
    }

    // ---- loading: validation, mapping, symbols, fixups, protections (no guest instruction runs) ----
    loader::LoadResult lr = loader::load_macho(file, *provider.value(), vopts);
    if (!lr.ok()) {
        out = load_failed(*lr.error);
        return out;  // the supervisor reports the diagnostics (unresolved symbols, library, reason)
    }
    loader::LoadedImage& image = *lr.image;
    out.required_libs = lr.report.required_libs;
    out.entry_address = image.entry_address();
    out.image_base = image.actual_base();

    // ---- execution environment: stack, argv/envp/apple -------------------------------------------
    log::info(log::Category::Abi, "preparing the guest execution environment");
    auto stack = abi::GuestStack::create(static_cast<size_t>(image.layout.stack_size));
    if (!stack.ok()) {
        out.state = State::LoadFailed;
        out.load_failure = loader::LoadFailure::Internal;
        out.message = "cannot create the guest stack: " + stack.error().message;
        return out;
    }
    std::vector<std::string> argv = cfg.args.empty() ? std::vector<std::string>{"guest"} : cfg.args;
    std::vector<std::string> apple = {"executable_path=" + argv[0]};
    auto ga = abi::build_guest_args(stack.value(), argv, cfg.env, apple);
    if (!ga.ok()) {
        out.state = State::LoadFailed;
        out.load_failure = loader::LoadFailure::Internal;
        out.message = "cannot build the guest arguments: " + ga.error().message;
        return out;
    }

    libsystem::RuntimeContext ctx;
    ctx.env = cfg.env;
    ctx.sink = cfg.sink;
    libsystem::set_context(&ctx);

    // ---- transfer of control -----------------------------------------------------------------------
    // LC_MAIN protocol: dyld calls main(argc, argv, envp, apple) on the process stack and passes the
    // return value to exit(). We do exactly that: main is the System V function at the entry point.
    log::info(log::Category::Execution, "entering guest code at {:#x}", image.entry_address());
    abi::CallOutcome r = abi::call_guest(image.entry_address(), stack.value(), ga.value().initial_sp, ga.value().argc, ga.value().argv,
                                         ga.value().envp, ga.value().apple);
    libsystem::set_context(nullptr);

    if (!ctx.fault.empty()) {
        out.state = State::Faulted;
        out.message = ctx.fault;
        log::error("guest faulted: {}", ctx.fault);
    } else {
        out.state = State::Exited;
        out.exit_code = static_cast<int32_t>(static_cast<uint32_t>(r.value));
        out.returned_from_main = !r.exited;
        log::info(log::Category::Execution, "guest {} {}", r.exited ? "called exit with" : "returned", out.exit_code);
    }
    // image, stack and thunk pages are released by their destructors when this function returns
    return out;
}

}  // namespace cider::guest

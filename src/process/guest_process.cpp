#include <format>

#include "cider/abi.hpp"
#include "cider/guest.hpp"
#include "cider/guest_threads.hpp"
#include "cider/libsystem.hpp"
#include "cider/linker.hpp"
#include "cider/objc.hpp"
#include "cider/log.hpp"
#include "cider/util.hpp"

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
Outcome load_failed(const linker::LinkError& e) {
    Outcome o;
    o.state = State::LoadFailed;
    o.load_failure = e.kind;
    o.message = e.message;
    o.required_libs = e.required_libs;
    o.unresolved = e.unresolved;
    o.missing_library = e.missing_library;
    return o;
}

Outcome internal_failure(std::string message) {
    Outcome o;
    o.state = State::LoadFailed;
    o.load_failure = loader::LoadFailure::Internal;
    o.message = std::move(message);
    return o;
}
}  // namespace

Outcome run_in_process(ByteView file, const GuestConfig& cfg, const loader::ValidatorOptions& vopts) {
    Outcome out;
    auto provider = libsystem::LibSystemProvider::create();
    if (!provider.ok()) return internal_failure("cannot initialise libSystem: " + provider.error().message);

    libsystem::RuntimeContext ctx;
    ctx.env = cfg.env;
    ctx.sink = cfg.sink;
    ctx.root = cfg.root;
    provider.value()->install_stdio(ctx);

    // ---- Objective-C runtime and the host-implemented classes (created before linking: binds to _OBJC_CLASS_$_NSObject and
    // friends resolve to them, and the linker hands every image's metadata to the runtime before the final protections) ----
    ctx.objc = std::make_unique<objc::Runtime>(ctx);
    objc::install_foundation(*ctx.objc);
    {
        std::string why;
        if (ctx.objc->failed() || !ctx.objc->build_native_thunks(why)) return internal_failure("cannot initialise the Objective-C runtime: " + why);
    }
    objc::Runtime* objc_rt = ctx.objc.get();
    provider.value()->set_dynamic_data([objc_rt](std::string_view symbol) { return objc_rt->dynamic_symbol(symbol); });

    // ---- loading: the dynamic linker maps the executable and every dylib, resolves symbols between them,
    // applies fixups and the final W^X protections. No guest instruction runs yet. -------------------------
    linker::LinkOptions lo;
    lo.before_protect = [objc_rt](linker::ImageRecord& image, std::string& error) { return objc_rt->prepare_image(image, error); };
    lo.executable_path = cfg.image_path;
    lo.validator = vopts;
    lo.reject_cycles = cfg.reject_cycles;
    linker::LinkError le;
    std::unique_ptr<linker::Program> program = linker::Program::link(file, lo, *provider.value(), le);
    if (!program) return load_failed(le);   // the supervisor reports the diagnostics (unresolved symbols, library, reason)

    out.required_libs = program->required_libs();
    out.entry_address = program->entry_address();
    out.image_base = program->main().mapped.actual_base();
    for (const auto& r : program->images()) {
        LoadedImageInfo li;
        li.name = r->name;
        li.path = r->path.empty() ? std::string() : path_to_utf8(r->path);
        li.base = r->header;
        li.initializers = r->initializers.size();
        li.main = r->is_main;
        out.images.push_back(std::move(li));
    }

    // ---- execution environment: stack, argv/envp/apple -------------------------------------------
    log::info(log::Category::Abi, "preparing the guest execution environment");
    auto stack = abi::GuestStack::create(static_cast<size_t>(program->main().mapped.layout.stack_size));
    if (!stack.ok()) return internal_failure("cannot create the guest stack: " + stack.error().message);
    std::vector<std::string> argv = cfg.args.empty() ? std::vector<std::string>{"guest"} : cfg.args;
    std::vector<std::string> apple = {"executable_path=" + argv[0]};
    auto ga = abi::build_guest_args(stack.value(), argv, cfg.env, apple);
    if (!ga.ok()) return internal_failure("cannot build the guest arguments: " + ga.error().message);

    {
        std::string why;
        if (!objc_rt->finish_images(why)) {
            Outcome o;
            o.state = State::LoadFailed;
            o.load_failure = loader::LoadFailure::Incompatible;
            o.message = "Objective-C: " + why;
            o.required_libs = program->required_libs();
            return o;
        }
    }
    if (objc_rt->foundation_requested) log::info(log::Category::Objc, "Foundation compatibility loaded");
    // Callbacks may only target executable code of the loaded images and the libSystem thunks.
    for (const auto& r : program->images())
        for (const auto& s : r->mapped.layout.segments)
            if (s.final_prot == memory::Prot::ReadExecute) {
                const uint64_t seg_lo = static_cast<uint64_t>(static_cast<int64_t>(s.vmaddr) + r->mapped.slide);
                ctx.code_ranges.emplace_back(seg_lo, seg_lo + s.vmsize);
            }
    ctx.code_ranges.push_back(provider.value()->thunk_range());
    if (auto r = objc_rt->thunk_range(); r.first) ctx.code_ranges.push_back(r);
    for (const auto& r : program->images()) {
        if (!r->tls.present()) continue;
        libsystem::TlsImage t;
        t.lo = r->mapped.actual_base();
        t.hi = t.lo + r->mapped.layout.span;
        t.tmpl = r->tls.tmpl;
        t.tmpl_size = r->tls.tmpl_size;
        t.init_size = r->tls.init_size;
        t.align = r->tls.align;
        ctx.tls_images.push_back(t);
    }
    libsystem::set_context(&ctx);
    ctx.threads = std::make_unique<libsystem::GuestThreads>(ctx);
    ctx.threads->attach_main();                      // this OS thread is guest thread 1 and owns the guest lock

    // ---- Objective-C +load, then Mach-O initializers ---------------------------------------------------------
    objc_rt->set_top_level_stack(&stack.value(), ga.value().initial_sp);
    if (objc_rt->has_objc_images()) {
        const abi::GuestArgs& ga0 = ga.value();
        objc_rt->run_load_methods([&](objc::Addr imp, objc::Addr self, objc::Addr sel) { abi::call_guest(imp, stack.value(), ga0.initial_sp, self, sel); });
    }

    // ---- Mach-O initializers (never Windows constructors): dependencies first, each exactly once ----------
    // dyld calls them as init(argc, argv, envp, apple).
    const abi::GuestArgs& g = ga.value();
    linker::InitResult ir = program->run_initializers([&](uint64_t fn, const linker::ImageRecord&) {
        abi::CallOutcome c = abi::call_guest(fn, stack.value(), g.initial_sp, g.argc, g.argv, g.envp, g.apple);
        return linker::InitCall{c.exited, c.value};
    });
    out.initializers_run = ir.calls;
    if (ir.status == linker::InitResult::Status::Exited || !ctx.fault.empty()) {
        libsystem::set_context(nullptr);
        if (!ctx.fault.empty()) {
            out.state = State::Faulted;
            out.message = ctx.fault;
            log::error("guest faulted: {}", ctx.fault);
        } else {
            out.state = State::Exited;
            out.exit_code = static_cast<int32_t>(static_cast<uint32_t>(ir.exit_code));
            out.returned_from_main = false;
            out.message = ir.message;
            log::info(log::Category::Execution, "guest called exit with {} during initialization; main was not run", out.exit_code);
        }
        return out;
    }

    // ---- transfer of control -----------------------------------------------------------------------
    // LC_MAIN protocol: dyld calls main(argc, argv, envp, apple) on the process stack and passes the
    // return value to exit(). We do exactly that: main is the System V function at the entry point.
    log::info(log::Category::Execution, "entering guest code at {:#x}", program->entry_address());
    log::info(log::Category::Execution, "Guest started");
    abi::CallOutcome r = abi::call_guest(program->entry_address(), stack.value(), g.initial_sp, g.argc, g.argv, g.envp, g.apple);

    // atexit handlers: reverse order, after main returned or exit() was called (not after _exit or a trap). A handler that calls
    // exit() replaces the exit code and ends the sequence.
    uint64_t final_code = r.value;
    while (ctx.fault.empty() && !ctx.skip_atexit && !ctx.atexit_handlers.empty()) {
        const uint64_t fn = ctx.atexit_handlers.back();
        ctx.atexit_handlers.pop_back();
        log::info(log::Category::Execution, "running atexit handler at {:#x}", fn);
        abi::CallOutcome h = abi::call_guest(fn, stack.value(), g.initial_sp);
        if (h.exited) { final_code = h.value; break; }
    }
    out.objc_live_objects = objc_rt->live_object_count();
    // The process ends here: other guest threads (if any) are told to unwind and joined.
    if (!ctx.threads->shutdown()) log::warn(log::Category::Execution, "some guest threads did not stop; the worker exit will end them");
    libsystem::set_context(nullptr);

    if (!ctx.fault.empty()) {
        out.state = State::Faulted;
        out.message = ctx.fault;
        log::error("guest faulted: {}", ctx.fault);
    } else {
        out.state = State::Exited;
        out.exit_code = static_cast<int32_t>(static_cast<uint32_t>(final_code));
        out.returned_from_main = !r.exited;
        log::info(log::Category::Execution, "guest {} {}", r.exited ? "called exit with" : "returned", out.exit_code);
    }
    // images, stack and thunk pages are released by their destructors when this function returns
    return out;
}

}  // namespace cider::guest

#pragma once
// CIDER's Objective-C runtime (0.4): a from-scratch subset of the APPLE ("objc2", non-fragile) runtime ABI, driven by the metadata
// that clang emits for macOS x86_64. See docs/OBJECTIVE-C-RUNTIME.md for what was studied, why libobjc2 was not used as the base,
// and exactly what is and is not implemented.
//
// Guest-visible structures are the ones Apple's compiler writes into the Mach-O (class_t, class_ro_t, method_list_t, ivar_list_t,
// category_t, protocol_t ...). The runtime READS them (every pointer is validated before it is dereferenced), keeps its own state in
// host tables, and writes into guest memory only where the ABI says the runtime must: selector references, instance-variable
// offset variables, and the objects it allocates.
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "cider/abi.hpp"
#include "cider/linker.hpp"
#include "cider/memory.hpp"

namespace cider::libsystem {
struct RuntimeContext;
struct HostSymbol;
}

namespace cider::objc {

using Addr = uint64_t;   // a guest address (Class, SEL, id, IMP, Method, Ivar, Protocol are all just addresses)

struct Method {
    std::string name;
    Addr sel = 0;
    std::string types;
    Addr imp = 0;
    Addr handle = 0;            // the guest-visible `Method` pointer (24-byte record in the arena), created lazily
};

struct Ivar {
    std::string name, type;
    Addr offset_var = 0;        // address of the 32-bit offset variable the compiler emitted (0 for ivars added at run time)
    uint32_t offset = 0, size = 0;
    uint8_t align_log2 = 0;
    Addr handle = 0;
};

struct Class {
    Addr addr = 0;                         // address of the class_t in guest memory
    std::string name;
    Class* meta = nullptr;                 // class: its metaclass
    Class* instance_class = nullptr;       // metaclass: the class it describes
    Class* super = nullptr;
    Addr raw_super = 0, raw_isa = 0, ro = 0;
    bool is_meta = false, root = false, native = false, realized = false, registered = false, under_construction = false;
    bool has_load = false;
    Addr load_imp = 0;                     // the class's OWN +load (captured before categories are attached)
    uint32_t ro_flags = 0, instance_start = 0, instance_size = 0;
    std::vector<Method> methods;           // later entries override earlier ones (categories are appended)
    std::unordered_map<Addr, size_t> by_sel;
    std::vector<Ivar> ivars;
    std::vector<std::string> protocols;
    std::unordered_map<Addr, Addr> cache;  // sel -> imp (resolved through the superclass chain)
    int image = -1;                        // linker image index (-1: native / created at run time)
};

struct Protocol {
    std::string name;
    std::vector<std::string> parents;
    Addr handle = 0;
};

struct ObjectState {
    uint32_t rc = 1;                       // strong retain count
    bool deallocating = false;
    bool weak_referenced = false;
};

// One native (host-implemented) method of a built-in class. `fn` follows the Windows ABI when !raw: (self, _cmd, args...) with up to
// four integer/pointer arguments in rcx,rdx,r8,r9 after the System V remap; `raw` functions receive abi::GuestRegs*.
struct NativeMethod {
    std::string selector, types;
    void* fn = nullptr;
    bool raw = false;
};

// Raw thunk behind the "no method found" IMP: -forwardingTargetForSelector:, -forwardInvocation:, else the unrecognized-selector trap.
void objc_forward_message(abi::GuestRegs* regs);

class Runtime {
public:
    explicit Runtime(libsystem::RuntimeContext& ctx);
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    static Runtime* current();                    // the runtime of the active guest process (nullptr without Objective-C)

    // ---- images (linker integration) -------------------------------------------------------------------------------------------
    // Called per image before the final protections: uniquifies selector references and parses class/category/protocol metadata.
    bool prepare_image(linker::ImageRecord& image, std::string& error);
    // After every image was prepared (and protected): realizes classes, applies categories, slides instance-variable offsets.
    bool finish_images(std::string& error);
    // Sends +load to the classes/categories that define it, superclasses first. `caller` runs one guest function.
    void run_load_methods(const std::function<void(Addr imp, Addr self, Addr sel)>& caller);
    bool has_objc_images() const { return !images_.empty(); }
    bool foundation_requested = false;            // some image links against Foundation/CoreFoundation
    bool objc_requested = false;
    bool appkit_requested = false;                // some image links against AppKit
    // Data symbols of the framework subsets (___NSArray0__, _NSApp, _CGRectZero...): asked in registration order after the built-in names.
    using DynamicProvider = std::function<std::optional<uint64_t>(std::string_view)>;
    void add_dynamic_provider(DynamicProvider f) { providers_.push_back(std::move(f)); }

    // ---- selectors, classes, methods -----------------------------------------------------------------------------------------
    Addr sel(std::string_view name);              // sel_registerName
    std::string_view sel_name(Addr s) const;
    void flush_caches_public() { flush_caches(); }
    Addr protocol_handle(const std::string& name);                 // objc_getProtocol: 0 when no image declares it
    const std::string* protocol_name_of(Addr handle) const;
    bool conforms_to(Class* c, const std::string& protocol) const;
    Class* class_named(std::string_view name) const;
    Class* class_at(Addr cls) const;              // nullptr unless `cls` is a registered class_t (or metaclass)
    Class* class_of_object(Addr obj) const;       // reads the isa word
    Addr find_imp(Class* c, Addr sel);            // method lookup with caches; 0 = not implemented
    Method* find_method(Class* c, Addr sel);      // like find_imp but returns the record (instance or class method per `c`)
    bool is_kind_of(Class* c, Class* ancestor) const;
    Addr method_handle(Class* c, Method& m);
    Method* method_from_handle(Addr h);
    Ivar* ivar_from_handle(Addr h);
    Addr ivar_handle(Class* c, Ivar& v);

    // ---- run-time class construction (objc_allocateClassPair & co.) -----------------------------------------------------------------
    Class* allocate_class_pair(Class* super, const std::string& name, size_t extra_bytes);
    bool register_class_pair(Class* cls);
    bool dispose_class_pair(Class* cls);
    bool add_method(Class* c, Addr sel, const std::string& types, Addr imp, bool replace, Addr* old_imp = nullptr);

    // ---- native classes (Foundation subset) ---------------------------------------------------------------------------------------
    Class* declare_native_class(const std::string& name, const std::string& super, uint32_t instance_size);
    void add_native_methods(Class* cls, bool class_methods, const std::vector<NativeMethod>& methods);
    bool build_native_thunks(std::string& error);   // after all declarations: creates the host->guest thunks and fills the IMPs
    std::vector<std::string> native_class_names(const std::string& owner = {}) const;   // owner: framework that declared them ("Foundation"...)
    void set_native_owner(std::string owner) { native_owner_ = std::move(owner); }      // applies to the classes declared next
    std::pair<Addr, Addr> thunk_range() const;

    // ---- objects ------------------------------------------------------------------------------------------------------------------------
    Addr create_instance(Class* c, size_t extra);
    bool is_live_object(Addr obj) const { return live_.count(obj) != 0; }
    bool is_static_object(Addr obj) const;           // inside a loaded image or the runtime arena: immortal, never retained/released
    void destruct_instance(Addr obj);                // runs .cxx_destruct of every class in the chain (ARC ivar cleanup)
    ObjectState* object_state(Addr obj);
    void dispose_instance(Addr obj);                // frees memory and forgets the object (no dealloc message)
    Addr arena_alloc(size_t size, size_t align = 16);   // guest-visible runtime memory (never freed before exit)

    // ---- calling guest/native code from the runtime ----------------------------------------------------------------------------------
    // Sends a message the way compiled code would, from inside a host function. Returns rax of the method.
    uint64_t send(Addr self, std::string_view selector, std::initializer_list<uint64_t> args = {});
    uint64_t send(Addr self, Addr sel, std::initializer_list<uint64_t> args = {});
    // Calls a guest function (an IMP) with integer arguments. From a host function: a nested callback; at top level: a fresh call.
    uint64_t call_imp(Addr imp, std::initializer_list<uint64_t> args);
    void set_top_level_stack(const abi::GuestStack* stack, uint64_t initial_sp) { top_stack_ = stack; top_sp_ = initial_sp; }
    bool failed() const { return failed_; }

    // ---- autorelease pools / weak references / ARC helpers (objc_memory.cpp) ----------------------------------------------------------
    Addr retain(Addr obj);
    void release(Addr obj);
    Addr autorelease(Addr obj);
    Addr pool_push();
    void pool_pop(Addr token);
    void drain_all_pools();                          // at process exit
    bool store_weak(Addr* location, Addr obj);       // returns false if obj is deallocating
    Addr load_weak(Addr* location);                  // retained+autoreleased by the caller's convention
    void clear_weak_refs_to(Addr obj);
    void remove_weak_location(Addr* location);

    // ---- diagnostics ---------------------------------------------------------------------------------------------------------------------------
    std::optional<uint64_t> dynamic_symbol(std::string_view name);   // _OBJC_CLASS_$_X, __objc_empty_cache, ___CFConstantStringClassReference
    size_t class_count() const { return classes_.size(); }
    size_t live_object_count() const { return live_.size(); }
    uint64_t messages_sent = 0;
    uint64_t lookups = 0;
    libsystem::RuntimeContext& ctx() { return ctx_; }

    // host helpers the dispatch stubs call (objc_dispatch.cpp)
    Addr lookup_for_message(Addr self, Addr sel);
    Addr lookup_for_super(Addr super_struct, Addr sel, int mode);
    Addr forward_trap_imp() const { return forward_imp_; }
    Addr nil_return_imp() const { return nil_imp_; }

    // generic checked reads of guest memory (VirtualQuery based)
    bool readable(Addr a, size_t n) const;
    bool read_cstring(Addr a, std::string& out, size_t max = 4096) const;

private:
    friend struct Parser;
    libsystem::RuntimeContext& ctx_;
    memory::Region arena_;
    size_t arena_used_ = 0;
    std::deque<std::string> sel_storage_;
    std::unordered_map<Addr, std::string_view> sel_names_;
    std::unordered_map<Addr, std::string> proto_handles_;
    std::unordered_map<std::string_view, Addr> sels_;
    std::vector<std::unique_ptr<Class>> classes_;
    std::unordered_map<Addr, Class*> by_addr_;
    std::unordered_map<std::string, Class*> by_name_;
    std::unordered_map<std::string, Protocol> protocols_;
    std::unordered_map<Addr, ObjectState> live_;
    struct PendingCategory {
        std::string name;
        Addr cls = 0;
        std::vector<Method> instance_methods, class_methods;
        std::vector<std::string> protocols;
        bool load = false;
        int image = -1;
    };
    std::vector<PendingCategory> categories_;
    std::vector<Class*> load_classes_;              // classes with +load (non-lazy classlist), in image order
    std::vector<std::pair<std::string, Addr>> load_categories_;
    std::vector<int> images_;
    std::vector<std::pair<Addr, Addr>> image_ranges_;     // mapped ranges of the images that contain Objective-C metadata
    // native thunks
    struct NativeEntry { Class* cls; bool class_method; size_t index; };
    std::vector<std::pair<NativeEntry, NativeMethod>> native_;
    std::vector<DynamicProvider> providers_;
    bool resolving_ = false;
    std::string native_owner_;
    std::unordered_map<std::string, std::string> native_owner_of_;
    abi::ThunkTable native_table_;
    Addr forward_imp_ = 0, nil_imp_ = 0, lookup_thunk_ = 0, lookup_super_thunk_ = 0;
    abi::ThunkTable core_table_;
    std::unordered_map<Addr, Method*> method_handles_;
    std::unordered_map<Addr, std::pair<Class*, size_t>> ivar_handles_;
    const abi::GuestStack* top_stack_ = nullptr;
    uint64_t top_sp_ = 0;
    bool failed_ = false;
    unsigned traced_ = 0;
    std::unordered_set<Addr> logged_sels_;
    // pools / weak
    struct WeakEntry { Addr* location; };
    std::unordered_map<Addr, std::vector<Addr*>> weak_by_object_;
    std::unordered_map<Addr*, Addr> weak_by_location_;
    friend struct Pools;

    Class* make_class(Addr cls_addr, bool meta);
    bool realize(Class* c, std::string& error);
    void rebuild_index(Class* c);
    void flush_caches();
    bool write_u32(Addr a, uint32_t v);
    bool register_name(Class* c);
};

// Entry points of the libobjc API as host symbols (objc_api.cpp) and the Foundation subset (src/foundation).
void add_objc_symbols(std::vector<libsystem::HostSymbol>& out);
void add_objc_memory_symbols(std::vector<libsystem::HostSymbol>& out);   // objc_memory.cpp: retain/release/ARC helpers
void add_foundation_symbols(std::vector<libsystem::HostSymbol>& out);
void install_foundation(Runtime& rt);       // declares NSObject, NSString, ... (foundation/*.cpp)

// ---- layout constants of the Apple ABI (x86_64) -------------------------------------------------------------------------------------
inline constexpr uint32_t kRoMeta = 0x1, kRoRoot = 0x2, kRoHasCxxStructors = 0x4;
inline constexpr uint32_t kMethodListEntSizeMask = 0xFFFC, kMethodListRelativeFlag = 0x80000000;

}  // namespace cider::objc

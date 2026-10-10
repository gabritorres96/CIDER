// Reads the metadata clang emits for the Apple (objc2) runtime from a loaded Mach-O image and registers classes, categories and
// protocols. EVERY pointer is checked with Runtime::readable before it is dereferenced, every count is bounded, and anything the
// runtime does not implement (relative method lists, ...) is refused with a message instead of being misread.
#include <algorithm>
#include <cstring>
#include <format>

#include "cider/libsystem.hpp"
#include "cider/log.hpp"
#include "cider/objc.hpp"

namespace cider::objc {

struct Parser {
    Runtime& rt;
    linker::ImageRecord& img;
    std::string err;

    template <class T> bool rd(Addr a, T& out) {
        if (!rt.readable(a, sizeof(T))) return false;
        std::memcpy(&out, reinterpret_cast<const void*>(a), sizeof(T));
        return true;
    }
    bool fail(std::string m) { if (err.empty()) err = std::move(m); return false; }

    bool method_list(Addr list, std::vector<Method>& out, const char* what) {
        if (!list) return true;
        uint32_t entsize_flags = 0, count = 0;
        if (!rd(list, entsize_flags) || !rd(list + 4, count)) return fail(std::string(what) + ": method list is not readable");
        if (entsize_flags & kMethodListRelativeFlag) return fail(std::string(what) + ": relative method lists (macOS 11+ deployment targets) are not supported");
        if ((entsize_flags & kMethodListEntSizeMask) != 24) return fail(std::string(what) + ": unexpected method entry size");
        if (count > 100000 || !rt.readable(list + 8, static_cast<size_t>(count) * 24)) return fail(std::string(what) + ": method list is truncated or too large");
        for (uint32_t i = 0; i < count; ++i) {
            Addr name = 0, types = 0, imp = 0;
            rd(list + 8 + 24ull * i, name);
            rd(list + 16 + 24ull * i, types);
            rd(list + 24 + 24ull * i, imp);
            Method m;
            if (!rt.read_cstring(name, m.name, 1024) || m.name.empty()) return fail(std::string(what) + ": bad method name");
            if (types) rt.read_cstring(types, m.types, 1024);
            m.sel = rt.sel(m.name);
            m.imp = imp;
            out.push_back(std::move(m));
        }
        return true;
    }

    bool ivar_list(Addr list, std::vector<Ivar>& out, const char* what) {
        if (!list) return true;
        uint32_t entsize = 0, count = 0;
        if (!rd(list, entsize) || !rd(list + 4, count)) return fail(std::string(what) + ": ivar list is not readable");
        if ((entsize & ~3u) != 32) return fail(std::string(what) + ": unexpected ivar entry size");
        if (count > 10000 || !rt.readable(list + 8, static_cast<size_t>(count) * 32)) return fail(std::string(what) + ": ivar list is truncated or too large");
        for (uint32_t i = 0; i < count; ++i) {
            const Addr e = list + 8 + 32ull * i;
            Addr off = 0, name = 0, type = 0;
            uint32_t align = 0, size = 0;
            rd(e, off); rd(e + 8, name); rd(e + 16, type); rd(e + 24, align); rd(e + 28, size);
            Ivar v;
            if (!rt.read_cstring(name, v.name, 1024)) v.name.clear();       // anonymous ivars (bitfields) have no name
            if (type) rt.read_cstring(type, v.type, 1024);
            if (!off || !rt.readable(off, 4)) return fail(std::string(what) + ": ivar offset variable is not readable");
            rd(off, v.offset);
            v.offset_var = off;
            v.size = size;
            v.align_log2 = static_cast<uint8_t>(std::min<uint32_t>(align, 12));
            out.push_back(std::move(v));
        }
        return true;
    }

    void protocol_names(Addr list, std::vector<std::string>& out, int depth = 0) {
        if (!list || depth > 8) return;
        uint64_t count = 0;
        if (!rd(list, count) || count > 1000 || !rt.readable(list + 8, count * 8)) return;
        for (uint64_t i = 0; i < count; ++i) {
            Addr p = 0, name = 0, parents = 0;
            rd(list + 8 + 8 * i, p);
            if (!rd(p + 8, name) || !rd(p + 16, parents)) continue;
            std::string n;
            if (!rt.read_cstring(name, n, 512) || n.empty()) continue;
            out.push_back(n);
            if (!rt.protocols_.count(n)) {
                Protocol pr;
                pr.name = n;
                protocol_names(parents, pr.parents, depth + 1);
                rt.protocols_[n] = std::move(pr);
            }
        }
    }

    // class_ro_t: flags@0 instanceStart@4 instanceSize@8 name@24 baseMethods@32 baseProtocols@40 ivars@48
    bool read_ro(Addr ro, Class& c, bool want_ivars, const char* what) {
        uint32_t flags = 0, start = 0, size = 0;
        Addr name = 0, methods = 0, protos = 0, ivars = 0;
        if (!rt.readable(ro, 72)) return fail(std::string(what) + ": class_ro_t is not readable");
        rd(ro, flags); rd(ro + 4, start); rd(ro + 8, size); rd(ro + 24, name); rd(ro + 32, methods); rd(ro + 40, protos); rd(ro + 48, ivars);
        c.ro = ro;
        c.ro_flags = flags;
        c.instance_start = start;
        c.instance_size = size;
        if (!rt.read_cstring(name, c.name, 512) || c.name.empty()) return fail(std::string(what) + ": class name is not readable");
        if (size > (256u << 20) || start > size) return fail(c.name + ": implausible instance size");
        if (!method_list(methods, c.methods, c.name.c_str())) return false;
        if (want_ivars && !ivar_list(ivars, c.ivars, c.name.c_str())) return false;
        protocol_names(protos, c.protocols);
        return true;
    }

    bool parse_class(Addr cls) {
        if (rt.by_addr_.count(cls)) return true;
        Addr isa = 0, super = 0, data = 0;
        if (!rt.readable(cls, 40)) return fail(std::format("class pointer {:#x} is not readable", cls));
        rd(cls, isa); rd(cls + 8, super); rd(cls + 32, data);
        const Addr ro = data & ~Addr{7};
        Class* c = rt.make_class(cls, false);
        if (!read_ro(ro, *c, true, "class")) return false;
        if (c->ro_flags & kRoMeta) return fail(c->name + ": a metaclass is listed as a class");
        c->raw_isa = isa;
        c->raw_super = super;
        c->root = (c->ro_flags & kRoRoot) != 0;
        c->image = static_cast<int>(img.index);
        // metaclass
        Addr mdata = 0;
        if (!rt.readable(isa, 40)) return fail(c->name + ": metaclass pointer is not readable");
        rd(isa + 32, mdata);
        Class* m = rt.make_class(isa, true);
        if (!read_ro(mdata & ~Addr{7}, *m, false, "metaclass")) return false;
        if (!(m->ro_flags & kRoMeta)) return fail(c->name + ": isa does not point to a metaclass");
        m->instance_class = c;
        m->image = c->image;
        c->meta = m;
        return true;
    }

    bool parse_category(Addr cat) {
        // category_t: name@0 cls@8 instanceMethods@16 classMethods@24 protocols@32 instanceProperties@40
        Addr name = 0, cls = 0, im = 0, cm = 0, protos = 0;
        if (!rt.readable(cat, 48)) return fail("category pointer is not readable");
        rd(cat, name); rd(cat + 8, cls); rd(cat + 16, im); rd(cat + 24, cm); rd(cat + 32, protos);
        Runtime::PendingCategory pc;
        if (!rt.read_cstring(name, pc.name, 512)) return fail("category name is not readable");
        pc.cls = cls;
        pc.image = static_cast<int>(img.index);
        if (!method_list(im, pc.instance_methods, pc.name.c_str()) || !method_list(cm, pc.class_methods, pc.name.c_str())) return false;
        protocol_names(protos, pc.protocols);
        rt.categories_.push_back(std::move(pc));
        return true;
    }

    bool parse_protocol(Addr p) {
        Addr name = 0, parents = 0;
        if (!rd(p + 8, name) || !rd(p + 16, parents)) return fail("protocol pointer is not readable");
        Protocol pr;
        if (!rt.read_cstring(name, pr.name, 512) || pr.name.empty()) return fail("protocol name is not readable");
        protocol_names(parents, pr.parents);
        rt.protocols_[pr.name] = std::move(pr);
        return true;
    }

    template <class F> bool each_pointer(const macho::Section& sec, F&& f) {
        const Addr base = static_cast<Addr>(static_cast<int64_t>(sec.addr) + img.mapped.slide);
        if (sec.size % 8 != 0 || sec.size > (64u << 20)) return fail(std::format("{} has an invalid size", sec.name));
        if (!rt.readable(base, sec.size)) return fail(std::format("{} is not readable", sec.name));
        for (uint64_t i = 0; i < sec.size / 8; ++i) {
            Addr v = 0;
            rd(base + 8 * i, v);
            if (!f(base + 8 * i, v)) return false;
        }
        return true;
    }

    bool run() {
        for (const auto& seg : img.info.segments) {
            for (const auto& sec : seg.sections) {
                if (sec.name == "__objc_selrefs") {
                    // selector references are patched IN PLACE: the slot gets the runtime's unique SEL for the method-name string
                    if (!each_pointer(sec, [&](Addr slot, Addr str) {
                            std::string name;
                            if (!rt.read_cstring(str, name, 1024) || name.empty()) return fail("a selector reference does not point to a string");
                            const Addr s = rt.sel(name);
                            if (rt.logged_sels_.insert(s).second && rt.logged_sels_.size() <= 64) log::info(log::Category::Objc, "Selector {} resolved", name);
                            std::memcpy(reinterpret_cast<void*>(slot), &s, 8);
                            return true;
                        }))
                        return false;
                } else if (sec.name == "__objc_classlist" || sec.name == "__objc_nlclslist") {
                    const bool nl = sec.name == "__objc_nlclslist";
                    if (!each_pointer(sec, [&](Addr, Addr cls) {
                            if (!parse_class(cls)) return false;
                            if (nl) rt.load_classes_.push_back(rt.by_addr_[cls]);
                            return true;
                        }))
                        return false;
                } else if (sec.name == "__objc_catlist" || sec.name == "__objc_nlcatlist") {
                    const bool nl = sec.name == "__objc_nlcatlist";
                    const size_t before = rt.categories_.size();
                    if (!each_pointer(sec, [&](Addr, Addr cat) { return parse_category(cat); })) return false;
                    if (nl)
                        for (size_t i = before; i < rt.categories_.size(); ++i) rt.categories_[i].load = true;
                } else if (sec.name == "__objc_protolist") {
                    if (!each_pointer(sec, [&](Addr, Addr p) { return parse_protocol(p); })) return false;
                } else if (sec.name == "__objc_imageinfo") {
                    if (sec.size != 8) return fail("__objc_imageinfo has an unexpected size");
                    uint32_t version = 0, flags = 0;
                    const Addr base = static_cast<Addr>(static_cast<int64_t>(sec.addr) + img.mapped.slide);
                    rd(base, version); rd(base + 4, flags);
                    if (version != 0) return fail("unsupported __objc_imageinfo version");
                    // flags 0x40 = "image has class properties"; bit 0 (OBJC_IMAGE_IS_REPLACEMENT) and the Swift bits are not supported
                    if (flags & 0x1) return fail("replacement images (OBJC_IMAGE_IS_REPLACEMENT) are not supported");
                }
            }
        }
        return true;
    }
};

bool Runtime::prepare_image(linker::ImageRecord& image, std::string& error) {
    for (const auto& d : image.info.dylibs) {
        if (d.path == libsystem::kObjcInstallName) objc_requested = true;
        if (d.path == libsystem::kFoundationInstallName || d.path == libsystem::kCoreFoundationInstallName) foundation_requested = true;
    }
    Parser p{*this, image, {}};
    if (!p.run()) { error = "Objective-C metadata: " + p.err; return false; }
    if (images_.empty()) log::info(log::Category::Objc, "Runtime initialized");
    images_.push_back(static_cast<int>(image.index));
    const Addr lo = image.mapped.actual_base();
    image_ranges_.emplace_back(lo, lo + image.mapped.layout.span);
    return true;
}

bool Runtime::realize(Class* c, std::string& error) {
    if (c->realized) return true;
    if (c->under_construction) { error = c->name + ": circular superclass chain"; return false; }
    c->under_construction = true;
    Class* super = nullptr;
    if (c->raw_super) {
        super = class_at(c->raw_super);
        if (!super || super->is_meta) { error = std::format("{}: superclass at {:#x} is not a registered class", c->name, c->raw_super); return false; }
        if (!realize(super, error)) return false;
    } else if (!c->root) {
        error = c->name + ": class without superclass that is not marked as a root class";
        return false;
    }
    c->super = super;
    c->meta->super = super ? super->meta : c;                      // root metaclass: its superclass is the root class (class methods fall back to instance methods)
    // non-fragile instance layout: the superclass may be bigger or smaller than the compiler assumed
    if (super) {
        const int64_t delta = static_cast<int64_t>(super->instance_size) - static_cast<int64_t>(c->instance_start);
        if (delta != 0) {
            for (auto& v : c->ivars) {
                const int64_t n = static_cast<int64_t>(v.offset) + delta;
                if (n < 0 || n > 0x7fffffff || !write_u32(v.offset_var, static_cast<uint32_t>(n))) { error = c->name + ": cannot slide instance variable " + v.name; return false; }
                v.offset = static_cast<uint32_t>(n);
            }
            c->instance_start = static_cast<uint32_t>(static_cast<int64_t>(c->instance_start) + delta);
            c->instance_size = static_cast<uint32_t>(static_cast<int64_t>(c->instance_size) + delta);
        }
    }
    c->under_construction = false;
    c->realized = c->meta->realized = true;
    if (by_name_.count(c->name)) { error = "duplicate class " + c->name; return false; }
    c->registered = c->meta->registered = true;
    log::info(log::Category::Objc, "Class {} registered", c->name);
    by_name_[c->name] = c;
    rebuild_index(c);
    rebuild_index(c->meta);
    if (auto it = c->meta->by_sel.find(sel("load")); it != c->meta->by_sel.end()) c->load_imp = c->meta->methods[it->second].imp;   // before categories add theirs
    return true;
}

bool Runtime::finish_images(std::string& error) {
    std::vector<Class*> from_images;
    for (auto& c : classes_)
        if (!c->is_meta && !c->realized && c->image >= 0) from_images.push_back(c.get());
    for (Class* c : from_images)
        if (!realize(c, error)) return false;
    for (auto& pc : categories_) {
        Class* target = class_at(pc.cls);
        if (!target || target->is_meta) { log::warn(log::Category::Execution, "category {} extends a class that is not registered: ignored", pc.name); continue; }
        target->methods.insert(target->methods.end(), pc.instance_methods.begin(), pc.instance_methods.end());
        target->meta->methods.insert(target->meta->methods.end(), pc.class_methods.begin(), pc.class_methods.end());
        target->protocols.insert(target->protocols.end(), pc.protocols.begin(), pc.protocols.end());
        rebuild_index(target);
        rebuild_index(target->meta);
        log::debug(log::Category::Execution, "category {} attached to {}", pc.name, target->name);
    }
    flush_caches();
    // +load: collected from the non-lazy lists, superclasses first
    std::vector<Class*> ordered;
    std::function<void(Class*)> schedule = [&](Class* c) {
        if (!c || std::find(ordered.begin(), ordered.end(), c) != ordered.end()) return;
        if (c->super) schedule(c->super);
        if (c->load_imp && std::find(load_classes_.begin(), load_classes_.end(), c) != load_classes_.end()) ordered.push_back(c);
    };
    for (Class* c : load_classes_) schedule(c);
    load_classes_ = ordered;
    return true;
}

void Runtime::run_load_methods(const std::function<void(Addr imp, Addr self, Addr sel)>& caller) {
    const Addr load = sel("load");
    for (Class* c : load_classes_) {
        if (!c->load_imp) continue;
        // only the +load defined by the class itself (not an inherited or category one) belongs here; categories are run below
        log::info(log::Category::Execution, "+[{} load]", c->name);
        caller(c->load_imp, c->addr, load);
    }
    for (auto& pc : categories_) {
        if (!pc.load) continue;
        Class* target = class_at(pc.cls);
        if (!target) continue;
        for (const auto& m : pc.class_methods)
            if (m.sel == load) {
                log::info(log::Category::Execution, "+[{} load] (category {})", target->name, pc.name);
                caller(m.imp, target->addr, load);
            }
    }
    load_classes_.clear();
}

}  // namespace cider::objc

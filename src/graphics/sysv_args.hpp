#pragma once
// Walks the arguments of a System V call that a raw host function received (abi::GuestRegs), including the geometry structures:
//   * CGPoint / CGSize (two doubles, class SSE): two consecutive xmm registers;
//   * CGRect (four doubles, 32 bytes, class MEMORY): copied onto the stack in argument order;
//   * results: a CGRect comes back through a hidden pointer in rdi (also returned in rax); a CGPoint / CGSize in xmm0:xmm1.
// Integer and double arguments consume their own register classes independently, exactly like the psABI.
#include <cstdint>
#include <cstring>

#include "cider/abi.hpp"
#include "cider/geometry.hpp"

namespace cider::graphics::sysv {

inline double as_double(uint64_t bits) { double d; std::memcpy(&d, &bits, 8); return d; }
inline uint64_t as_bits(double d) { uint64_t b; std::memcpy(&b, &d, 8); return b; }

class Args {
public:
    explicit Args(abi::GuestRegs* r) : r_(r) {}
    uint64_t next_int() { return gp_ < 6 ? r_->gp[gp_++] : stack_slot(); }
    double next_double() { return as_double(xmm_ < 8 ? r_->xmm[xmm_++] : stack_slot()); }
    geometry::Point next_point() { geometry::Point p; p.x = next_double(); p.y = next_double(); return p; }
    geometry::Size next_size() { geometry::Size s; s.width = next_double(); s.height = next_double(); return s; }
    geometry::Rect next_rect() {
        geometry::Rect r;
        r.origin.x = as_double(stack_slot());
        r.origin.y = as_double(stack_slot());
        r.size.width = as_double(stack_slot());
        r.size.height = as_double(stack_slot());
        return r;
    }

private:
    uint64_t stack_slot() {
        const auto* base = reinterpret_cast<const uint64_t*>(r_->stack_args);
        return base[stack_++];
    }
    abi::GuestRegs* r_;
    unsigned gp_ = 0, xmm_ = 0;
    uint64_t stack_ = 0;
};

inline void ret_rect(abi::GuestRegs* r, uint64_t out_ptr, const geometry::Rect& v) {
    double* d = reinterpret_cast<double*>(out_ptr);
    d[0] = v.origin.x; d[1] = v.origin.y; d[2] = v.size.width; d[3] = v.size.height;
    r->ret_rax = out_ptr;
}
inline void ret_point(abi::GuestRegs* r, const geometry::Point& p) { r->ret_xmm0 = as_bits(p.x); r->ret_xmm1 = as_bits(p.y); }
inline void ret_size(abi::GuestRegs* r, const geometry::Size& s) { r->ret_xmm0 = as_bits(s.width); r->ret_xmm1 = as_bits(s.height); }
inline void ret_double(abi::GuestRegs* r, double d) { r->ret_xmm0 = as_bits(d); }

}  // namespace cider::graphics::sysv

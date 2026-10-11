// libFuzzer: the CoreGraphics engine (ORVYN 0.6.1). Two inputs from one byte stream:
//   * the first byte selects image decoding: the rest is handed to WIC with tight limits (must reject or decode, never crash, never allocate beyond the limits);
//   * otherwise the bytes are a program of context operations (paths, arcs, transforms with NaN/inf/huge values, clips, save/restore) run against a checking
//     device that verifies clips stay strictly nested and balanced when the context dies.
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>

#include "../../src/graphics/d2d_device.hpp"
#include "orvyn/cg.hpp"

namespace {

using namespace orvyn::cg;

struct CheckDevice : Device {
    int clips = 0;
    const char* name() const override { return "fuzz"; }
    void clear(const Color&) override {}
    void fill_path(const Path& p, const Affine&, const Color&, FillRule, bool) override { (void)p.bounding_box(); }
    void stroke_path(const Path& p, const Affine&, const Color&, const StrokeStyle& s, bool) override { (void)p.size(); if (s.dash.size() > 128) __builtin_trap(); }
    void push_clip(const Path&, const Affine&, FillRule) override { ++clips; }
    void pop_clip() override { if (--clips < 0) __builtin_trap(); }
    void draw_image(const Image&, const Rect&, const Affine&, double, bool) override {}
    void draw_text(const std::u16string&, const FontSpec&, Point, const Affine&, const Color&) override {}
    Size measure_text(const std::u16string&, const FontSpec&) override { return {}; }
    bool ok() const override { return true; }
};

struct Reader {
    const uint8_t* p;
    size_t n;
    uint8_t byte() { return n ? (--n, *p++) : 0; }
    double num() {                                       // small, huge, NaN and infinite values all show up
        const uint8_t k = byte();
        switch (k & 7) {
            case 0: return std::nan("");
            case 1: return INFINITY;
            case 2: return -INFINITY;
            case 3: return 1e300;
            default: return (static_cast<int>(byte()) - 128) * (k & 8 ? 10.0 : 0.25);
        }
    }
};

void run_ops(const uint8_t* data, size_t size) {
    CheckDevice dev;
    {
        Context c(&dev, view_base(100, false, 1.0), Size{100, 100});
        Reader r{data, size};
        for (int steps = 0; steps < 400 && r.n; ++steps) {
            switch (r.byte() % 18) {
                case 0: c.save(); break;
                case 1: c.restore(); break;
                case 2: c.translate(r.num(), r.num()); break;
                case 3: c.scale(r.num(), r.num()); break;
                case 4: c.rotate(r.num()); break;
                case 5: c.path().move_to({r.num(), r.num()}); break;
                case 6: c.path().line_to({r.num(), r.num()}); break;
                case 7: { const double a = r.num(), b = r.num(), cc = r.num(), d = r.num(), e = r.num(), f = r.num(); c.path().curve_to({a, b}, {cc, d}, {e, f}); break; }
                case 8: { const double x = r.num(), y = r.num(), rad = r.num(), s = r.num(), e = r.num(); Path p; p.arc({x, y}, rad, s, e, r.byte() & 1); c.path().add_path(p, c.state().ctm, true); break; }
                case 9: c.path().close(); break;
                case 10: c.fill_current((r.byte() & 1) ? FillRule::EvenOdd : FillRule::NonZero); break;
                case 11: c.stroke_current(); break;
                case 12: c.clip_current((r.byte() & 1) ? FillRule::EvenOdd : FillRule::NonZero); break;
                case 13: c.clip_to_rect(orvyn::geometry::make_rect(r.num(), r.num(), r.num(), r.num())); break;
                case 14: c.fill_rect(orvyn::geometry::make_rect(r.num(), r.num(), r.num(), r.num())); break;
                case 15: c.stroke_rect(orvyn::geometry::make_rect(r.num(), r.num(), r.num(), r.num())); break;
                case 16: { auto& l = c.mutable_state().line; l.width = r.num(); l.dash.assign(r.byte() % 5, std::fabs(r.num())); break; }
                default: { Path p; p.arc_to({r.num(), r.num()}, {r.num(), r.num()}, r.num()); (void)p.contains({r.num(), r.num()}, FillRule::NonZero); break; }
            }
        }
    }
    if (dev.clips != 0) __builtin_trap();                 // a context must pop every clip it pushed, whatever the program did
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0) return 0;
    if (data[0] & 1) {
        Image img;
        std::string format, error;
        DecodeLimits lim;
        lim.max_dimension = 2048;
        lim.max_pixels = 1u << 20;
        lim.max_input = 1u << 20;
        if (decode_image(data + 1, size - 1, lim, img, format, error) && (!img.valid() || img.width > 2048 || img.height > 2048)) __builtin_trap();
    } else {
        run_ops(data + 1, size - 1);
    }
    return 0;
}

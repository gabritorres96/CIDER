#pragma once
// Direct2D implementation of cg::Device (ORVYN 0.6.1) plus the WIC/DirectWrite helpers the image and text code needs.
//
// Why Direct2D: GDI has no antialiasing or transforms for vector drawing; GDI+ has both but is legacy (no DPI-aware targets, software only, no geometry masks);
// Direct2D gives antialiased paths, arbitrary affine transforms, geometry-masked clipping, per-monitor DPI and a software WIC bitmap target that works
// without a desktop (the headless tests rasterize with it). Dependencies are only Windows system DLLs (d2d1, dwrite, windowscodecs).
// Every COM object created here is counted (com_objects_alive) so tests can prove nothing leaks.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "orvyn/cg.hpp"

struct HWND__;

namespace orvyn::cg {

class D2DDevice : public Device {
public:
    // A render target for a window client area (size in pixels). nullptr when Direct2D is unavailable.
    static std::unique_ptr<D2DDevice> create_for_window(HWND__* hwnd, uint32_t width, uint32_t height);
    // A software render target over a WIC bitmap (BGRA, premultiplied): headless drawing, bitmap contexts, cacheDisplay.
    static std::unique_ptr<D2DDevice> create_bitmap(uint32_t width, uint32_t height);

    ~D2DDevice() override;
    const char* name() const override { return window_ ? "direct2d-window" : "direct2d-bitmap"; }

    bool begin();                                                // BeginDraw
    bool end();                                                  // EndDraw; false (and ok() == false) when the target must be recreated
    bool resize(uint32_t width, uint32_t height);                // window targets
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    bool read_pixels(std::vector<uint8_t>& bgra) const;          // bitmap targets: row 0 first, premultiplied BGRA
    bool write_pixels(const uint8_t* bgra, size_t stride);       // bitmap targets: replace the content (before begin())

    // Device
    void clear(const Color& c) override;
    void fill_path(const Path& p, const Affine& to_device, const Color& c, FillRule rule, bool antialias) override;
    void stroke_path(const Path& p, const Affine& to_device, const Color& c, const StrokeStyle& s, bool antialias) override;
    void push_clip(const Path& p, const Affine& to_device, FillRule rule) override;
    void pop_clip() override;
    void draw_image(const Image& img, const Rect& dst, const Affine& to_device, double alpha, bool interpolate) override;
    void draw_text(const std::u16string& text, const FontSpec& font, Point origin_user, const Affine& to_device, const Color& c) override;
    Size measure_text(const std::u16string& text, const FontSpec& font) override;
    bool ok() const override { return ok_; }

    struct Impl;

private:
    D2DDevice();
    std::unique_ptr<Impl> impl_;
    uint32_t width_ = 0, height_ = 0;
    bool window_ = false, ok_ = true;
};

// ---- WIC image decoding / encoding (ORVYN 0.6.3) ----------------------------------------------------------------------------------------------------------------
struct DecodeLimits {
    uint32_t max_dimension = 16384;                              // width and height
    uint64_t max_pixels = 64ull << 20;                           // 64 megapixels
    size_t max_input = 128u << 20;                               // bytes of encoded data
};
// Decodes PNG, JPEG, BMP, GIF, TIFF, ICO (whatever WIC has a codec for) from memory into premultiplied BGRA. `error` explains a failure; nothing is allocated
// before the dimensions were validated against `limits`.
bool decode_image(const uint8_t* data, size_t size, const DecodeLimits& limits, Image& out, std::string& format, std::string& error);
bool encode_png(const Image& img, std::vector<uint8_t>& out, std::string& error);

// Number of COM objects created by this module that are still alive (leak checks), and GDI objects of the current process.
int com_objects_alive();
void shutdown_d2d();                                             // releases the factories (process end / tests)

}  // namespace orvyn::cg

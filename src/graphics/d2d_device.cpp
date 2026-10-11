// Direct2D / DirectWrite / WIC implementation of cg::Device. See d2d_device.hpp for the rationale.
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

#include "orvyn/log.hpp"
#include "d2d_device.hpp"

namespace orvyn::cg {
namespace {

std::atomic<int> g_live{0};

// Minimal counting smart pointer (only what is needed; every acquire/release is balanced and counted)
template <class T>
class Com {
public:
    Com() = default;
    explicit Com(T* p) : p_(p) { if (p_) ++g_live; }                // takes ownership of an already AddRef'ed pointer
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    Com(Com&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
    Com& operator=(Com&& o) noexcept { if (this != &o) { reset(); p_ = o.p_; o.p_ = nullptr; } return *this; }
    ~Com() { reset(); }
    void reset() { if (p_) { p_->Release(); p_ = nullptr; --g_live; } }
    T* get() const { return p_; }
    T* operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }
    T** put() { reset(); return &p_; }                              // for out-parameters: call adopt() afterwards
    void adopt() { if (p_) ++g_live; }
private:
    T* p_ = nullptr;
};
template <class T, class F>
bool create(Com<T>& out, F&& f) {                                  // f(T** ) -> HRESULT
    T* raw = nullptr;
    const HRESULT hr = f(&raw);
    if (FAILED(hr) || !raw) return false;
    out = Com<T>(raw);
    return true;
}

D2D1_COLOR_F to_d2d(const Color& c) { return D2D1::ColorF(static_cast<float>(c.r), static_cast<float>(c.g), static_cast<float>(c.b), static_cast<float>(c.a)); }
D2D1_MATRIX_3X2_F to_d2d(const Affine& t) {
    return D2D1::Matrix3x2F(static_cast<float>(t.a), static_cast<float>(t.b), static_cast<float>(t.c), static_cast<float>(t.d), static_cast<float>(t.tx), static_cast<float>(t.ty));
}
D2D1_POINT_2F pt(Point p) { return D2D1::Point2F(static_cast<float>(p.x), static_cast<float>(p.y)); }

struct Factories {
    Com<ID2D1Factory> d2d;
    Com<IDWriteFactory> dwrite;
    Com<IWICImagingFactory> wic;
    bool com_initialized = false;
    bool tried = false;
};
Factories g_f;

bool ensure_factories() {
    if (g_f.tried) return static_cast<bool>(g_f.d2d);
    g_f.tried = true;
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    g_f.com_initialized = SUCCEEDED(hr);
    create(g_f.d2d, [](ID2D1Factory** f) { return D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, f); });
    create(g_f.dwrite, [](IDWriteFactory** f) { return DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(f)); });
    create(g_f.wic, [](IWICImagingFactory** f) { return CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(f)); });
    return static_cast<bool>(g_f.d2d);
}

std::wstring widen16(const std::u16string& s) { return std::wstring(s.begin(), s.end()); }
std::wstring widen8(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

}  // namespace

int com_objects_alive() { return g_live.load(); }
void shutdown_d2d() {
    g_f.wic.reset();
    g_f.dwrite.reset();
    g_f.d2d.reset();
    if (g_f.com_initialized) { CoUninitialize(); g_f.com_initialized = false; }
    g_f.tried = false;
}

// ---- implementation -----------------------------------------------------------------------------------------------------------------------------------------
struct D2DDevice::Impl {
    Com<ID2D1RenderTarget> rt;
    Com<ID2D1HwndRenderTarget> hwnd_rt;
    Com<IWICBitmap> bitmap;                                       // bitmap targets
    struct ClipEntry { bool layer = false; Com<ID2D1Layer> layer_obj; };
    std::vector<ClipEntry> clips;
    bool drawing = false;
};

D2DDevice::D2DDevice() : impl_(std::make_unique<Impl>()) {}
D2DDevice::~D2DDevice() {
    // a target destroyed in the middle of a frame or with clips still pushed must be finished first, or Direct2D reports an error and keeps the resources
    if (impl_->rt && impl_->drawing) {
        while (!impl_->clips.empty()) pop_clip();
        impl_->rt->EndDraw();
        impl_->drawing = false;
    }
}

std::unique_ptr<D2DDevice> D2DDevice::create_for_window(HWND__* hwnd, uint32_t w, uint32_t h) {
    if (!ensure_factories() || !hwnd) return nullptr;
    std::unique_ptr<D2DDevice> d(new D2DDevice());
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
    D2D1_HWND_RENDER_TARGET_PROPERTIES hp = D2D1::HwndRenderTargetProperties(hwnd, D2D1::SizeU(std::max(1u, w), std::max(1u, h)), D2D1_PRESENT_OPTIONS_NONE);
    if (!create(d->impl_->hwnd_rt, [&](ID2D1HwndRenderTarget** o) { return g_f.d2d->CreateHwndRenderTarget(props, hp, o); })) return nullptr;
    ID2D1RenderTarget* base = nullptr;
    d->impl_->hwnd_rt->QueryInterface(IID_PPV_ARGS(&base));
    d->impl_->rt = Com<ID2D1RenderTarget>(base);
    d->width_ = w;
    d->height_ = h;
    d->window_ = true;
    return d;
}

std::unique_ptr<D2DDevice> D2DDevice::create_bitmap(uint32_t w, uint32_t h) {
    if (!ensure_factories() || !g_f.wic || w == 0 || h == 0) return nullptr;
    std::unique_ptr<D2DDevice> d(new D2DDevice());
    if (!create(d->impl_->bitmap, [&](IWICBitmap** o) { return g_f.wic->CreateBitmap(w, h, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, o); })) return nullptr;
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
    ID2D1RenderTarget* rt = nullptr;
    if (FAILED(g_f.d2d->CreateWicBitmapRenderTarget(d->impl_->bitmap.get(), props, &rt)) || !rt) return nullptr;
    d->impl_->rt = Com<ID2D1RenderTarget>(rt);
    d->width_ = w;
    d->height_ = h;
    return d;
}

bool D2DDevice::begin() {
    if (!impl_->rt || impl_->drawing) return false;
    impl_->rt->BeginDraw();
    impl_->drawing = true;
    ok_ = true;
    return true;
}
bool D2DDevice::end() {
    if (!impl_->rt || !impl_->drawing) return false;
    while (!impl_->clips.empty()) pop_clip();                     // never leave clips pushed across frames
    const HRESULT hr = impl_->rt->EndDraw();
    impl_->drawing = false;
    if (hr == D2DERR_RECREATE_TARGET) { ok_ = false; return false; }
    if (FAILED(hr)) { ok_ = false; return false; }
    return true;
}
bool D2DDevice::resize(uint32_t w, uint32_t h) {
    if (!window_ || !impl_->hwnd_rt) return false;
    width_ = std::max(1u, w);
    height_ = std::max(1u, h);
    return SUCCEEDED(impl_->hwnd_rt->Resize(D2D1::SizeU(width_, height_)));
}

bool D2DDevice::read_pixels(std::vector<uint8_t>& out) const {
    if (!impl_->bitmap) return false;
    Com<IWICBitmapLock> lock;
    WICRect rc{0, 0, static_cast<INT>(width_), static_cast<INT>(height_)};
    if (!create(lock, [&](IWICBitmapLock** o) { return impl_->bitmap->Lock(&rc, WICBitmapLockRead, o); })) return false;
    UINT stride = 0, size = 0;
    BYTE* data = nullptr;
    lock->GetStride(&stride);
    lock->GetDataPointer(&size, &data);
    if (!data) return false;
    out.resize(static_cast<size_t>(width_) * height_ * 4);
    for (uint32_t y = 0; y < height_; ++y) std::memcpy(out.data() + static_cast<size_t>(y) * width_ * 4, data + static_cast<size_t>(y) * stride, static_cast<size_t>(width_) * 4);
    return true;
}
bool D2DDevice::write_pixels(const uint8_t* bgra, size_t stride) {
    if (!impl_->bitmap || impl_->drawing) return false;
    Com<IWICBitmapLock> lock;
    WICRect rc{0, 0, static_cast<INT>(width_), static_cast<INT>(height_)};
    if (!create(lock, [&](IWICBitmapLock** o) { return impl_->bitmap->Lock(&rc, WICBitmapLockWrite, o); })) return false;
    UINT dstride = 0, size = 0;
    BYTE* data = nullptr;
    lock->GetStride(&dstride);
    lock->GetDataPointer(&size, &data);
    if (!data) return false;
    for (uint32_t y = 0; y < height_; ++y) std::memcpy(data + static_cast<size_t>(y) * dstride, bgra + y * stride, static_cast<size_t>(width_) * 4);
    return true;
}

namespace {
bool build_geometry(const Path& p, FillRule rule, Com<ID2D1PathGeometry>& out) {
    if (!create(out, [](ID2D1PathGeometry** o) { return g_f.d2d->CreatePathGeometry(o); })) return false;
    Com<ID2D1GeometrySink> sink;
    if (!create(sink, [&](ID2D1GeometrySink** o) { return out->Open(o); })) return false;
    sink->SetFillMode(rule == FillRule::EvenOdd ? D2D1_FILL_MODE_ALTERNATE : D2D1_FILL_MODE_WINDING);
    bool open = false;
    for (const auto& e : p.elements()) {
        switch (e.type) {
            case PathElement::Move:
                if (open) sink->EndFigure(D2D1_FIGURE_END_OPEN);
                sink->BeginFigure(pt(e.p[0]), D2D1_FIGURE_BEGIN_FILLED);
                open = true;
                break;
            case PathElement::Line:
                if (open) sink->AddLine(pt(e.p[0]));
                break;
            case PathElement::Cubic:
                if (open) sink->AddBezier(D2D1::BezierSegment(pt(e.p[0]), pt(e.p[1]), pt(e.p[2])));
                break;
            case PathElement::Close:
                if (open) { sink->EndFigure(D2D1_FIGURE_END_CLOSED); open = false; }
                break;
        }
    }
    if (open) sink->EndFigure(D2D1_FIGURE_END_OPEN);
    return SUCCEEDED(sink->Close());
}
}  // namespace

void D2DDevice::clear(const Color& c) {
    if (impl_->rt) impl_->rt->Clear(to_d2d(c));
}

void D2DDevice::fill_path(const Path& p, const Affine& t, const Color& c, FillRule rule, bool aa) {
    if (!impl_->rt || p.empty()) return;
    Com<ID2D1PathGeometry> g;
    if (!build_geometry(p, rule, g)) { ok_ = false; return; }
    Com<ID2D1SolidColorBrush> brush;
    if (!create(brush, [&](ID2D1SolidColorBrush** o) { return impl_->rt->CreateSolidColorBrush(to_d2d(c), o); })) { ok_ = false; return; }
    impl_->rt->SetAntialiasMode(aa ? D2D1_ANTIALIAS_MODE_PER_PRIMITIVE : D2D1_ANTIALIAS_MODE_ALIASED);
    impl_->rt->SetTransform(to_d2d(t));
    impl_->rt->FillGeometry(g.get(), brush.get());
    impl_->rt->SetTransform(D2D1::Matrix3x2F::Identity());
}

void D2DDevice::stroke_path(const Path& p, const Affine& t, const Color& c, const StrokeStyle& s, bool aa) {
    if (!impl_->rt || p.empty()) return;
    Com<ID2D1PathGeometry> g;
    if (!build_geometry(p, FillRule::NonZero, g)) { ok_ = false; return; }
    Com<ID2D1SolidColorBrush> brush;
    if (!create(brush, [&](ID2D1SolidColorBrush** o) { return impl_->rt->CreateSolidColorBrush(to_d2d(c), o); })) { ok_ = false; return; }
    D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties();
    sp.startCap = sp.endCap = s.cap == LineCap::Round ? D2D1_CAP_STYLE_ROUND : s.cap == LineCap::Square ? D2D1_CAP_STYLE_SQUARE : D2D1_CAP_STYLE_FLAT;
    sp.lineJoin = s.join == LineJoin::Round ? D2D1_LINE_JOIN_ROUND : s.join == LineJoin::Bevel ? D2D1_LINE_JOIN_BEVEL : D2D1_LINE_JOIN_MITER_OR_BEVEL;
    sp.miterLimit = static_cast<float>(std::max(1.0, s.miter_limit));
    const float width = static_cast<float>(std::max(0.0, s.width));
    Com<ID2D1StrokeStyle> style;
    std::vector<float> dashes;
    if (!s.dash.empty() && width > 0) {
        for (double d : s.dash) dashes.push_back(static_cast<float>(std::max(0.0, d) / width));      // D2D dash lengths are in units of the stroke width
        sp.dashStyle = D2D1_DASH_STYLE_CUSTOM;
        sp.dashOffset = static_cast<float>(s.dash_phase / width);
    }
    if (!create(style, [&](ID2D1StrokeStyle** o) { return g_f.d2d->CreateStrokeStyle(sp, dashes.empty() ? nullptr : dashes.data(), static_cast<UINT32>(dashes.size()), o); })) { ok_ = false; return; }
    impl_->rt->SetAntialiasMode(aa ? D2D1_ANTIALIAS_MODE_PER_PRIMITIVE : D2D1_ANTIALIAS_MODE_ALIASED);
    impl_->rt->SetTransform(to_d2d(t));
    impl_->rt->DrawGeometry(g.get(), brush.get(), width, style.get());
    impl_->rt->SetTransform(D2D1::Matrix3x2F::Identity());
}

void D2DDevice::push_clip(const Path& p, const Affine& t, FillRule rule) {
    if (!impl_->rt) return;
    Impl::ClipEntry e;
    Rect r;
    if (t.is_axis_aligned() && p.is_rect(r)) {
        const Rect d = transformed_bounds(r, t);
        impl_->rt->PushAxisAlignedClip(D2D1::RectF(static_cast<float>(d.min_x()), static_cast<float>(d.min_y()), static_cast<float>(d.max_x()), static_cast<float>(d.max_y())), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        impl_->clips.push_back(std::move(e));
        return;
    }
    Com<ID2D1PathGeometry> g;
    if (!build_geometry(p, rule, g) || !create(e.layer_obj, [&](ID2D1Layer** o) { return impl_->rt->CreateLayer(nullptr, o); })) {
        // cannot clip: clip everything (the safe failure for a clip) with an empty axis aligned rectangle
        impl_->rt->PushAxisAlignedClip(D2D1::RectF(0, 0, 0, 0), D2D1_ANTIALIAS_MODE_ALIASED);
        impl_->clips.push_back(Impl::ClipEntry{});
        ok_ = false;
        return;
    }
    D2D1_LAYER_PARAMETERS lp = D2D1::LayerParameters(D2D1::InfiniteRect(), g.get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, to_d2d(t));
    impl_->rt->PushLayer(lp, e.layer_obj.get());
    e.layer = true;
    impl_->clips.push_back(std::move(e));
}
void D2DDevice::pop_clip() {
    if (!impl_->rt || impl_->clips.empty()) return;
    const bool layer = impl_->clips.back().layer;
    if (layer) impl_->rt->PopLayer(); else impl_->rt->PopAxisAlignedClip();
    impl_->clips.pop_back();                                      // releases the layer object
}

void D2DDevice::draw_image(const Image& img, const Rect& dst, const Affine& t, double alpha, bool interpolate) {
    if (!impl_->rt || !img.valid()) return;
    Com<ID2D1Bitmap> bmp;
    const D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (!create(bmp, [&](ID2D1Bitmap** o) { return impl_->rt->CreateBitmap(D2D1::SizeU(img.width, img.height), img.bgra.data(), img.width * 4, bp, o); })) { ok_ = false; return; }
    const Rect s = geometry::standardize(dst);
    // bitmap pixel (px, py), row 0 at the top, to user space: x = dst.min_x + px * dst.w / w ; y = dst.max_y - py * dst.h / h  (CoreGraphics draws images upright in y-up user space)
    const Affine local{s.width() / img.width, 0, 0, -s.height() / img.height, s.min_x(), s.max_y()};
    impl_->rt->SetTransform(to_d2d(concat(local, t)));
    impl_->rt->DrawBitmap(bmp.get(), D2D1::RectF(0, 0, static_cast<float>(img.width), static_cast<float>(img.height)), static_cast<float>(alpha),
                          interpolate ? D2D1_BITMAP_INTERPOLATION_MODE_LINEAR : D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
    impl_->rt->SetTransform(D2D1::Matrix3x2F::Identity());
}

namespace {
bool make_layout(const std::u16string& text, const FontSpec& font, Com<IDWriteTextLayout>& layout) {
    if (!g_f.dwrite) return false;
    Com<IDWriteTextFormat> fmt;
    const std::wstring family = widen8(font.family);
    if (!create(fmt, [&](IDWriteTextFormat** o) {
            return g_f.dwrite->CreateTextFormat(family.c_str(), nullptr, font.bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                                                font.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, static_cast<float>(std::max(1.0, font.size)), L"en-us", o);
        })) return false;
    const std::wstring w = widen16(text);
    return create(layout, [&](IDWriteTextLayout** o) { return g_f.dwrite->CreateTextLayout(w.c_str(), static_cast<UINT32>(w.size()), fmt.get(), 100000.f, 100000.f, o); });
}
}  // namespace

void D2DDevice::draw_text(const std::u16string& text, const FontSpec& font, Point origin, const Affine& t, const Color& c) {
    if (!impl_->rt || text.empty()) return;
    Com<IDWriteTextLayout> layout;
    if (!make_layout(text, font, layout)) { ok_ = false; return; }
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    Com<ID2D1SolidColorBrush> brush;
    if (!create(brush, [&](ID2D1SolidColorBrush** o) { return impl_->rt->CreateSolidColorBrush(to_d2d(c), o); })) { ok_ = false; return; }
    // layout space: origin top-left, y down. The text box's bottom-left corner is placed at `origin`; in a y-up user space (negative determinant of the device matrix) the layout
    // must be mirrored so the glyphs are upright, in a flipped (y-down) space the origin is the top-left corner and nothing is mirrored.
    const bool y_up_user = t.determinant() < 0;
    const Affine local = y_up_user ? Affine{1, 0, 0, -1, origin.x, origin.y + m.height} : Affine{1, 0, 0, 1, origin.x, origin.y};
    impl_->rt->SetTransform(to_d2d(concat(local, t)));
    impl_->rt->DrawTextLayout(D2D1::Point2F(0, 0), layout.get(), brush.get());
    impl_->rt->SetTransform(D2D1::Matrix3x2F::Identity());
}
Size D2DDevice::measure_text(const std::u16string& text, const FontSpec& font) {
    if (!ensure_factories()) return {};
    Com<IDWriteTextLayout> layout;
    if (text.empty()) { std::u16string sp = u" "; if (!make_layout(sp, font, layout)) return {}; DWRITE_TEXT_METRICS m{}; layout->GetMetrics(&m); return {0, m.height}; }
    if (!make_layout(text, font, layout)) return {};
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    return {m.widthIncludingTrailingWhitespace, m.height};
}

// ---- WIC decode / encode --------------------------------------------------------------------------------------------------------------------------------------
bool decode_image(const uint8_t* data, size_t size, const DecodeLimits& lim, Image& out, std::string& format, std::string& error) {
    out = Image{};
    if (!data || size == 0) { error = "empty image data"; return false; }
    if (size > lim.max_input) { error = "encoded image is larger than the limit"; return false; }
    if (!ensure_factories() || !g_f.wic) { error = "Windows Imaging Component is unavailable"; return false; }
    Com<IWICStream> stream;
    if (!create(stream, [](IWICStream** o) { return g_f.wic->CreateStream(o); }) || FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(data), static_cast<DWORD>(size)))) { error = "cannot open the data"; return false; }
    Com<IWICBitmapDecoder> decoder;
    if (!create(decoder, [&](IWICBitmapDecoder** o) { return g_f.wic->CreateDecoderFromStream(stream.get(), nullptr, WICDecodeMetadataCacheOnDemand, o); })) { error = "unrecognized or corrupt image format"; return false; }
    GUID container{};
    decoder->GetContainerFormat(&container);
    format = container == GUID_ContainerFormatPng ? "png" : container == GUID_ContainerFormatJpeg ? "jpeg" : container == GUID_ContainerFormatBmp ? "bmp" :
             container == GUID_ContainerFormatGif ? "gif" : container == GUID_ContainerFormatTiff ? "tiff" : container == GUID_ContainerFormatIco ? "ico" : "other";
    Com<IWICBitmapFrameDecode> frame;
    if (!create(frame, [&](IWICBitmapFrameDecode** o) { return decoder->GetFrame(0, o); })) { error = "the image has no frame"; return false; }
    UINT w = 0, h = 0;
    if (FAILED(frame->GetSize(&w, &h)) || w == 0 || h == 0) { error = "invalid image dimensions"; return false; }
    if (w > lim.max_dimension || h > lim.max_dimension || static_cast<uint64_t>(w) * h > lim.max_pixels) { error = "image dimensions exceed the limits"; return false; }
    Com<IWICFormatConverter> conv;
    if (!create(conv, [](IWICFormatConverter** o) { return g_f.wic->CreateFormatConverter(o); }) ||
        FAILED(conv->Initialize(frame.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) { error = "cannot convert the pixel format"; return false; }
    out.width = w;
    out.height = h;
    out.bgra.assign(static_cast<size_t>(w) * h * 4, 0);
    if (FAILED(conv->CopyPixels(nullptr, w * 4, static_cast<UINT>(out.bgra.size()), out.bgra.data()))) { out = Image{}; error = "the pixel data is corrupt"; return false; }
    return true;
}

bool encode_png(const Image& img, std::vector<uint8_t>& out, std::string& error) {
    if (!img.valid()) { error = "invalid image"; return false; }
    if (!ensure_factories() || !g_f.wic) { error = "WIC unavailable"; return false; }
    Com<IWICBitmap> bmp;
    if (!create(bmp, [&](IWICBitmap** o) { return g_f.wic->CreateBitmapFromMemory(img.width, img.height, GUID_WICPixelFormat32bppPBGRA, img.width * 4, static_cast<UINT>(img.bgra.size()), const_cast<BYTE*>(img.bgra.data()), o); })) { error = "cannot wrap the pixels"; return false; }
    IStream* mem = nullptr;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &mem))) { error = "no memory"; return false; }
    Com<IStream> stream(mem);
    Com<IWICBitmapEncoder> enc;
    if (!create(enc, [&](IWICBitmapEncoder** o) { return g_f.wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, o); }) || FAILED(enc->Initialize(stream.get(), WICBitmapEncoderNoCache))) { error = "cannot create the PNG encoder"; return false; }
    Com<IWICBitmapFrameEncode> fe;
    IPropertyBag2* props = nullptr;
    IWICBitmapFrameEncode* feraw = nullptr;
    if (FAILED(enc->CreateNewFrame(&feraw, &props)) || !feraw) { error = "cannot create a frame"; return false; }
    fe = Com<IWICBitmapFrameEncode>(feraw);
    if (props) props->Release();
    WICPixelFormatGUID pf = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(fe->Initialize(nullptr)) || FAILED(fe->SetSize(img.width, img.height)) || FAILED(fe->SetPixelFormat(&pf))) { error = "cannot configure the frame"; return false; }
    // PNG stores straight alpha: convert from premultiplied
    std::vector<uint8_t> straight = img.bgra;
    for (size_t i = 0; i + 3 < straight.size(); i += 4) {
        const uint8_t a = straight[i + 3];
        if (a != 0 && a != 255) for (int k = 0; k < 3; ++k) straight[i + k] = static_cast<uint8_t>(std::min(255, (straight[i + k] * 255 + a / 2) / a));
    }
    if (FAILED(fe->WritePixels(img.height, img.width * 4, static_cast<UINT>(straight.size()), straight.data())) || FAILED(fe->Commit()) || FAILED(enc->Commit())) { error = "cannot write the pixels"; return false; }
    HGLOBAL hg = nullptr;
    GetHGlobalFromStream(stream.get(), &hg);
    const SIZE_T n = GlobalSize(hg);
    void* p = GlobalLock(hg);
    if (!p) { error = "no memory"; return false; }
    out.assign(static_cast<uint8_t*>(p), static_cast<uint8_t*>(p) + n);
    GlobalUnlock(hg);
    return true;
}

}  // namespace orvyn::cg

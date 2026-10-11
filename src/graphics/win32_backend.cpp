// Win32 GUI backend (CIDER 0.5.8): real HWNDs through RegisterClassExW / CreateWindowExW and the standard BUTTON / STATIC / EDIT controls.
// Everything runs on the main guest thread, which is the OS thread that owns every window and pumps their messages. See docs/GUI-BACKEND.md.
#include <algorithm>
#include <unordered_map>
#include <unordered_set>

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>

#include "cider/log.hpp"
#include "d2d_device.hpp"
#include "gui_backend.hpp"

namespace cider::gui {
namespace {

using geometry::PixelRect;
using geometry::Point;
using geometry::Rect;
using geometry::Size;

#ifndef EM_SETCUEBANNER
#define EM_SETCUEBANNER 0x1501      // ECM_FIRST + 1 (commctrl.h)
#endif

const wchar_t* kWindowClass = L"CIDERWindow";
const wchar_t* kViewClass = L"CIDERView";

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

struct Node {
    ChildKind kind = ChildKind::Container;
    bool is_window = false;
    unsigned style_mask = 0;
    Size min_content{0, 0};
    bool destroying = false;
    bool custom_paint = false;                                 // painted by Events::paint on a Direct2D surface
    bool painting = false;
    std::unique_ptr<cg::D2DDevice> surface;
};

class Win32Backend final : public Backend {
public:
    Win32Backend();
    ~Win32Backend() override;
    const char* name() const override { return "win32"; }

    NativeId create_window(const WindowSpec& spec) override;
    void set_window_title(NativeId w, const std::string& t) override { SetWindowTextW(hwnd(w), widen(t).c_str()); }
    Rect content_rect(NativeId w) override;
    Rect frame_rect(NativeId w) override;
    void set_content_rect(NativeId w, const Rect& r) override;
    Size client_size(NativeId w) override;
    void set_min_content_size(NativeId w, Size s) override { if (auto* n = node(w)) n->min_content = s; }
    void center(NativeId w) override;
    void show(NativeId w, bool activate) override;
    void hide(NativeId w) override { ShowWindow(hwnd(w), SW_HIDE); }
    bool is_visible(NativeId w) override { return IsWindowVisible(hwnd(w)) != 0; }
    bool is_active(NativeId w) override { return GetForegroundWindow() == hwnd(w); }
    void activate(NativeId w) override { SetForegroundWindow(hwnd(w)); }

    NativeId create_child(NativeId parent, ChildKind kind, const Rect& rect, const std::string& text) override;
    void set_rect(NativeId c, const Rect& rect) override;
    void set_text(NativeId n, const std::string& t) override { SetWindowTextW(hwnd(n), widen(t).c_str()); }
    std::string text(NativeId n) override;
    void set_enabled(NativeId n, bool e) override { EnableWindow(hwnd(n), e ? TRUE : FALSE); }
    void set_hidden(NativeId n, bool h) override { ShowWindow(hwnd(n), h ? SW_HIDE : SW_SHOWNA); }
    void set_checked(NativeId n, int state) override { SendMessageW(hwnd(n), BM_SETCHECK, state ? BST_CHECKED : BST_UNCHECKED, 0); }
    int checked(NativeId n) override { return SendMessageW(hwnd(n), BM_GETCHECK, 0, 0) == BST_CHECKED ? 1 : 0; }
    void set_read_only(NativeId n, bool ro) override { SendMessageW(hwnd(n), EM_SETREADONLY, ro ? TRUE : FALSE, 0); }
    void set_placeholder(NativeId n, const std::string& t) override { SendMessageW(hwnd(n), EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(widen(t).c_str())); }
    void focus(NativeId n) override { SetFocus(hwnd(n)); }
    void destroy(NativeId n) override;

    Rect screen_frame() override;
    Rect visible_frame() override;
    double scale_factor() override { return geometry::scale_for_dpi(GetDpiForSystem()); }
    void set_custom_paint(NativeId n, bool on) override;
    void invalidate(NativeId n) override { if (auto* nd = node(n)) if (nd->custom_paint) InvalidateRect(hwnd(n), nullptr, FALSE); }
    void display_now(NativeId n) override { if (auto* nd = node(n)) if (nd->custom_paint) { InvalidateRect(hwnd(n), nullptr, FALSE); UpdateWindow(hwnd(n)); } }
    cg::Device* paint_device() override { return paint_device_; }
    double paint_scale() override { return paint_scale_; }
    int pump() override;
    void wait(void* wake_event, int64_t ms) override;
    size_t native_count() const override { return nodes_.size(); }

    // window procedures (static trampolines call these)
    LRESULT window_proc(HWND h, UINT m, WPARAM w, LPARAM l);
    LRESULT view_proc(HWND h, UINT m, WPARAM w, LPARAM l);

private:
    static HWND hwnd(NativeId id) { return reinterpret_cast<HWND>(static_cast<uintptr_t>(id)); }
    static NativeId id_of(HWND h) { return static_cast<NativeId>(reinterpret_cast<uintptr_t>(h)); }
    Node* node(NativeId id) {
        auto it = nodes_.find(id);
        return it == nodes_.end() ? nullptr : &it->second;
    }
    HWND top_level_of(HWND h) const { return GetAncestor(h, GA_ROOT); }
    double scale_of(HWND h) const { return geometry::scale_for_dpi(GetDpiForWindow(h)); }
    long screen_height_px() const { return GetSystemMetrics(SM_CYSCREEN); }
    HFONT font_for(unsigned dpi);
    void erase_tree(HWND h);                                   // forget a native object and everything inside it
    void apply_font_tree(HWND top);
    LRESULT common(HWND h, UINT m, WPARAM w, LPARAM l, bool& handled);
    void forward_mouse(HWND h, UINT m, WPARAM w, LPARAM l);
    DWORD style_for(unsigned mask) const;

    std::unordered_map<NativeId, Node> nodes_;
    std::unordered_map<unsigned, HFONT> fonts_;
    uint64_t next_control_id_ = 1000;
    HINSTANCE instance_ = nullptr;
    std::u16string pending_surrogate_;
    cg::Device* paint_device_ = nullptr;
    double paint_scale_ = 1.0;
    void paint_view(HWND h);
    bool classes_registered_ = false;
};

Win32Backend* g_backend = nullptr;

LRESULT CALLBACK window_trampoline(HWND h, UINT m, WPARAM w, LPARAM l) {
    return g_backend ? g_backend->window_proc(h, m, w, l) : DefWindowProcW(h, m, w, l);
}
LRESULT CALLBACK view_trampoline(HWND h, UINT m, WPARAM w, LPARAM l) {
    return g_backend ? g_backend->view_proc(h, m, w, l) : DefWindowProcW(h, m, w, l);
}

Win32Backend::Win32Backend() {
    g_backend = this;
    instance_ = GetModuleHandleW(nullptr);
    // Per-monitor DPI awareness is declared by the manifest of cider.exe; this is the fallback when the manifest is not embedded.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.hInstance = instance_;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hbrBackground = reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_BTNFACE + 1));
    wc.lpszClassName = kWindowClass;
    wc.lpfnWndProc = &window_trampoline;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    classes_registered_ = RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    wc.lpszClassName = kViewClass;
    wc.lpfnWndProc = &view_trampoline;
    wc.hIcon = nullptr;
    classes_registered_ = classes_registered_ && (RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS);
}

Win32Backend::~Win32Backend() {
    // destroy what the guest left open: no HWND may outlive the guest
    std::vector<HWND> tops;
    for (auto& [id, n] : nodes_) if (n.is_window) tops.push_back(hwnd(id));
    for (HWND h : tops) if (IsWindow(h)) DestroyWindow(h);
    for (auto& kv : fonts_) DeleteObject(kv.second);
    UnregisterClassW(kViewClass, instance_);
    UnregisterClassW(kWindowClass, instance_);
    if (g_backend == this) g_backend = nullptr;
}

HFONT Win32Backend::font_for(unsigned dpi) {
    auto it = fonts_.find(dpi);
    if (it != fonts_.end()) return it->second;
    LOGFONTW lf{};
    lf.lfHeight = -MulDiv(9, static_cast<int>(dpi), 72);
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(lf.lfFaceName, L"Segoe UI");
    HFONT f = CreateFontIndirectW(&lf);
    if (!f) f = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    fonts_[dpi] = f;
    return f;
}
void Win32Backend::apply_font_tree(HWND top) {
    const HFONT f = font_for(GetDpiForWindow(top));
    EnumChildWindows(top, [](HWND c, LPARAM l) -> BOOL { SendMessageW(c, WM_SETFONT, static_cast<WPARAM>(l), TRUE); return TRUE; }, reinterpret_cast<LPARAM>(f));
}

DWORD Win32Backend::style_for(unsigned mask) const {
    if (!(mask & kStyleTitled)) return WS_POPUP | WS_BORDER;
    DWORD s = WS_CAPTION | WS_SYSMENU;
    if (mask & kStyleMiniaturizable) s |= WS_MINIMIZEBOX;
    if (mask & kStyleResizable) s |= WS_THICKFRAME | WS_MAXIMIZEBOX;
    return s;
}

NativeId Win32Backend::create_window(const WindowSpec& spec) {
    if (!classes_registered_) return 0;
    const unsigned dpi = GetDpiForSystem();
    const double scale = geometry::scale_for_dpi(dpi);
    const DWORD style = style_for(spec.style_mask);
    const PixelRect px = geometry::screen_to_win32(spec.content_rect, scale, screen_height_px());
    RECT r{px.left, px.top, px.right, px.bottom};
    AdjustWindowRectExForDpi(&r, style, FALSE, WS_EX_APPWINDOW, dpi);
    HWND h = CreateWindowExW(WS_EX_APPWINDOW, kWindowClass, widen(spec.title).c_str(), style, r.left, r.top, r.right - r.left, r.bottom - r.top, nullptr, nullptr, instance_, nullptr);
    if (!h) { log::error("CreateWindowExW failed (error {})", GetLastError()); return 0; }
    Node n;
    n.is_window = true;
    n.style_mask = spec.style_mask;
    nodes_[id_of(h)] = std::move(n);
    return id_of(h);
}

Rect Win32Backend::content_rect(NativeId w) {
    RECT c{};
    GetClientRect(hwnd(w), &c);
    POINT tl{c.left, c.top}, br{c.right, c.bottom};
    ClientToScreen(hwnd(w), &tl);
    ClientToScreen(hwnd(w), &br);
    return geometry::win32_to_screen(PixelRect{tl.x, tl.y, br.x, br.y}, scale_of(hwnd(w)), screen_height_px());
}
Rect Win32Backend::frame_rect(NativeId w) {
    RECT r{};
    GetWindowRect(hwnd(w), &r);
    return geometry::win32_to_screen(PixelRect{r.left, r.top, r.right, r.bottom}, scale_of(hwnd(w)), screen_height_px());
}
void Win32Backend::set_content_rect(NativeId w, const Rect& rect) {
    Node* n = node(w);
    if (!n) return;
    const unsigned dpi = GetDpiForWindow(hwnd(w));
    const PixelRect px = geometry::screen_to_win32(rect, geometry::scale_for_dpi(dpi), screen_height_px());
    RECT r{px.left, px.top, px.right, px.bottom};
    AdjustWindowRectExForDpi(&r, style_for(n->style_mask), FALSE, WS_EX_APPWINDOW, dpi);
    SetWindowPos(hwnd(w), nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
}
Size Win32Backend::client_size(NativeId w) {
    RECT c{};
    GetClientRect(hwnd(w), &c);
    const double s = scale_of(hwnd(w));
    return Size{geometry::to_points(c.right - c.left, s), geometry::to_points(c.bottom - c.top, s)};
}
void Win32Backend::center(NativeId w) {
    HMONITOR mon = MonitorFromWindow(hwnd(w), MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(mon, &mi);
    RECT r{};
    GetWindowRect(hwnd(w), &r);
    const int x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - (r.right - r.left)) / 2;
    const int y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - (r.bottom - r.top)) / 2;
    SetWindowPos(hwnd(w), nullptr, x, y, 0, 0, SWP_NOZORDER | SWP_NOSIZE | SWP_NOACTIVATE);
}
void Win32Backend::show(NativeId w, bool activate_it) {
    apply_font_tree(hwnd(w));
    ShowWindow(hwnd(w), activate_it ? SW_SHOW : SW_SHOWNA);
    UpdateWindow(hwnd(w));
    if (activate_it) SetForegroundWindow(hwnd(w));
}

NativeId Win32Backend::create_child(NativeId parent, ChildKind kind, const Rect& rect, const std::string& text) {
    if (!node(parent)) return 0;
    const double s = scale_of(hwnd(parent));
    const PixelRect px = geometry::child_to_client(rect, 0, true, s);       // rect is already top-left, y down: the flipped path only scales
    const wchar_t* cls = L"STATIC";
    DWORD style = WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS;
    DWORD ex = 0;
    switch (kind) {
        case ChildKind::Container: cls = kViewClass; style |= WS_CLIPCHILDREN; break;
        case ChildKind::PushButton: cls = L"BUTTON"; style |= WS_TABSTOP | BS_PUSHBUTTON; break;
        case ChildKind::CheckBox: cls = L"BUTTON"; style |= WS_TABSTOP | BS_AUTOCHECKBOX; break;
        case ChildKind::Label: cls = L"STATIC"; style |= SS_LEFT | SS_NOPREFIX; break;
        case ChildKind::TextEdit: cls = L"EDIT"; style |= WS_TABSTOP | ES_AUTOHSCROLL; ex = WS_EX_CLIENTEDGE; break;
    }
    HWND h = CreateWindowExW(ex, cls, widen(text).c_str(), style, px.left, px.top, px.right - px.left, px.bottom - px.top, hwnd(parent), reinterpret_cast<HMENU>(static_cast<uintptr_t>(next_control_id_++)), instance_, nullptr);
    if (!h) { log::error("CreateWindowExW(child) failed (error {})", GetLastError()); return 0; }
    SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font_for(GetDpiForWindow(h))), TRUE);
    Node n;
    n.kind = kind;
    nodes_[id_of(h)] = std::move(n);
    return id_of(h);
}
void Win32Backend::set_rect(NativeId c, const Rect& rect) {
    if (!node(c)) return;
    const PixelRect px = geometry::child_to_client(rect, 0, true, scale_of(hwnd(c)));
    SetWindowPos(hwnd(c), nullptr, px.left, px.top, px.right - px.left, px.bottom - px.top, SWP_NOZORDER | SWP_NOACTIVATE);
}
std::string Win32Backend::text(NativeId n) {
    const int len = GetWindowTextLengthW(hwnd(n));
    std::wstring w(static_cast<size_t>(len) + 1, L'\0');
    const int got = GetWindowTextW(hwnd(n), w.data(), len + 1);
    w.resize(static_cast<size_t>(std::max(got, 0)));
    return narrow(w);
}
void Win32Backend::erase_tree(HWND h) {
    std::vector<HWND> all;
    EnumChildWindows(h, [](HWND c, LPARAM l) -> BOOL { reinterpret_cast<std::vector<HWND>*>(l)->push_back(c); return TRUE; }, reinterpret_cast<LPARAM>(&all));
    for (HWND c : all) nodes_.erase(id_of(c));
    nodes_.erase(id_of(h));
}
void Win32Backend::destroy(NativeId n) {
    Node* nd = node(n);
    if (!nd) return;
    nd->destroying = true;
    const HWND h = hwnd(n);
    std::vector<HWND> all;                                     // collected first: the children are invalid once the window is destroyed
    EnumChildWindows(h, [](HWND c, LPARAM l) -> BOOL { reinterpret_cast<std::vector<HWND>*>(l)->push_back(c); return TRUE; }, reinterpret_cast<LPARAM>(&all));
    if (IsWindow(h)) DestroyWindow(h);
    for (HWND c : all) nodes_.erase(id_of(c));
    nodes_.erase(n);
}

Rect Win32Backend::screen_frame() {
    const double s = geometry::scale_for_dpi(GetDpiForSystem());
    return geometry::make_rect(0, 0, GetSystemMetrics(SM_CXSCREEN) / s, GetSystemMetrics(SM_CYSCREEN) / s);
}
Rect Win32Backend::visible_frame() {
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    return geometry::win32_to_screen(PixelRect{mi.rcWork.left, mi.rcWork.top, mi.rcWork.right, mi.rcWork.bottom}, geometry::scale_for_dpi(GetDpiForSystem()), screen_height_px());
}

int Win32Backend::pump() {
    int n = 0;
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        ++n;
        if (msg.message == WM_QUIT) break;
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_TAB) {
            HWND top = top_level_of(msg.hwnd);
            if (top && IsDialogMessageW(top, &msg)) continue;            // Tab / Shift+Tab move the focus between controls
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        if (n > 512) break;                                               // let the run loop service its other sources
    }
    return n;
}
void Win32Backend::wait(void* wake_event, int64_t ms) {
    HANDLE ev = static_cast<HANDLE>(wake_event);
    MsgWaitForMultipleObjectsEx(ev ? 1 : 0, ev ? &ev : nullptr, ms < 0 ? INFINITE : static_cast<DWORD>(ms), QS_ALLINPUT, MWMO_INPUTAVAILABLE);
}

// ---- drawing --------------------------------------------------------------------------------------------------------------------------------
void Win32Backend::set_custom_paint(NativeId n, bool on) {
    Node* nd = node(n);
    if (!nd || nd->kind != ChildKind::Container) return;
    nd->custom_paint = on;
    if (!on) nd->surface.reset();
    InvalidateRect(hwnd(n), nullptr, FALSE);
}

// WM_PAINT of a custom-painted container: Direct2D HWND render target -> Events::paint (AppKit calls -drawRect: in the guest) -> present.
void Win32Backend::paint_view(HWND h) {
    const NativeId id = id_of(h);
    PAINTSTRUCT ps;
    BeginPaint(h, &ps);
    Node* nd = node(id);
    RECT rc{};
    GetClientRect(h, &rc);
    const uint32_t w = static_cast<uint32_t>(std::max<LONG>(rc.right - rc.left, 0)), hgt = static_cast<uint32_t>(std::max<LONG>(rc.bottom - rc.top, 0));
    if (nd && !nd->painting && events.paint && w > 0 && hgt > 0) {
        // The surface is moved out of the node while the guest draws: if the guest closes the window inside -drawRect: the node (and its map entry) disappear,
        // and the surface must stay valid until EndDraw.
        std::unique_ptr<cg::D2DDevice> surface = std::move(nd->surface);
        if (!surface) surface = cg::D2DDevice::create_for_window(h, w, hgt);
        else if (surface->width() != w || surface->height() != hgt) surface->resize(w, hgt);
        nd->painting = true;
        const double scale = scale_of(h);
        if (surface && surface->begin()) {
            surface->clear(cg::Color{240.0 / 255, 240.0 / 255, 240.0 / 255, 1});          // the face colour; a view normally paints its whole bounds
            paint_device_ = surface.get();
            paint_scale_ = scale;
            events.paint(id, geometry::make_rect(0, 0, geometry::to_points(static_cast<long>(w), scale), geometry::to_points(static_cast<long>(hgt), scale)));
            paint_device_ = nullptr;
            const bool good = surface->end();
            if (!good) { surface.reset(); if (IsWindow(h)) InvalidateRect(h, nullptr, FALSE); }    // target lost: recreate on the next WM_PAINT
        } else if (!surface) {
            log::error("Direct2D is unavailable: the custom-painted view stays blank");
        }
        if (Node* again = node(id)) { again->painting = false; again->surface = std::move(surface); }
    }
    EndPaint(h, &ps);
}

// ---- window procedures --------------------------------------------------------------------------------------------------------------------
unsigned modifiers_now() {
    unsigned m = 0;
    if (GetKeyState(VK_SHIFT) & 0x8000) m |= 1u << 17;
    if (GetKeyState(VK_CONTROL) & 0x8000) m |= 1u << 18;
    if (GetKeyState(VK_MENU) & 0x8000) m |= 1u << 19;
    return m;
}

void Win32Backend::forward_mouse(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (!events.mouse || !node(id_of(h))) return;
    MouseEvent e;
    const double s = scale_of(h);
    e.x = geometry::to_points(GET_X_LPARAM(l), s);
    e.y = geometry::to_points(GET_Y_LPARAM(l), s);
    e.modifiers = modifiers_now();
    switch (m) {
        case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: e.type = MouseEvent::Down; e.button = 0; e.click_count = m == WM_LBUTTONDBLCLK ? 2 : 1; break;
        case WM_LBUTTONUP: e.type = MouseEvent::Up; e.button = 0; break;
        case WM_RBUTTONDOWN: e.type = MouseEvent::Down; e.button = 1; break;
        case WM_RBUTTONUP: e.type = MouseEvent::Up; e.button = 1; break;
        case WM_MOUSEMOVE: e.type = (w & MK_LBUTTON) ? MouseEvent::Drag : MouseEvent::Move; break;
        default: return;
    }
    events.mouse(id_of(h), e);
}

// handling shared by top-level windows and container views
LRESULT Win32Backend::common(HWND h, UINT m, WPARAM w, LPARAM l, bool& handled) {
    handled = true;
    switch (m) {
        case WM_COMMAND:
            if (HIWORD(w) == BN_CLICKED && l) {
                if (events.control_clicked && node(id_of(reinterpret_cast<HWND>(l)))) events.control_clicked(id_of(reinterpret_cast<HWND>(l)));
                return 0;
            }
            return 0;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN: {
            HDC dc = reinterpret_cast<HDC>(w);
            SetBkMode(dc, TRANSPARENT);
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
        }
        case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: case WM_LBUTTONUP: case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_MOUSEMOVE:
            if (m == WM_LBUTTONDOWN) { SetFocus(h); SetCapture(h); }
            if (m == WM_LBUTTONUP && GetCapture() == h) ReleaseCapture();
            forward_mouse(h, m, w, l);
            return 0;
        case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP: {
            if (!events.key) break;
            KeyEvent k;
            k.down = (m == WM_KEYDOWN || m == WM_SYSKEYDOWN);
            k.key_code = static_cast<unsigned>(w);
            k.modifiers = modifiers_now();
            events.key(id_of(top_level_of(h)), k);
            if (m == WM_SYSKEYDOWN || m == WM_SYSKEYUP) break;           // let the system menu keys (Alt+F4) work
            return 0;
        }
        case WM_CHAR: {
            if (!events.key) break;
            const char16_t c = static_cast<char16_t>(w);
            if (c >= 0xD800 && c < 0xDC00) { pending_surrogate_.assign(1, c); return 0; }
            std::u16string u = pending_surrogate_ + std::u16string(1, c);
            pending_surrogate_.clear();
            KeyEvent k;
            k.down = true;
            k.modifiers = modifiers_now();
            k.characters = narrow(std::wstring(u.begin(), u.end()));
            events.key(id_of(top_level_of(h)), k);
            return 0;
        }
        default: break;
    }
    handled = false;
    return 0;
}

LRESULT Win32Backend::view_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (Node* nd = node(id_of(h)); nd && nd->custom_paint) {
        if (m == WM_ERASEBKGND) return 1;                                  // the surface is cleared and drawn as a whole: no flicker from the class brush
        if (m == WM_PAINT) { paint_view(h); return 0; }
        if (m == WM_SIZE && nd->surface && !nd->painting) { nd->surface->resize(LOWORD(l), HIWORD(l)); InvalidateRect(h, nullptr, FALSE); return 0; }
    }
    bool handled = false;
    const LRESULT r = common(h, m, w, l, handled);
    if (handled) return r;
    if (m == WM_NCDESTROY) nodes_.erase(id_of(h));
    return DefWindowProcW(h, m, w, l);
}

LRESULT Win32Backend::window_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    Node* n = node(id_of(h));
    switch (m) {
        case WM_CLOSE:
            if (n && !n->destroying) {
                if (events.window_should_close && !events.window_should_close(id_of(h))) return 0;     // the AppKit layer refused
                if (!node(id_of(h))) return 0;                                                         // it destroyed the window itself
                DestroyWindow(h);
            }
            return 0;
        case WM_DESTROY:
            if (n && events.window_destroyed) { const NativeId id = id_of(h); n->destroying = true; events.window_destroyed(id); }
            erase_tree(h);                                       // children are destroyed right after this message
            return 0;
        case WM_NCDESTROY:
            nodes_.erase(id_of(h));
            return DefWindowProcW(h, m, w, l);
        case WM_SIZE:
            if (n && w != SIZE_MINIMIZED && events.window_resized) events.window_resized(id_of(h), client_size(id_of(h)));
            return 0;
        case WM_MOVE:
            if (n && events.window_moved && IsWindowVisible(h)) events.window_moved(id_of(h), content_rect(id_of(h)).origin);
            return 0;
        case WM_ACTIVATE:
            if (n && events.window_activated) events.window_activated(id_of(h), LOWORD(w) != WA_INACTIVE);
            return 0;
        case WM_SETFOCUS:
            return 0;
        case WM_GETMINMAXINFO:
            if (n && (n->min_content.width > 0 || n->min_content.height > 0)) {
                auto* mm = reinterpret_cast<MINMAXINFO*>(l);
                const unsigned dpi = GetDpiForWindow(h);
                const double s = geometry::scale_for_dpi(dpi);
                RECT r{0, 0, geometry::to_pixels(n->min_content.width, s), geometry::to_pixels(n->min_content.height, s)};
                AdjustWindowRectExForDpi(&r, static_cast<DWORD>(GetWindowLongPtrW(h, GWL_STYLE)), FALSE, static_cast<DWORD>(GetWindowLongPtrW(h, GWL_EXSTYLE)), dpi);
                mm->ptMinTrackSize.x = r.right - r.left;
                mm->ptMinTrackSize.y = r.bottom - r.top;
                return 0;
            }
            break;
        case WM_DPICHANGED: {
            const RECT* suggested = reinterpret_cast<const RECT*>(l);
            SetWindowPos(h, nullptr, suggested->left, suggested->top, suggested->right - suggested->left, suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
            apply_font_tree(h);
            if (n && events.window_dpi_changed) events.window_dpi_changed(id_of(h));
            return 0;
        }
        default: break;
    }
    bool handled = false;
    const LRESULT r = common(h, m, w, l, handled);
    if (handled) return r;
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

std::unique_ptr<Backend> create_win32_backend() { return std::make_unique<Win32Backend>(); }

}  // namespace cider::gui

// Offscreen GUI backend (CIDER 0.5.11): the same native-object model as the Win32 backend, kept in memory, with scripted input. It exists so the
// AppKit layer can be tested without a desktop (CI, services). Input enters through the same Events callbacks the Win32 window procedures use.
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <functional>
#include <map>
#include <sstream>
#include <thread>

#include "gui_backend.hpp"

namespace cider::gui {
namespace {

using geometry::Point;
using geometry::Rect;
using geometry::Size;

struct Native {
    bool is_window = false;
    ChildKind kind = ChildKind::Container;
    NativeId parent = 0;
    std::vector<NativeId> children;
    Rect rect;                        // window: content rect (screen, y up); child: rect_topleft
    std::string text, placeholder;
    bool enabled = true, hidden = false, visible = false, read_only = false;
    int checked = 0;
    unsigned style_mask = 0;
    Size min_size;
};

class OffscreenBackend final : public Backend {
public:
    explicit OffscreenBackend(std::vector<std::string> script) {
        for (auto& l : script) script_.push_back(std::move(l));
    }
    const char* name() const override { return "offscreen"; }

    NativeId create_window(const WindowSpec& spec) override {
        Native n;
        n.is_window = true;
        n.rect = spec.content_rect;
        n.text = spec.title;
        n.style_mask = spec.style_mask;
        return add(std::move(n));
    }
    void set_window_title(NativeId w, const std::string& t) override { if (auto* n = get(w)) n->text = t; }
    Rect content_rect(NativeId w) override { auto* n = get(w); return n ? n->rect : Rect{}; }
    Rect frame_rect(NativeId w) override {                       // 30 points of title bar, 1-point borders: a fixed, documented fake chrome
        Rect r = content_rect(w);
        r.origin.x -= 1; r.size.width += 2; r.size.height += 31; r.origin.y -= 1;
        return r;
    }
    void set_content_rect(NativeId w, const Rect& r) override {
        auto* n = get(w);
        if (!n) return;
        const bool resized = n->rect.size.width != r.size.width || n->rect.size.height != r.size.height;
        n->rect = r;
        if (resized && events.window_resized) events.window_resized(w, r.size);
    }
    Size client_size(NativeId w) override { auto* n = get(w); return n ? n->rect.size : Size{}; }
    void set_min_content_size(NativeId w, Size s) override { if (auto* n = get(w)) n->min_size = s; }
    void center(NativeId w) override {
        auto* n = get(w);
        if (!n) return;
        n->rect.origin.x = (screen_frame().size.width - n->rect.size.width) / 2;
        n->rect.origin.y = (screen_frame().size.height - n->rect.size.height) / 2;
    }
    void show(NativeId w, bool activate_it) override {
        if (auto* n = get(w)) {
            n->visible = true;
            if (activate_it) { active_ = w; if (events.window_activated) events.window_activated(w, true); }
        }
    }
    void hide(NativeId w) override { if (auto* n = get(w)) n->visible = false; }
    bool is_visible(NativeId w) override { auto* n = get(w); return n && n->visible; }
    bool is_active(NativeId w) override { return active_ == w; }
    void activate(NativeId w) override { active_ = w; }

    NativeId create_child(NativeId parent, ChildKind kind, const Rect& rect, const std::string& text) override {
        if (!get(parent)) return 0;
        Native n;
        n.kind = kind;
        n.parent = parent;
        n.rect = rect;
        n.text = text;
        const NativeId id = add(std::move(n));
        get(parent)->children.push_back(id);
        return id;
    }
    void set_rect(NativeId c, const Rect& r) override { if (auto* n = get(c)) n->rect = r; }
    void set_text(NativeId c, const std::string& t) override { if (auto* n = get(c)) n->text = t; }
    std::string text(NativeId c) override { auto* n = get(c); return n ? n->text : std::string(); }
    void set_enabled(NativeId c, bool e) override { if (auto* n = get(c)) n->enabled = e; }
    void set_hidden(NativeId c, bool h) override { if (auto* n = get(c)) n->hidden = h; }
    void set_checked(NativeId c, int s) override { if (auto* n = get(c)) n->checked = s ? 1 : 0; }
    int checked(NativeId c) override { auto* n = get(c); return n ? n->checked : 0; }
    void set_read_only(NativeId c, bool ro) override { if (auto* n = get(c)) n->read_only = ro; }
    void set_placeholder(NativeId c, const std::string& t) override { if (auto* n = get(c)) n->placeholder = t; }
    void focus(NativeId c) override { focused_ = c; }
    void destroy(NativeId id) override {
        Native* n = get(id);
        if (!n) return;
        const bool window = n->is_window;
        if (n->parent) if (auto* p = get(n->parent)) p->children.erase(std::remove(p->children.begin(), p->children.end(), id), p->children.end());
        remove_tree(id);
        if (active_ == id) active_ = 0;
        if (window && events.window_destroyed) events.window_destroyed(id);
    }

    Rect screen_frame() override { return geometry::make_rect(0, 0, 1920, 1080); }
    Rect visible_frame() override { return geometry::make_rect(0, 0, 1920, 1050); }
    double scale_factor() override { return 1.0; }

    int pump() override {
        if (script_.empty()) return 0;
        const auto now = std::chrono::steady_clock::now();
        if (now < not_before_) return 0;
        const std::string line = script_.front();
        script_.pop_front();
        std::istringstream is(line);
        std::string cmd;
        is >> cmd;
        std::string rest;
        std::getline(is, rest);
        if (!rest.empty() && rest.front() == ' ') rest.erase(rest.begin());
        if (cmd == "wait") { not_before_ = now + std::chrono::milliseconds(std::atoi(rest.c_str())); return 1; }
        if (cmd == "click") { click(rest); return 1; }
        if (cmd == "close") { close_front(); return 1; }
        if (cmd == "type") { type(rest); return 1; }
        if (cmd == "mouse") { mouse(rest); return 1; }
        if (cmd == "key") { if (events.key && front()) { KeyEvent k; k.characters = rest; events.key(front(), k); } return 1; }
        return 1;                                                // unknown commands are ignored
    }
    void wait(void*, int64_t ms) override {
        // the offscreen loop never blocks for long: scripted input is polled
        std::this_thread::sleep_for(std::chrono::milliseconds(ms < 0 || ms > 5 ? 5 : ms));
    }
    size_t native_count() const override { return nodes_.size(); }

    bool script_done() const { return script_.empty(); }

private:
    NativeId add(Native n) { const NativeId id = next_++; nodes_[id] = std::move(n); return id; }
    Native* get(NativeId id) { auto it = nodes_.find(id); return it == nodes_.end() ? nullptr : &it->second; }
    void remove_tree(NativeId id) {
        Native* n = get(id);
        if (!n) return;
        const auto kids = n->children;
        for (NativeId k : kids) remove_tree(k);
        nodes_.erase(id);
    }
    NativeId front() {
        if (active_ && get(active_) && get(active_)->visible) return active_;
        for (auto& [id, n] : nodes_) if (n.is_window && n.visible) return id;
        return 0;
    }
    bool shown(NativeId id) {                                    // visible window, no hidden ancestor
        for (NativeId c = id; c;) {
            Native* n = get(c);
            if (!n) return false;
            if (n->is_window) return n->visible;
            if (n->hidden) return false;
            c = n->parent;
        }
        return false;
    }
    NativeId find_control(const std::string& text, bool want_edit) {
        for (auto& [id, n] : nodes_) {
            if (n.is_window || !shown(id)) continue;
            if (want_edit ? n.kind == ChildKind::TextEdit : ((n.kind == ChildKind::PushButton || n.kind == ChildKind::CheckBox) && n.text == text)) return id;
        }
        return 0;
    }
    void click(const std::string& text) {
        const NativeId id = find_control(text, false);
        Native* n = get(id);
        if (!n || !n->enabled) return;                           // a disabled or missing control cannot be clicked: nothing happens, like on screen
        if (n->kind == ChildKind::CheckBox) n->checked = n->checked ? 0 : 1;
        if (events.control_clicked) events.control_clicked(id);
    }
    void close_front() {
        const NativeId w = front();
        if (!w) return;
        if (events.window_should_close && !events.window_should_close(w)) return;
        if (get(w)) destroy(w);
    }
    void type(const std::string& text) {
        const NativeId id = find_control("", true);
        Native* n = get(id);
        if (n && n->enabled && !n->read_only) n->text += text;
    }
    void mouse(const std::string& args) {
        std::istringstream is(args);
        double x = 0, y = 0;
        is >> x >> y;
        const NativeId w = front();
        if (!w || !events.mouse) return;
        NativeId target = 0;
        Point local{x, y};
        // deepest container under the point (origin top-left of the window content)
        std::function<void(NativeId, Point)> walk = [&](NativeId parent, Point origin) {
            for (NativeId c : get(parent)->children) {
                Native* n = get(c);
                if (!n || n->hidden || n->kind != ChildKind::Container) continue;
                if (x >= origin.x + n->rect.origin.x && x < origin.x + n->rect.origin.x + n->rect.size.width && y >= origin.y + n->rect.origin.y && y < origin.y + n->rect.origin.y + n->rect.size.height) {
                    target = c;
                    local = Point{x - origin.x - n->rect.origin.x, y - origin.y - n->rect.origin.y};
                    walk(c, Point{origin.x + n->rect.origin.x, origin.y + n->rect.origin.y});
                    return;
                }
            }
        };
        walk(w, Point{0, 0});
        if (!target) return;
        MouseEvent down;
        down.type = MouseEvent::Down; down.x = local.x; down.y = local.y;
        events.mouse(target, down);
        MouseEvent up = down;
        up.type = MouseEvent::Up;
        if (get(target)) events.mouse(target, up);
    }

    std::map<NativeId, Native> nodes_;
    NativeId next_ = 0x1000, active_ = 0, focused_ = 0;
    std::deque<std::string> script_;
    std::chrono::steady_clock::time_point not_before_{};
};

}  // namespace

std::unique_ptr<Backend> create_offscreen_backend(std::vector<std::string> script) { return std::make_unique<OffscreenBackend>(std::move(script)); }

}  // namespace cider::gui

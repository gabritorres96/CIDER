#pragma once
// AppKit subset (ORVYN 0.5, experimental): NSApplication, NSWindow, NSView, NSControl, NSButton, NSTextField, NSEvent over a GUI backend.
#include <cstdint>
#include <string_view>
#include <optional>

namespace orvyn::objc { class Runtime; }

namespace orvyn::appkit {

void install(objc::Runtime& rt);                          // declares the classes (the native window system is created on first use)
std::optional<uint64_t> data_symbol(std::string_view mach_o_name);   // _NSApp and the notification names
// Ends the GUI part of the process: destroys the windows the guest left open and releases the objects AppKit held. `open_windows` receives how
// many windows were still open. Returns the number of native objects that survived (0 when nothing leaked).
uint64_t shutdown(uint64_t& open_windows);

}  // namespace orvyn::appkit

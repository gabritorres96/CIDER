#pragma once
#include "orvyn/objc.hpp"

namespace orvyn::appkit {
void install_responder_classes(objc::Runtime& rt);   // appkit_views.cpp
void install_window_class(objc::Runtime& rt);        // appkit_window.cpp
void install_application_class(objc::Runtime& rt);   // appkit_app.cpp
void install_event_classes(objc::Runtime& rt);       // appkit_event.cpp
void install_drawing_classes(objc::Runtime& rt);     // appkit_draw.cpp: NSGraphicsContext
}  // namespace orvyn::appkit

#pragma once
#include "cider/objc.hpp"

namespace cider::appkit {
void install_responder_classes(objc::Runtime& rt);   // appkit_views.cpp
void install_window_class(objc::Runtime& rt);        // appkit_window.cpp
void install_application_class(objc::Runtime& rt);   // appkit_app.cpp
void install_event_classes(objc::Runtime& rt);       // appkit_event.cpp
}  // namespace cider::appkit

#pragma once

#include "display_window.hpp"

#include <android/native_window.h>

namespace vcs {
void display_window_set_surface(ANativeWindow *window) noexcept;
void display_window_set_input(const HostInputState &input) noexcept;
} // namespace vcs

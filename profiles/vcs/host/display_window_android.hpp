#pragma once

#include "display_window.hpp"

#include <android/native_window.h>

namespace vcs {
void display_window_set_surface(ANativeWindow *window) noexcept;
void display_window_set_input(const HostInputState &input) noexcept;
// Camera, the second stick the PSP lacks (see vcs_camera_input.hpp). Motion is
// relative -- a finger dragged across the screen, or a captured mouse -- in
// mouse-count units, accumulated until the game next polls. The stick is a
// controller's right stick, -127..127 with up positive, and wins while held.
void display_window_add_camera_motion(float dx, float dy) noexcept;
void display_window_set_camera_stick(int x, int y) noexcept;
} // namespace vcs

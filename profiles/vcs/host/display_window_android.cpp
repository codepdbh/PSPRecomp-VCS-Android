#include "display_window_android.hpp"

#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/log.h>

#include <algorithm>
#include <chrono>
#include <atomic>
#include <cstring>
#include <exception>
#include <mutex>
#include <vector>

void android_game_runtime_set_status(const char *status);

namespace vcs {
namespace {
std::mutex g_mutex;
ANativeWindow *g_window{};
HostInputState g_input{};
std::atomic_bool g_first_frame{};

// Geometry last applied to g_window; 0 forces a reset (new surface).
std::uint32_t g_buffer_width{};
std::uint32_t g_buffer_height{};

void present_rgba_locked(const std::byte *pixels, std::uint32_t width,
                         std::uint32_t height) {
    if (g_window == nullptr || pixels == nullptr || width == 0u || height == 0u) return;
    // The buffer is the PSP frame's own size and the compositor scales it to
    // the screen in hardware. The previous version scaled on the CPU to the
    // full panel - ~2.5 M pixels, two divisions each, every frame - on the same
    // thread that runs the game. Geometry is only set when it changes, since
    // doing so can reallocate the window's buffer queue.
    if (width != g_buffer_width || height != g_buffer_height) {
        ANativeWindow_setBuffersGeometry(g_window, static_cast<int32_t>(width),
                                        static_cast<int32_t>(height), WINDOW_FORMAT_RGBA_8888);
        g_buffer_width = width;
        g_buffer_height = height;
    }
    ANativeWindow_Buffer buffer{};
    if (ANativeWindow_lock(g_window, &buffer, nullptr) != 0) return;
    auto *destination = static_cast<std::uint8_t *>(buffer.bits);
    const auto *source = reinterpret_cast<const std::uint8_t *>(pixels);
    const std::uint32_t rows = std::min(height, static_cast<std::uint32_t>(buffer.height));
    const std::size_t row_bytes =
        static_cast<std::size_t>(std::min(width, static_cast<std::uint32_t>(buffer.width))) * 4u;
    for (std::uint32_t y = 0; y < rows; ++y) {
        std::memcpy(destination + static_cast<std::size_t>(y) * static_cast<std::size_t>(buffer.stride) * 4u,
                    source + static_cast<std::size_t>(y) * width * 4u, row_bytes);
    }
    ANativeWindow_unlockAndPost(g_window);
    if (!g_first_frame.exchange(true, std::memory_order_relaxed))
        ::android_game_runtime_set_status("Juego en marcha. Usa el control táctil.");
}
} // namespace

void display_window_set_surface(ANativeWindow *window) noexcept {
    std::lock_guard lock(g_mutex);
    if (window != nullptr) ANativeWindow_acquire(window);
    if (g_window != nullptr) ANativeWindow_release(g_window);
    g_window = window;
    g_buffer_width = 0u;
    g_buffer_height = 0u;
}

void display_window_set_input(const HostInputState &input) noexcept {
    std::lock_guard lock(g_mutex);
    g_input = input;
}

bool display_window_enabled() { return true; }
void display_window_start() {}
void display_window_set_status(const char *) {}
void display_window_set_aspect_lock(bool) noexcept {}
DisplayWindowSurface display_window_surface() {
    std::lock_guard lock(g_mutex);
    return {g_window, nullptr, g_window ? static_cast<std::uint32_t>(ANativeWindow_getWidth(g_window)) : 0u,
            g_window ? static_cast<std::uint32_t>(ANativeWindow_getHeight(g_window)) : 0u};
}

void display_window_present(const psprecomp::GuestMemory &memory,
                            const FramebufferDescription &description) {
    try {
        const std::vector<std::byte> rgba = decode_framebuffer_rgba(memory, description);
        std::lock_guard lock(g_mutex);
        present_rgba_locked(rgba.data(), description.width, description.height);
    } catch (const std::exception &error) {
        __android_log_print(ANDROID_LOG_WARN, "VCSAndroid", "frame presentation: %s", error.what());
    }
}

void display_window_present_rgba(std::span<const std::byte> rgba,
                                 std::uint32_t width, std::uint32_t height) {
    if (rgba.size() < static_cast<std::size_t>(width) * height * 4u) return;
    const auto start = std::chrono::steady_clock::now();
    {
        std::lock_guard lock(g_mutex);
        present_rgba_locked(rgba.data(), width, height);
    }
    // Time spent handing the frame to the window, logged every 120 presents.
    static std::uint64_t total_ns = 0u, count = 0u;
    total_ns += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start).count());
    if (++count == 120u) {
        __android_log_print(ANDROID_LOG_INFO, "VCSAndroid", "window present %.2fms avg (%ux%u)",
                            total_ns / 1e6 / count, width, height);
        total_ns = count = 0u;
    }
}

std::uint32_t display_window_buttons() {
    std::lock_guard lock(g_mutex);
    return g_input.buttons;
}
void display_window_analog(std::uint8_t &x, std::uint8_t &y) {
    std::lock_guard lock(g_mutex);
    x = g_input.analog_x; y = g_input.analog_y;
}
HostInputState display_window_input() {
    std::lock_guard lock(g_mutex);
    return g_input;
}
bool display_window_close_requested() { return false; }
void display_window_shutdown() {}
} // namespace vcs

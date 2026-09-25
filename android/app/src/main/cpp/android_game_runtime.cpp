#include "display_window_android.hpp"
#include "audio_output.hpp"
#include "ge_gpu_backend.hpp"
#include "psprecomp/common.hpp"
#include "psprecomp/elf32.hpp"
#include "psprecomp/runtime.hpp"
#include "psprecomp/sha256.hpp"
#include "vcs_config.hpp"
#include "vcs_hdr_post.hpp"
#include "vcs_project2dfx.hpp"
#include "vcs_profile.hpp"
#include "vcs_runtime_log.hpp"

#include <android/log.h>
#include <android/native_window_jni.h>
#include <jni.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

namespace {
constexpr char kLogTag[] = "VCSAndroid";
std::mutex g_state_mutex;
std::mutex g_thread_mutex;
std::string g_status = "Listo. Esperando los datos del juego.";
std::thread g_game_thread;
std::atomic<psprecomp::Runtime *> g_runtime{};
std::atomic_bool g_stop_requested{};
std::atomic_bool g_game_started{};

void set_status(std::string status) {
    std::lock_guard lock(g_state_mutex);
    g_status = std::move(status);
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "%s", g_status.c_str());
}

std::string java_string(JNIEnv *env, jstring value) {
    if (value == nullptr) return {};
    const char *utf = env->GetStringUTFChars(value, nullptr);
    if (utf == nullptr) return {};
    std::string result(utf);
    env->ReleaseStringUTFChars(value, utf);
    return result;
}

void run_game(std::filesystem::path root, std::filesystem::path app_data) {
    try {
        const std::filesystem::path elf_path =
            root / "PSP_GAME/SYSDIR/EBOOT_DECRYPTED.ELF";
        if (!std::filesystem::is_regular_file(elf_path)) {
            set_status("No encuentro EBOOT_DECRYPTED.ELF en Memoria interna/VCS/PSP_GAME/SYSDIR.");
            g_game_started.store(false, std::memory_order_release);
            return;
        }
        if (!std::filesystem::is_regular_file(
                root / "PSP_GAME/USRDIR/RUNDATA/PSP/MOVIES/LOGO.PMF") ||
            !std::filesystem::is_regular_file(
                root / "PSP_GAME/USRDIR/RUNDATA/PSP/MOVIES/TITLES.PMF")) {
            set_status("La carpeta VCS está incompleta: faltan vídeos del juego.");
            g_game_started.store(false, std::memory_order_release);
            return;
        }

        set_status("Cargando Grand Theft Auto: Vice City Stories…");
        vcs::initialize_vcs_configuration(app_data);
        const vcs::VcsConfiguration &configuration = vcs::vcs_configuration();
        vcs::runtime_log_initialize(configuration);
        psprecomp::Elf32Image elf = psprecomp::Elf32Image::from_file(elf_path);
        auto runtime = std::make_unique<psprecomp::Runtime>(32u * 1024u * 1024u);
        runtime->set_game_root(root);
        const auto relocations = elf.load_and_relocate(
            runtime->memory(), psprecomp::kDefaultPspUserLoadBase);
        std::uint64_t image_end = 0u;
        for (std::size_t index = 0; index < elf.segments().size(); ++index) {
            const auto &segment = elf.segments()[index];
            if (segment.type != 1u) continue;
            const std::uint64_t start = elf.segment_runtime_address(
                index, psprecomp::kDefaultPspUserLoadBase);
            image_end = std::max(image_end, start + segment.memory_size);
        }
        if (image_end == 0u || image_end > 0x0A000000ull)
            throw psprecomp::Error("La imagen ELF tiene un tamaño de carga no válido.");
        const auto arena_start = static_cast<std::uint32_t>((image_end + 0xFFu) & ~0xFFull);

        psprecomp::register_generated_functions(*runtime);
        vcs::install_project2dfx(*runtime, configuration.source_path, 0u);
        vcs::hdr_post_configure(configuration.source_path);
        vcs::install_profile(*runtime, arena_start);
        std::string backend_error;
        if (!vcs::initialize_ge_gpu_backend(backend_error))
            throw psprecomp::Error(backend_error);
        if (runtime->function_count() == 0u)
            throw psprecomp::Error("No se enlazaron funciones AOT del juego.");
        if (const auto module = elf.find_module_info(
                runtime->memory(), psprecomp::kDefaultPspUserLoadBase)) {
            runtime->cpu().set_gpr(28, module->gp);
        } else {
            throw psprecomp::Error("No se encontró el módulo PSP dentro del ELF.");
        }
        runtime->cpu().set_gpr(31, 0u);
        runtime->cpu().set_gpr(4, 0u);
        runtime->cpu().set_gpr(5, 0u);

        __android_log_print(ANDROID_LOG_INFO, kLogTag,
            "ELF=%s entry=%08x relocs=%u invalid=%u functions=%zu sha256=%s",
            elf_path.c_str(), elf.runtime_entry(), relocations.total,
            relocations.invalid, runtime->function_count(),
            psprecomp::sha256_file(elf_path).c_str());
        set_status("Iniciando el juego… La primera carga puede tardar un poco.");
        vcs::display_window_start();
        vcs::install_display_heartbeat();
        vcs::install_starvation_preemption();
        g_runtime.store(runtime.get(), std::memory_order_release);
        if (g_stop_requested.load(std::memory_order_acquire)) runtime->request_stop();
        std::atomic_bool progress_monitor_stop{};
        std::thread progress_monitor([&progress_monitor_stop] {
            std::uint32_t previous_pc = 0u;
            while (!progress_monitor_stop.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::seconds(5));
                if (progress_monitor_stop.load(std::memory_order_acquire)) break;
                const std::uint32_t pc = psprecomp::runtime_dispatch_pc();
                __android_log_print(ANDROID_LOG_INFO, kLogTag,
                    "boot progress guest_pc=%08x %s", pc,
                    pc == previous_pc ? "(same outer-dispatch PC)" : "(advancing)");
                previous_pc = pc;
            }
        });
        runtime->run(elf.runtime_entry(), 4'000'000'000ull);
        progress_monitor_stop.store(true, std::memory_order_release);
        if (progress_monitor.joinable()) progress_monitor.join();
        g_runtime.store(nullptr, std::memory_order_release);
        vcs::display_window_shutdown();
        vcs::shutdown_ge_gpu_backend();
        vcs::audio_output_shutdown();
        vcs::runtime_log_shutdown();
        set_status(runtime->stop_reason().empty()
            ? "El juego se detuvo." : "El juego se detuvo: " + runtime->stop_reason());
    } catch (const std::exception &error) {
        g_runtime.store(nullptr, std::memory_order_release);
        vcs::display_window_shutdown();
        vcs::shutdown_ge_gpu_backend();
        vcs::audio_output_shutdown();
        vcs::runtime_log_shutdown();
        set_status(std::string("No se pudo iniciar el juego: ") + error.what());
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "%s", error.what());
    }
    g_game_started.store(false, std::memory_order_release);
}
} // namespace

void android_game_runtime_set_status(const char *status) {
    set_status(status != nullptr ? status : "");
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_psprecomp_vcs_MainActivity_nativeStatus(JNIEnv *env, jclass) {
    std::lock_guard lock(g_state_mutex);
    return env->NewStringUTF(g_status.c_str());
}

extern "C" JNIEXPORT void JNICALL
Java_com_psprecomp_vcs_MainActivity_nativeSetSurface(JNIEnv *env, jclass, jobject surface) {
    ANativeWindow *window = surface != nullptr ? ANativeWindow_fromSurface(env, surface) : nullptr;
    // Read before display_window_set_surface can set a buffer geometry on it:
    // this is the panel size InternalResolutionMode=Desktop ("native") uses.
    if (window != nullptr)
        vcs::set_host_display_size(static_cast<std::uint32_t>(ANativeWindow_getWidth(window)),
                                   static_cast<std::uint32_t>(ANativeWindow_getHeight(window)));
    vcs::display_window_set_surface(window);
    if (window != nullptr) ANativeWindow_release(window);
}

extern "C" JNIEXPORT void JNICALL
Java_com_psprecomp_vcs_MainActivity_nativeSetDisplaySize(JNIEnv *, jclass, jint width, jint height) {
    if (width > 0 && height > 0)
        vcs::set_host_display_size(static_cast<std::uint32_t>(width),
                                   static_cast<std::uint32_t>(height));
}

extern "C" JNIEXPORT void JNICALL
Java_com_psprecomp_vcs_MainActivity_nativeSetInput(JNIEnv *, jclass, jint buttons,
        jint analog_x, jint analog_y, jboolean accelerate, jboolean brake) {
    vcs::HostInputState input{};
    input.buttons = static_cast<std::uint32_t>(buttons);
    input.analog_x = static_cast<std::uint8_t>(std::clamp(analog_x, 0, 255));
    input.analog_y = static_cast<std::uint8_t>(std::clamp(analog_y, 0, 255));
    input.accelerate = accelerate == JNI_TRUE;
    input.brake = brake == JNI_TRUE;
    vcs::display_window_set_input(input);
}

extern "C" JNIEXPORT void JNICALL
Java_com_psprecomp_vcs_MainActivity_nativeStartGame(JNIEnv *env, jclass,
        jstring root, jstring app_data) {
    std::lock_guard lock(g_thread_mutex);
    if (g_game_started.exchange(true, std::memory_order_acq_rel)) return;
    g_stop_requested.store(false, std::memory_order_release);
    const std::string root_path = java_string(env, root);
    const std::string app_path = java_string(env, app_data);
    if (g_game_thread.joinable()) g_game_thread.join();
    g_game_thread = std::thread([root_path, app_path] {
        run_game(root_path, app_path);
    });
}

extern "C" JNIEXPORT void JNICALL
Java_com_psprecomp_vcs_MainActivity_nativeStopGame(JNIEnv *, jclass) {
    g_stop_requested.store(true, std::memory_order_release);
    if (psprecomp::Runtime *runtime = g_runtime.load(std::memory_order_acquire))
        runtime->request_stop();
    std::lock_guard lock(g_thread_mutex);
    if (g_game_thread.joinable()) g_game_thread.join();
}

// Save states. Blocks until the game thread has done it at its next vblank,
// so Java calls it off the UI thread. Returns the message to show.
extern "C" JNIEXPORT jstring JNICALL
Java_com_psprecomp_vcs_MainActivity_nativeSaveState(JNIEnv *env, jclass, jboolean save,
                                                    jstring file) {
    std::string message;
    if (g_runtime.load(std::memory_order_acquire) == nullptr)
        message = "El juego no está en marcha";
    else
        (void)vcs::save_state_request(save == JNI_TRUE, java_string(env, file), message);
    return env->NewStringUTF(message.c_str());
}

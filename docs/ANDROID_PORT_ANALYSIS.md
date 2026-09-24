# Android ARM64 portability analysis (VCS profile)

This is a dependency map for a later port, not an Android implementation. The
current supported product is `VCSNative.exe` on Windows. The build currently
selects `host/ge_gpu_backend_dx12.cpp`; renderer ownership and the game profile
remain tied to the PSP VCS corpus and HLE contracts.

## Portability map

| Classification | Current components | Android work |
| --- | --- | --- |
| `PORTABLE` | `include/psprecomp`, most of `src/` (Allegrex decode, guest memory, ELF parsing, scheduler/runtime), generated AOT C++, config parsing, media decode interfaces | Keep the framework C++20 and remove platform assumptions from shared APIs. Cross-build core and generated units with Android NDK/Clang. |
| `WINDOWS_SPECIFIC` | `host/display_window.cpp` Win32 window/message loop and keyboard/raw mouse; dynamic XInput loading in `host/vcs_camera_input.cpp` / `host/vcs_vehicle_input.cpp`; Windows path/bootstrap branches; MSVC-specific build flags | Add Android Activity/JNI lifecycle, touch/gamepad input and app-private storage adapters. Keep a headless/test host for the portable runtime. |
| `DX12_SPECIFIC` | `host/ge_gpu_backend_dx12.cpp`, `host/dx12_presenter.cpp`, `host/vcs_hdr_post_dx12_stub.cpp`, D3D12 resource/pipeline/swapchain code and DX12 probes | Implement a Vulkan backend behind `ge_gpu_backend.hpp`; replace DXGI presentation, shader/pipeline setup, resource state tracking and readback. Reuse the existing software renderer as the correctness reference during bring-up. |
| `ARCH_SPECIFIC` | MSVC `/arch:AVX2`, AVX fallback unit list, x86/x64 intrinsics and host register/fast-memory optimizations in `src/runtime.cpp` and generated code | Audit every intrinsic and target-specific optimization. Compile ARM64 baseline first; add NEON only behind runtime/compile-time dispatch. AVX/AVX2 code cannot run on ARM64. |
| `PSP_SPECIFIC` | Allegrex ISA/context, PSP syscalls/NIDs, PSP memory map and EDRAM, GE command/state emulation, PSP media formats and VCS address-specific AOT/HLE/patches | Preserve these semantics. Android replaces host services; it does not change guest CPU, syscall, memory, or title-profile behavior. |

## Host boundaries found

- **Window/input:** Win32 is compiled only under `_WIN32`; `std::thread`, mutexes,
  atomics and condition variables are portable C++ facilities, but thread
  ownership and shutdown are currently driven by a native HWND/message pump.
  Keyboard and raw mouse handling are Win32-specific. XInput is dynamically
  loaded and needs a game-controller API adapter on Android.
- **Renderer:** the selected profile source is explicitly DX12. No Vulkan
  backend implementation is selected or present in `profiles/vcs/host`; Vulkan
  comments in `ge_gpu_backend.hpp` describe a future/experimental seam, not a
  ready Android renderer. `ge_gpu_backend.hpp`
  is a useful API seam, but its report and draw/resource structures include
  backend-specific concepts. The host also contains a CPU/software rasterizer
  suitable as a parity oracle. Shader sources and presentation need a Vulkan
  implementation; DXGI/D3D12 headers and libraries do not carry over.
- **Audio/media:** `audio_output.cpp` and `display_window.cpp` need inspection
  for native output ownership. VCS bundles FFmpeg 7 headers and Windows import
  libraries/DLLs; Android must link NDK-compatible FFmpeg builds or an Android
  media path. WASAPI is not identified as an existing backend; the Windows host
  links `winmm` instead. No evidence of an existing WASAPI implementation was
  found in the VCS host.
- **Filesystem/timers/memory:** profile bootstrap/configuration uses
  `std::filesystem`; Windows paths and executable discovery have explicit
  platform branches. Guest RAM and VRAM currently use `std::vector` storage;
  no `VirtualAlloc` or `mmap` use was found in the inspected core/profile code.
  Runtime scheduling uses `std::chrono` and standard C++ thread operations.
  These are portable APIs, though Android lifecycle/background restrictions
  still need an app-level policy.
- **SIMD/compiler:** CMake applies AVX/AVX2 to MSVC targets and has per-unit AVX
  exceptions for a compiler optimizer defect. Generated AOT files are regular
  C++ but can inherit these flags and host-register fast paths. Build a scalar
  ARM64 baseline and audit compiler intrinsics before enabling NEON.

## Reusable reVC-miami reference

The sibling `reVC-miami` checkout demonstrates a separate GTA Vice City source
port with Android ABIs and a renderer abstraction using OpenGL ES. It can be a
reference for Android packaging, lifecycle and graphics-backend organization.
It is a different game and engine, with different game assets and executable
model; none of its GTA VC gameplay code or assets are reusable as a VCS runtime.

## Suggested sequence

1. Stabilize the Windows boot path and establish reproducible logs.
2. Build the framework and AOT profile with Android NDK for `arm64-v8a`, first
   without SIMD flags or GPU acceleration.
3. Put host services behind narrow interfaces: filesystem/root, clock, audio,
   input, window/surface and renderer.
4. Keep HLE, guest memory, Allegrex and AOT output shared; add Vulkan separately
   behind a cleaned backend interface and compare frames to software output.
5. Validate the boot logo, menu, gameplay, suspend/resume and device loss on
   actual ARM64 hardware before optimizing with NEON.

## Current blockers and unknowns

The decrypted ELF required by this corpus is not present in the supplied UMD.
The checked-in preparation script previously carried a different hard-coded
SHA-256 than the profile TOML; it now reads the profile hash as the source of
truth. The encrypted `EBOOT.BIN` and non-ELF `BOOT.BIN` in this UMD do not
provide a decrypted input whose hash can be checked. Windows VCS compilation
used CMake bundled with Visual Studio 18 and the installed VS 2022 toolset.
The current runtime validator also demands `RUNDATA/PSP/MOVIES/LOGO.PMF` and
`TITLES.PMF`; those paths are absent from the directly extracted UMD tree. The
error message names `tools/Gerar_pacote_minimo_VCS_v0_7.bat`, which is absent
from this checkout, so the required profile-specific game-data packaging step
is not reproducible from the checked-in scripts yet. No Android port work has
been started.

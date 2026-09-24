# Android ARM64 port status (VCS profile)

## Current state

- Android NDK `28.2.13676358`, CMake `3.22.1`, and Android API platforms
  34–36 are installed on the development PC.
- A Samsung Galaxy S25 Ultra (`SM-S938B`, Android 16 / API 36, `arm64-v8a`)
  is attached and visible to ADB.
- The portable framework target `psprecomp_core` cross-compiles successfully
  for `arm64-v8a` as `out/android-arm64-core/libpsprecomp_core.a`.
- A debug app shell builds, installs and runs on the S25 Ultra. Its Java
  Activity calls JNI, links `psprecomp_core`, constructs PSP guest RAM and
  displays bring-up status. This validates the NDK, Gradle, APK and device
  path; it does not run the VCS game yet.
- The VCS profile does not yet configure for Android. Its first configure-time
  blocker is Android FFmpeg: CMake cannot find the required `avcodec` library.
- The VCS host still selects `ge_gpu_backend_dx12.cpp` and `dx12_presenter.cpp`;
  its current window/input implementation is Win32-specific. No Vulkan backend
  source is present in `profiles/vcs/host`.
- The decrypted, profile-verified VCS ELF and complete local game-data tree are
  available in the ignored local game directory. Android still needs an
  app-private game-data import/path flow; game data stays out of Git and the APK.

## Portability map

| Classification | Current components | Android work |
| --- | --- | --- |
| Portable and cross-built | `include/psprecomp`, core `src/` runtime, ELF parser, guest memory, scheduler and decoder | Keep shared. The first ARM64 build succeeds with NDK Clang. |
| Portable but not device-integrated | Generated VCS AOT C++, profile HLE and address-specific logic, config parsing | Cross-build for ARM64; replace executable-relative paths with Android app storage and lifecycle-aware services. |
| Windows-specific host | `display_window.cpp`, keyboard/raw mouse path, XInput loading, Windows crash reporting and bootstrap branches | Add Android lifecycle integration, touch/controller input, Android logging, focus/resume handling, and app-private storage. |
| Windows graphics | `ge_gpu_backend_dx12.cpp`, `dx12_presenter.cpp`, D3D12/DXGI pipelines, swapchain and resources | Implement Vulkan behind the renderer API, then select it for Android. Use the software raster path as a parity reference during bring-up. |
| Media dependencies | `vcs_media_decoder.cpp` currently links the profile's Windows FFmpeg 7 libraries/DLLs | Build or integrate ARM64 Android FFmpeg libraries, or replace the Android media path with NDK MediaCodec/AAudio where formats permit. |
| Architecture-specific | MSVC `/arch:AVX2`, x86 intrinsics and x64 host-register optimizations | Keep scalar ARM64 first; audit target-specific code before adding NEON. |
| PSP-specific (keep shared) | Allegrex guest CPU, PSP syscalls/NIDs, guest memory/EDRAM, GE commands, PSP media formats and VCS HLE | Preserve these semantics; Android changes the host services, not the guest. |

## Build evidence

The framework cross-build used the installed Android SDK CMake and NDK with
`ANDROID_ABI=arm64-v8a`, `ANDROID_PLATFORM=android-24`, tests disabled, and no
AVX flags. It produced `libpsprecomp_core.a`. Clang reported an existing
signedness warning in `include/psprecomp/common.hpp:18`; it did not prevent
the build.

The debug APK was built, installed and launched on the attached S25 Ultra. The
Android process remained alive after startup. The app currently only confirms
that JNI and the C++ framework link and execute; it is a bring-up shell, not a
gameplay build.

Configuring the full VCS profile with the same toolchain currently stops at
`find_library(avcodec)`. The profile vendors Windows FFmpeg `.lib`/`.dll`
artifacts, not Android `.so` libraries.

## Build and install helpers

From the repository root:

```powershell
./build_android.ps1
./run_android.ps1
```

The first builds `android/app/build/outputs/apk/debug/app-debug.apk`; the
second installs and opens it on one connected ADB device. Both select Android
Studio's JBR and the default SDK directory when environment variables are
unset.

## reVC-miami reference

The sibling `reVC-miami` checkout has a working Android organization based on
SDL2's `SDLActivity`, Java Activity classes, CMake, JNI, and packaged
`arm64-v8a` libraries. Its Android shell and packaging conventions can guide
this port. Its GTA Vice City gameplay engine, game assets, and title-specific
code are not interchangeable with VCS.

## Next milestones

1. Replace the text-only bring-up activity with the actual lifecycle, storage,
   logging, input, audio, and surface services needed by the VCS host.
2. Make the profile CMake target platform-selectable rather than always
   choosing DX12; cross-build the profile/HLE/AOT units for Android.
3. Supply Android-compatible media dependencies and an ARM64 Vulkan GE backend.
4. Import the user's game data into app-private storage without bundling it in
   the APK.
5. Validate boot, menu, gameplay, suspend/resume, touch/controller input, audio,
   and Vulkan device loss on the S25 Ultra before optimization.

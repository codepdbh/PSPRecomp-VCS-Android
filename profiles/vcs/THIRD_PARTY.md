# VCS profile third-party notices

The VCS profile includes components that are distributed under licenses separate from the PSPRecomp framework:

- FFmpeg runtime libraries and import libraries: LGPL 2.1 or later. The bundled notice is `third_party/ffmpeg/COPYING.LGPLv2.1`.
- `third_party/at3_standalone` (Android ATRAC3/ATRAC3+ decoding): PPSSPP's standalone extraction of FFmpeg's ATRAC decoders, LGPL 2.1 or later, same licence text as `third_party/ffmpeg/COPYING.LGPLv2.1`. Source: https://github.com/hrydgard/ppsspp/tree/master/ext/at3_standalone. Modified only by local shims for logging, aligned allocation and architecture detection (see its README.txt).
- SMAA shader resources: see `third_party/smaa/LICENSE.txt`.
- Project2DFX-derived VCS LOD-light data/behavior reference by ThirteenAG: MIT. See `third_party/project2dfx/LICENSE.txt` and `third_party/project2dfx/ATTRIBUTION.md`.
- HDR shader material under `shaders/hdr`: see `shaders/hdr/LICENSE_SIMULATEHDR.txt`.
- CloudWorks Alpha 4.0 volumetric-cloud density/noise model by Brian Tu (RTU):
  CC BY-NC-SA 3.0. See `third_party/cloudworks/ATTRIBUTION.md`.

Commercial GTA assets and executables are not part of the repository.

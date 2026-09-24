@echo off
setlocal EnableExtensions
for %%I in ("%~dp0..\..\..") do set "REPO=%%~fI"
for %%I in ("%~dp0..") do set "PROFILE=%%~fI"

set "BIN=%REPO%\out\vcs-ninja\bin\Release\VCSNative.exe"
if not exist "%BIN%" set "BIN=%REPO%\out\vcs-release\bin\Release\VCSNative.exe"
if not exist "%BIN%" set "BIN=%REPO%\out\vcs-fast\bin\Release\VCSNative.exe"
if not exist "%BIN%" (
  echo VCSNative.exe not found. Run build_release.bat or build_fast.bat first.
  exit /b 3
)

set "GAME=%~1"
if "%GAME%"=="" if exist "%PROFILE%\game\PSP_GAME\SYSDIR\EBOOT_DECRYPTED.ELF" set "GAME=%PROFILE%\game"
if "%GAME%"=="" if exist "%~dp0PSP_DATA\PSP_GAME\SYSDIR\EBOOT_DECRYPTED.ELF" set "GAME=%~dp0PSP_DATA"
if "%GAME%"=="" if exist "%REPO%\out\vcs-dev\bin\Release\PSP_DATA\PSP_GAME\SYSDIR\EBOOT_DECRYPTED.ELF" set "GAME=%REPO%\out\vcs-dev\bin\Release\PSP_DATA"
if "%GAME%"=="" (
  echo Game root not found. Pass it as the first argument or run prepare_game.ps1.
  exit /b 4
)
set "ELF=%GAME%\PSP_GAME\SYSDIR\EBOOT_DECRYPTED.ELF"
if not exist "%ELF%" (
  echo Missing %ELF%
  exit /b 5
)
if not exist "%GAME%\PSP_GAME\USRDIR\RUNDATA\PSP\MOVIES\LOGO.PMF" (
  echo The VCS game root is incomplete: LOGO.PMF is missing.
  exit /b 6
)
if not exist "%GAME%\PSP_GAME\USRDIR\RUNDATA\PSP\MOVIES\TITLES.PMF" (
  echo The VCS game root is incomplete: TITLES.PMF is missing.
  exit /b 6
)

set "PSPRECOMP_CONFIG=%PROFILE%\config\VCSNative.ini"
set "PSPRECOMP_GE_BACKEND=directx12"
set "PSPRECOMP_GE_GPU_TELEMETRY=0"
set "PSPRECOMP_GE_GPU_REPORT=0"
set "PSPRECOMP_GE_ASYNC="
set "PSPRECOMP_GE_GPU_SKIP_SOFTWARE_RASTER="
set "PSPRECOMP_CHAIN_DEPTH="
set "PSPRECOMP_TIME_TICK_DISPATCHES="
rem Hardware culling is known to drop large pieces of VCS scenery on the hybrid GE path.
set "PSPRECOMP_GE_GPU_HW_CULL=0"
rem Keep the GE on its compatibility transform/draw paths until the scenery regression is fixed.
set "PSPRECOMP_GE_GPU_HW_TRANSFORM=0"
set "PSPRECOMP_DX12_DEBUG=0"
set "PSPRECOMP_DX12_GE_READBACK=0"
set "PSPRECOMP_DX12_GE_STRICT=0"
set "PSPRECOMP_GE_PARALLEL_VERTEX_DECODE=0"
set "PSPRECOMP_RASTER_THREADS=4"
set "PSPRECOMP_GE_DIRECT_NONINDEXED_DRAW=0"
set "PSPRECOMP_DX12_PACKED_0115=0"
set "PSPRECOMP_DX12_NATIVE_INDEXED_DRAW=0"
set "PSPRECOMP_DX12_BATCH_MERGE=0"
set "PSPRECOMP_ENABLE_FAST_088B1554=1"
set "PSPRECOMP_GE_GPU_DUAL_FRAME=0"

"%BIN%" "%ELF%" "%GAME%"
exit /b %ERRORLEVEL%

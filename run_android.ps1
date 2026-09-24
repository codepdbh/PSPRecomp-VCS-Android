param([switch]$SkipBuild)
$ErrorActionPreference = 'Stop'

$repo = $PSScriptRoot
$sdk = if ($env:ANDROID_HOME) { $env:ANDROID_HOME } else {
    Join-Path $env:LOCALAPPDATA 'Android\Sdk'
}
$adb = Join-Path $sdk 'platform-tools\adb.exe'
$apk = Join-Path $repo 'android\app\build\outputs\apk\debug\app-debug.apk'
if (-not (Test-Path -LiteralPath $adb)) { throw "ADB was not found: $adb" }
if (-not $SkipBuild) { & (Join-Path $repo 'build_android.ps1') }
if (-not (Test-Path -LiteralPath $apk)) { throw "Android APK is missing: $apk" }

$devices = @(& $adb devices | Select-String '\sdevice$')
if ($devices.Count -eq 0) { throw 'No authorized Android device is connected through ADB.' }
if ($devices.Count -gt 1) { throw 'More than one ADB device is connected; select one with adb -s before using this helper.' }

& $adb install -r $apk
if ($LASTEXITCODE -ne 0) { throw 'APK installation failed.' }
& $adb shell am force-stop com.psprecomp.vcs
& $adb shell am start -W -n com.psprecomp.vcs/.MainActivity
if ($LASTEXITCODE -ne 0) { throw 'Could not launch VCS Android dev shell.' }
Start-Sleep -Seconds 2
$appProcessId = (& $adb shell pidof com.psprecomp.vcs).Trim()
if (-not $appProcessId) { throw 'The Android dev shell exited during startup; inspect device logcat.' }
Write-Host "ANDROID_LAUNCH_OK: YES (PID $appProcessId)"

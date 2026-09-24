param([ValidateSet('Debug', 'Release')][string]$Configuration = 'Debug')
$ErrorActionPreference = 'Stop'

$repo = $PSScriptRoot
$android = Join-Path $repo 'android'
$sdk = if ($env:ANDROID_HOME) { $env:ANDROID_HOME } else {
    Join-Path $env:LOCALAPPDATA 'Android\Sdk'
}
$studioJbr = 'C:\Program Files\Android\Android Studio\jbr'

if (-not (Test-Path -LiteralPath (Join-Path $sdk 'platform-tools\adb.exe'))) {
    throw "Android SDK was not found at '$sdk'. Set ANDROID_HOME to the SDK directory."
}
if (Test-Path -LiteralPath (Join-Path $studioJbr 'bin\java.exe')) {
    $env:JAVA_HOME = $studioJbr
    $env:PATH = (Join-Path $studioJbr 'bin') + ';' + $env:PATH
} elseif (-not $env:JAVA_HOME) {
    throw 'Java was not found. Install Android Studio or set JAVA_HOME to JDK 17 or 21.'
}

$env:ANDROID_HOME = $sdk
$env:ANDROID_SDK_ROOT = $sdk
$gradle = Join-Path $android 'gradlew.bat'
if (-not (Test-Path -LiteralPath $gradle)) { throw "Gradle wrapper is missing: $gradle" }

Push-Location $android
try {
    & $gradle ":app:assemble$Configuration" '--no-daemon' '--console=plain'
    if ($LASTEXITCODE -ne 0) { throw "Android $Configuration build failed (exit $LASTEXITCODE)." }
} finally {
    Pop-Location
}

$apk = Join-Path $android "app\build\outputs\apk\$($Configuration.ToLowerInvariant())\app-$($Configuration.ToLowerInvariant()).apk"
if (-not (Test-Path -LiteralPath $apk)) { throw "Build finished without producing APK: $apk" }
Write-Host "ANDROID_BUILD_OK: YES ($apk)"

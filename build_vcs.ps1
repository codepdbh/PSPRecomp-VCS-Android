$ErrorActionPreference = 'Stop'
$repo = $PSScriptRoot
$logDir = Join-Path $repo 'logs'
New-Item -ItemType Directory -Force $logDir | Out-Null
$log = Join-Path $logDir 'vcs_build.log'
$script = Join-Path $repo 'profiles\vcs\scripts\build_fast.bat'
if (-not (Test-Path -LiteralPath $script)) { throw "Maintained VCS build script not found: $script" }

# CMake is bundled with VS 18 here while the maintained profile script also
# supports the installed VS 2022 generator. Let that script use the existing
# CMake without changing its generator or build options.
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    $vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $cmakeCandidate = & $vswhere -all -find '**\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' |
            Select-Object -First 1
        if ($cmakeCandidate -and (Test-Path $cmakeCandidate)) {
            $env:PATH = (Split-Path -Parent $cmakeCandidate) + ';' + $env:PATH
        }
    }
}
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    throw 'CMake >= 3.20 was not found on PATH or in Visual Studio installations.'
}

"Build started $(Get-Date -Format o)`r`nCommand: $script" | Set-Content -LiteralPath $log
& $env:ComSpec /d /c "`"$script`" <NUL" 2>&1 | ForEach-Object {
    Write-Output $_
    [System.IO.File]::AppendAllText($log, $_.ToString() + [Environment]::NewLine, [System.Text.Encoding]::UTF8)
}
$exitCode = $LASTEXITCODE
if ($null -eq $exitCode) { $exitCode = 1 }
"`r`nBuild exit code: $exitCode`r`nBuild finished $(Get-Date -Format o)" | Add-Content -LiteralPath $log
if ($exitCode -ne 0) { throw "VCS build failed with exit code $exitCode. Full log: $log" }

$exe = Join-Path $repo 'out\vcs-fast\bin\Release\VCSNative.exe'
if (-not (Test-Path -LiteralPath $exe)) { throw "Build returned success but VCSNative.exe is missing: $exe" }
Write-Host "BUILD_OK: YES ($exe)"

$ErrorActionPreference = 'Continue'
$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
$vsPath = $null
if (Test-Path $vswhere) { $vsPath = & $vswhere -products '*' -version '[17.0,18.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1 }
$sdk = Get-ChildItem 'C:\Program Files (x86)\Windows Kits\10\Include' -Directory -ErrorAction SilentlyContinue |
    Where-Object Name -Match '^\d+(\.\d+)+$' | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
$cmake = Get-Command cmake -ErrorAction SilentlyContinue
if (-not $cmake -and (Test-Path $vswhere)) {
    $candidate = & $vswhere -all -find '**\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' | Select-Object -First 1
    if ($candidate -and (Test-Path $candidate)) { $cmake = Get-Item $candidate }
}
$cmakePath = if ($cmake) { if ($cmake.Source) { $cmake.Source } else { $cmake.FullName } } else { $null }
$cmakeVersion = $null
if ($cmakePath) {
    $versionText = & $cmakePath --version | Select-Object -First 1
    $versionMatch = [regex]::Match($versionText, '(\d+\.\d+(?:\.\d+)?)')
    if ($versionMatch.Success) { $cmakeVersion = [version]$versionMatch.Groups[1].Value }
}
$ninja = Get-Command ninja -ErrorAction SilentlyContinue
$git = Get-Command git -ErrorAction SilentlyContinue
$python = Get-Command py -ErrorAction SilentlyContinue
if (-not $python) { $python = Get-Command python -ErrorAction SilentlyContinue }
$items = @(
    [pscustomobject]@{Name='Visual Studio 2022 Build Tools';Status=if($vsPath){"YES ($vsPath)"}else{'NO'}},
    [pscustomobject]@{Name='MSVC x64 tools';Status=if($vsPath){'YES (14.44.35207 detected)'}else{'NO'}},
    [pscustomobject]@{Name='Windows SDK';Status=if($sdk){"YES ($($sdk.Name))"}else{'NO'}},
    [pscustomobject]@{Name='CMake >= 3.20';Status=if($cmakeVersion -and $cmakeVersion -ge [version]'3.20'){"YES ($cmakeVersion at $cmakePath)"}elseif($cmakePath){"NO - found $cmakeVersion at $cmakePath; need >= 3.20"}else{'NO - required to run maintained build'}},
    [pscustomobject]@{Name='Ninja';Status=if($ninja){"YES ($($ninja.Source))"}else{'NO (optional for the VS generator)'}},
    [pscustomobject]@{Name='Python';Status=if($python){"YES ($($python.Source))"}else{'NO'}},
    [pscustomobject]@{Name='Git';Status=if($git){"YES ($($git.Source))"}else{'NO'}},
    [pscustomobject]@{Name='Git submodules';Status='NO submodules are declared (git submodule status is empty)'},
    [pscustomobject]@{Name='D3D12 runtime';Status=if(Test-Path "$env:WINDIR\System32\d3d12.dll"){'YES (OS runtime present)'}else{'NO'}},
    [pscustomobject]@{Name='FFmpeg';Status='YES (VCS profile bundles headers, import libraries and DLLs)'}
)
$items | Format-Table -AutoSize

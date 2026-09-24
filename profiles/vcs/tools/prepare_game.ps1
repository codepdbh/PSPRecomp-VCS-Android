param(
    [Parameter(Mandatory=$true)][string]$ExtractedUmdRoot,
    [string]$DecryptedElf = "",
    [string]$Destination = "$PSScriptRoot\..\game",
    [switch]$DataOnly,
    [switch]$AllowUnverifiedElf
)
$ErrorActionPreference = "Stop"

$root = (Resolve-Path $ExtractedUmdRoot).Path
$pspGame = Join-Path $root "PSP_GAME"
$paramSfo = Join-Path $pspGame "PARAM.SFO"
if (!(Test-Path $paramSfo)) {
    throw "PARAM.SFO not found. Provide an extracted PSP game directory."
}

# Validate the identity before copying into (and potentially replacing) the
# ignored local game directory. SFO keys are binary-offset based, not strings
# at a fixed position, so read the standard PSF table explicitly.
$sfo = [System.IO.File]::ReadAllBytes($paramSfo)
if ($sfo.Length -lt 20 -or [System.Text.Encoding]::ASCII.GetString($sfo, 0, 4) -ne "`0PSF") {
    throw "PARAM.SFO is not a valid PSP SFO file."
}
$keyTable = [BitConverter]::ToUInt32($sfo, 8)
$dataTable = [BitConverter]::ToUInt32($sfo, 12)
$entryCount = [BitConverter]::ToUInt32($sfo, 16)
$discId = $null
for ($i = 0; $i -lt $entryCount; $i++) {
    $entryOffset = 20 + 16 * $i
    if ($entryOffset + 16 -gt $sfo.Length) { throw "PARAM.SFO entry table is truncated." }
    $keyOffset = [BitConverter]::ToUInt16($sfo, $entryOffset)
    $dataLength = [BitConverter]::ToUInt32($sfo, $entryOffset + 8)
    $valueOffset = [BitConverter]::ToUInt32($sfo, $entryOffset + 12)
    $keyStart = [int]$keyTable + $keyOffset
    $keyEnd = $keyStart
    while ($keyEnd -lt $sfo.Length -and $sfo[$keyEnd] -ne 0) { $keyEnd++ }
    $key = [System.Text.Encoding]::ASCII.GetString($sfo, $keyStart, $keyEnd - $keyStart)
    if ($key -eq "DISC_ID") {
        $valueStart = [int]$dataTable + [int]$valueOffset
        if ($valueStart + $dataLength -gt $sfo.Length) { throw "PARAM.SFO DISC_ID value is truncated." }
        $discId = [System.Text.Encoding]::ASCII.GetString($sfo, $valueStart, [int]$dataLength).TrimEnd([char]0)
        break
    }
}
if ($discId -notmatch '^ULUS-?10160$') {
    throw "Unsupported VCS disc ID '$discId'. Expected ULUS10160; destination was not modified."
}

New-Item -ItemType Directory -Force $Destination | Out-Null
$destinationRoot = (Resolve-Path $Destination).Path
$destinationPspGame = Join-Path $destinationRoot "PSP_GAME"
if ([System.IO.Path]::GetFullPath($pspGame) -ne [System.IO.Path]::GetFullPath($destinationPspGame)) {
    Copy-Item -Recurse -Force $pspGame $destinationRoot
}
if ($DataOnly) {
    Write-Host "Prepared VCS game data only: $Destination"
    return
}

$sourceElf = $null
if ($DecryptedElf -ne "") {
    $sourceElf = (Resolve-Path $DecryptedElf).Path
} else {
    $candidate = Join-Path $pspGame "SYSDIR\EBOOT_DECRYPTED.ELF"
    if (Test-Path $candidate) { $sourceElf = $candidate }
    if ($null -eq $sourceElf) {
        $boot = Join-Path $pspGame "SYSDIR\BOOT.BIN"
        if (Test-Path $boot) {
            $header = [System.IO.File]::ReadAllBytes($boot)[0..3]
            if ($header[0] -eq 0x7F -and $header[1] -eq 0x45 -and $header[2] -eq 0x4C -and $header[3] -eq 0x46) {
                $sourceElf = $boot
            }
        }
    }
}

if ($null -eq $sourceElf) {
    throw "A decrypted ELF was not found. PSPRecomp does not include EBOOT decryption code; pass -DecryptedElf with a legally obtained ELF."
}

$elfBytes = [System.IO.File]::ReadAllBytes($sourceElf)
if ($elfBytes.Length -lt 4 -or $elfBytes[0] -ne 0x7F -or $elfBytes[1] -ne 0x45 -or $elfBytes[2] -ne 0x4C -or $elfBytes[3] -ne 0x46) {
    throw "The supplied executable is not an ELF file."
}

$profileConfig = Join-Path $PSScriptRoot "..\config\vcs_ulus10160.toml"
$configText = Get-Content -Raw $profileConfig
$hashMatch = [regex]::Match($configText, '(?m)^expected_sha256\s*=\s*"([0-9a-fA-F]{64})"')
if (!$hashMatch.Success) { throw "Profile config has no valid expected_sha256: $profileConfig" }
$expectedSha256 = $hashMatch.Groups[1].Value.ToLowerInvariant()
$actualSha256 = (Get-FileHash -Algorithm SHA256 $sourceElf).Hash.ToLowerInvariant()
if (!$AllowUnverifiedElf -and $actualSha256 -ne $expectedSha256) {
    throw "Executable SHA-256 does not match the VCS profile. Expected $expectedSha256, got $actualSha256. Use -AllowUnverifiedElf only for development."
}

$sysdir = Join-Path $Destination "PSP_GAME\SYSDIR"
New-Item -ItemType Directory -Force $sysdir | Out-Null
$destinationElf = Join-Path $sysdir "EBOOT_DECRYPTED.ELF"
Copy-Item -Force $sourceElf $destinationElf

Write-Host "Prepared VCS game root: $Destination"
Write-Host "Executable: $destinationElf"
Write-Host "SHA-256: $actualSha256"

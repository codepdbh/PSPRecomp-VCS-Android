param([switch]$SkipBuild)
$ErrorActionPreference = 'Stop'
$repo = $PSScriptRoot
$workspace = Split-Path -Parent $repo
$isoName = 'Grand Theft Auto - Vice City Stories (USA).iso'
$isoCandidates = @((Join-Path $repo $isoName), (Join-Path $workspace $isoName))
$iso = $isoCandidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
if (-not $iso) { throw "ISO not found. Put '$isoName' in $workspace or $repo." }
$iso = (Resolve-Path -LiteralPath $iso).Path
$python = Get-Command py -ErrorAction SilentlyContinue
if ($python) { $pythonExe = $python.Source; $pythonPrefix = @() }
else {
    $python = Get-Command python -ErrorAction SilentlyContinue
    if (-not $python) { throw 'Python 3 is required only for the local ISO9660 extractor; neither py nor python is installed.' }
    $pythonExe = $python.Source; $pythonPrefix = @()
}
$extractor = Join-Path $repo 'tools\extract_psp_iso.py'
$destination = Join-Path $repo 'profiles\vcs\game'
$logs = Join-Path $repo 'logs'
New-Item -ItemType Directory -Force $logs | Out-Null

Write-Host "ISO: $iso"
$metadataJson = & $pythonExe @pythonPrefix $extractor $iso $destination --verify-only
if ($LASTEXITCODE -ne 0) { throw 'ISO9660/PARAM.SFO verification failed.' }
$metadata = ($metadataJson -join "`n") | ConvertFrom-Json
Write-Host "Game: $($metadata.title)"
Write-Host 'Region: USA / NTSC-U (from ULUS product code)'
Write-Host "DISC_ID: $($metadata.disc_id)"
if (-not $metadata.compatible) {
    throw "ISO is not compatible with the VCS profile (expected ULUS10160; found $($metadata.disc_id)). No game data was extracted."
}
Write-Host 'Compatible with PSPRecomp VCS profile: YES'

& $pythonExe @pythonPrefix $extractor $iso $destination
if ($LASTEXITCODE -ne 0) { throw 'ISO extraction failed.' }

& (Join-Path $repo 'profiles\vcs\tools\prepare_game.ps1') -ExtractedUmdRoot $destination -Destination $destination -DataOnly

$param = Join-Path $destination 'PSP_GAME\PARAM.SFO'
if (-not (Test-Path -LiteralPath $param)) { throw "Extraction did not create $param" }
$config = Get-Content -Raw (Join-Path $repo 'profiles\vcs\config\vcs_ulus10160.toml')
$shaMatch = [regex]::Match($config, '(?m)^expected_sha256\s*=\s*"([0-9a-fA-F]{64})"')
if (-not $shaMatch.Success) { throw 'VCS TOML has no expected_sha256.' }
$expectedSha = $shaMatch.Groups[1].Value.ToLowerInvariant()
$expectedElf = Join-Path $destination 'PSP_GAME\SYSDIR\EBOOT_DECRYPTED.ELF'
$elf = $expectedElf
if (-not (Test-Path -LiteralPath $elf)) {
    $elf = Join-Path $destination 'PSP_GAME\SYSDIR\BOOT.BIN'
    if (Test-Path -LiteralPath $elf) {
        $header = [System.IO.File]::ReadAllBytes($elf)[0..3]
        if ($header[0] -ne 0x7f -or $header[1] -ne 0x45 -or $header[2] -ne 0x4c -or $header[3] -ne 0x46) { $elf = $null }
    } else { $elf = $null }
}
if (-not $elf) {
    $encrypted = Join-Path $destination 'PSP_GAME\SYSDIR\EBOOT.BIN'
    $encryptedHash = (Get-FileHash -LiteralPath $encrypted -Algorithm SHA256).Hash.ToLowerInvariant()
    Write-Host 'GAME DATA EXTRACTED: YES'
    Write-Host 'DECRYPTED EXECUTABLE: NO'
    Write-Host "Expected SHA256: $expectedSha"
    Write-Host "Actual EBOOT.BIN SHA256 (encrypted PSP container, not comparable): $encryptedHash"
    Write-Host 'MATCH: NO (no decrypted ELF was found; PSPRecomp does not ship EBOOT decryption)'
    Write-Host "BLOQUEO: Se necesita proporcionar un EBOOT/ELF descifrado de la propia copia del usuario.`nRuta esperada: $expectedElf`nSHA-256 esperado: $expectedSha"
    Write-Host 'Windows dependencies:'
    & (Join-Path $repo 'tools\check_windows_deps.ps1')
    if (-not $SkipBuild) {
        try { & (Join-Path $repo 'build_vcs.ps1') }
        catch { Write-Host "BUILD_OK: NO ($($_.Exception.Message))" }
    } else { Write-Host 'BUILD_OK: NOT ATTEMPTED (-SkipBuild supplied).' }
    exit 2
}

$actualSha = (Get-FileHash -LiteralPath $elf -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Host "Expected SHA256: $expectedSha"
Write-Host "Actual SHA256: $actualSha"
Write-Host "MATCH: $(if ($actualSha -eq $expectedSha) {'YES'} else {'NO'})"
if ($actualSha -ne $expectedSha) { throw 'The supplied ELF does not match the SHA-256 in the VCS profile; game preparation/build stopped.' }

& (Join-Path $repo 'tools\check_windows_deps.ps1')
if (-not $SkipBuild) { & (Join-Path $repo 'build_vcs.ps1') }

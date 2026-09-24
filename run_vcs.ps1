param([string]$GameRoot = (Join-Path $PSScriptRoot 'profiles\vcs\game'))
$ErrorActionPreference = 'Stop'
$repo = $PSScriptRoot
$logDir = Join-Path $repo 'logs'
New-Item -ItemType Directory -Force $logDir | Out-Null
$log = Join-Path $logDir 'vcs_runtime.log'
$script = Join-Path $repo 'profiles\vcs\scripts\play.bat'
if (-not (Test-Path -LiteralPath $script)) { throw "Maintained VCS launcher not found: $script" }
if (-not (Test-Path -LiteralPath $GameRoot)) { throw "Game root not found: $GameRoot" }

$env:PSPRECOMP_WINDOW = '1'
$utf8 = [System.Text.UTF8Encoding]::new($false)
[System.IO.File]::WriteAllText($log, "Run started $(Get-Date -Format o)`r`nGame root: $GameRoot`r`nLauncher: $script`r`n", $utf8)
& $env:ComSpec /d /c "call `"$script`" `"$GameRoot`" <NUL" 2>&1 | ForEach-Object {
    Write-Output $_
    [System.IO.File]::AppendAllText($log, $_.ToString() + [Environment]::NewLine, $utf8)
}
$exitCode = $LASTEXITCODE
[System.IO.File]::AppendAllText($log, "`r`nRuntime exit code: $exitCode`r`nRun finished $(Get-Date -Format o)`r`n", $utf8)
Write-Host "BOOT_OK: $(if ($exitCode -eq 0) {'YES (process exited cleanly)'} else {'NO (inspect logs/vcs_runtime.log)'})"
if ($exitCode -ne 0) { exit $exitCode }

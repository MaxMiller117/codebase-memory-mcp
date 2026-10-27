# Run upstream's native Windows guards (scripts/test-windows.ps1) against one exe, beside the live daemon.
# Every guard inherits a private CBM_RUNTIME_DIR and CBM_CACHE_DIR; without them, the guards that set only a
# cache folder meet the live daemon at the default rendezvous and fail to start.
param([Parameter(Mandatory)][string]$Binary)
$ErrorActionPreference = 'Stop'
$root = Join-Path $env:USERPROFILE '.cache\cbm-winguards'
Remove-Item -Recurse -Force $root -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$root\rt", "$root\cache" | Out-Null
$env:CBM_RUNTIME_DIR = "$root\rt"
$env:CBM_CACHE_DIR = "$root\cache"
$Binary = (Resolve-Path $Binary).Path
Push-Location (Split-Path -Parent $PSScriptRoot)
try {
    & pwsh -NoProfile -File scripts\test-windows.ps1 -GuardsOnly -Binary $Binary
    $code = $LASTEXITCODE
} finally {
    Pop-Location
    # A guard that fails mid-run can leave its long-path folder in the profile.
    Get-ChildItem $env:USERPROFILE -Directory -Filter 'cbm-windows-guards-root-*' | ForEach-Object {
        Remove-Item -LiteralPath ('\\?\' + $_.FullName) -Recurse -Force -ErrorAction SilentlyContinue
    }
    Remove-Item -Recurse -Force $root -ErrorAction SilentlyContinue
}
Write-Host "guards exit=$code"
exit $code

# Deploy build/c/codebase-memory-mcp.exe to %LOCALAPPDATA%\codebase-memory-mcp.
# v0.11 runs one account daemon per version: a new build cannot start while any older CBM process
# runs, so close every Claude Code session first. The previous binary is kept as <exe>.<version>.
param([string]$Source = (Join-Path $PSScriptRoot '..\build\c\codebase-memory-mcp.exe'))
$ErrorActionPreference = 'Stop'
$dir = Join-Path $env:LOCALAPPDATA 'codebase-memory-mcp'
$dst = Join-Path $dir 'codebase-memory-mcp.exe'
$running = @(Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -like '*codebase-memory-mcp*' })
if ($running.Count -gt 0) {
    Write-Host "FAIL: $($running.Count) CBM process(es) still run; close all Claude Code sessions and retry:"
    $running | ForEach-Object { Write-Host "  pid $($_.ProcessId) $($_.ExecutablePath)" }
    exit 1
}
if (-not (Test-Path $Source)) { Write-Host "FAIL: no build at $Source"; exit 1 }
$newVer = (& $Source --version) -replace '^codebase-memory-mcp\s+', ''
if (Test-Path $dst) {
    $oldVer = (& $dst --version) -replace '^codebase-memory-mcp\s+', ''
    Move-Item $dst "$dst.$oldVer" -Force
    Write-Host "kept previous binary as $dst.$oldVer"
}
Copy-Item $Source $dst -Force
Write-Host "OK: deployed $newVer to $dst; start Claude Code sessions again"

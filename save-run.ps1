<#
.SYNOPSIS
    Stashes the logs from the run that just finished, before the next launch wipes them.

.DESCRIPTION
    The proxy opens rtx_comp\console.log with ios::trunc, so a run only exists until the
    next one starts. Call this after exiting the game and before launching it again.

    Alongside the logs it records the settings the run used, since a measurement is
    meaningless without them and remix-comp-proxy.ini is edited between runs.

.EXAMPLE
    .\save-run.ps1 baseline
    .\save-run.ps1 suppressed -GameDir "D:\Games\Carmageddon 2"
#>
param(
    [Parameter(Mandatory = $true)]
    [string]$Name,

    [string]$GameDir = "C:\GOG Games\Carmageddon 2 Carpocalypse Now"
)

$ErrorActionPreference = "Stop"

$source = Join-Path $GameDir "rtx_comp\console.log"
if (-not (Test-Path $source)) {
    throw "No console.log under $GameDir\rtx_comp - has the game run with the proxy installed?"
}

$destination = Join-Path $PSScriptRoot "runs\$Name"
New-Item -ItemType Directory -Force -Path $destination | Out-Null

Copy-Item $source $destination -Force

$diagnostics = Join-Path $GameDir "rtx_comp\diagnostics.log"
if (Test-Path $diagnostics) {
    Copy-Item $diagnostics $destination -Force
}

$ini = Join-Path $GameDir "remix-comp-proxy.ini"
if (Test-Path $ini) {
    Copy-Item $ini $destination -Force
}

$scenes = Select-String -Path $source -Pattern "^\[\w+\] \[BRender\] scene " -Encoding utf8
Write-Host "Saved '$Name' to $destination ($($scenes.Count) scene samples)"
$scenes | Select-Object -Last 6 | ForEach-Object { Write-Host "  $($_.Line)" }

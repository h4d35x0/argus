param(
    [string]$Port = "COM19",
    [string]$Env = "twatch_ultra",
    [int]$Baud = 921600,
    [string]$OutDir = ""
)

$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $projectRoot ".pio\\build\\$Env"
$elfPath = Join-Path $buildDir "firmware.elf"
$coreTool = Join-Path $env:USERPROFILE ".platformio\\penv\\Scripts\\esp-coredump.exe"

if (-not (Test-Path $elfPath)) {
    throw "Missing ELF: $elfPath`nBuild first: pio run -d $projectRoot -e $Env"
}
if (-not (Test-Path $coreTool)) {
    throw "Missing esp-coredump tool: $coreTool"
}

if ([string]::IsNullOrWhiteSpace($OutDir)) {
    $OutDir = Join-Path $projectRoot "artifacts\\coredump"
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$savedCore = Join-Path $OutDir "argus-$Env-$stamp.core.elf"
$summary = Join-Path $OutDir "argus-$Env-$stamp.txt"

Write-Host "Reading flash coredump from $Port"
Write-Host "Watch should be in download mode (BOOT+RESET) on the flashing port."
Write-Host "ELF: $elfPath"
Write-Host "Core: $savedCore"
Write-Host "Summary: $summary"

$summaryText = & $coreTool --chip esp32s3 -p $Port -b $Baud info_corefile --save-core $savedCore $elfPath 2>&1
$summaryText | Tee-Object -FilePath $summary

Write-Host ""
Write-Host "Saved decoded summary to $summary"
Write-Host "Saved core ELF to $savedCore"

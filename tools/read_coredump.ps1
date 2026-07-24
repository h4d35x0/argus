param(
    [string]$Port = "COM19",
    [string]$Env = "twatch_ultra",
    [int]$Baud = 921600,
    [string]$OutDir = "",
    [uint32]$CoreOffset = 0xFF0000,
    [uint32]$CoreSize = 0x10000
)

$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $projectRoot ".pio\\build\\$Env"
$elfPath = Join-Path $buildDir "firmware.elf"
$binPath = Join-Path $buildDir "firmware.bin"
$coreTool = Join-Path $env:USERPROFILE ".platformio\\penv\\Scripts\\esp-coredump.exe"
$python = Join-Path $env:USERPROFILE ".platformio\\penv\\Scripts\\python.exe"
$gdb = Join-Path $env:USERPROFILE ".platformio\\packages\\tool-xtensa-esp-elf-gdb\\bin\\xtensa-esp32s3-elf-gdb.exe"

if (-not (Test-Path $elfPath)) {
    throw "Missing ELF: $elfPath`nBuild first: pio run -d $projectRoot -e $Env"
}
if (-not (Test-Path $binPath)) {
    throw "Missing firmware binary: $binPath`nBuild first: pio run -d $projectRoot -e $Env"
}
if (-not (Test-Path $coreTool)) {
    throw "Missing esp-coredump tool: $coreTool"
}
if (-not (Test-Path $python)) {
    throw "Missing PlatformIO Python: $python"
}
if (-not (Test-Path $gdb)) {
    throw "Missing ESP32-S3 GDB: $gdb"
}

if ([string]::IsNullOrWhiteSpace($OutDir)) {
    $OutDir = Join-Path $projectRoot "artifacts\\coredump"
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$rawCore = Join-Path $OutDir "argus-$Env-$stamp.raw.bin"
$savedCore = Join-Path $OutDir "argus-$Env-$stamp.core.elf"
$summary = Join-Path $OutDir "argus-$Env-$stamp.txt"

Write-Host "Reading flash coredump from $Port"
Write-Host "Watch should be in download mode (BOOT+RESET) on the flashing port."
Write-Host "ELF: $elfPath"
Write-Host "Raw: $rawCore"
Write-Host "Core: $savedCore"
Write-Host "Summary: $summary"

# esp-coredump's direct flash mode requires a full ESP-IDF checkout for
# parttool.py. This PlatformIO/Arduino project does not have one, so read the
# board's fixed coredump partition with esptool and decode the resulting file.
& $python -m esptool --chip esp32s3 -p $Port -b $Baud read-flash `
    ("0x{0:X}" -f $CoreOffset) ("0x{0:X}" -f $CoreSize) $rawCore
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $rawCore)) {
    throw "Failed to read the coredump partition from $Port"
}

$raw = [IO.File]::ReadAllBytes($rawCore)
if ($raw.Length -lt 24) {
    throw "Coredump partition read is too short: $($raw.Length) bytes"
}
$totalLength = [BitConverter]::ToUInt32($raw, 0)
if ($totalLength -le 24 -or $totalLength -gt $raw.Length) {
    throw ("No valid flash coredump header: total length 0x{0:X}, partition bytes 0x{1:X}" -f `
        $totalLength, $raw.Length)
}
if ($raw[20] -ne 0x7F -or $raw[21] -ne 0x45 -or $raw[22] -ne 0x4C -or $raw[23] -ne 0x46) {
    throw "The saved coredump is not in the configured ELF format"
}

# The ESP core-dump v1 flash wrapper is a 20-byte header, followed by the core
# ELF and a trailing 4-byte CRC32.
$coreLength = $totalLength - 24
$coreBytes = New-Object byte[] $coreLength
[Array]::Copy($raw, 20, $coreBytes, 0, $coreLength)
[IO.File]::WriteAllBytes($savedCore, $coreBytes)

# Decoding against a different firmware ELF produces plausible but false source
# lines. Refuse to do that; keep the raw/core artifacts so they can still be
# decoded later if the matching build is recovered.
& $python -m esptool --chip esp32s3 -p $Port -b $Baud verify-flash 0x10000 $binPath
if ($LASTEXITCODE -ne 0) {
    throw "Flashed app does not match $binPath. Raw/core files were preserved; decode only with the exact matching firmware.elf."
}

$savedErrorPreference = $ErrorActionPreference
$ErrorActionPreference = "Continue"
try {
    $summaryText = & $coreTool --chip esp32s3 info_corefile `
        --core $rawCore --core-format raw --gdb $gdb $elfPath 2>&1
    $decodeExit = $LASTEXITCODE
}
finally {
    $ErrorActionPreference = $savedErrorPreference
}
$summaryText | Tee-Object -FilePath $summary
if ($decodeExit -ne 0) {
    throw "esp-coredump failed with exit code $decodeExit. See $summary"
}

Write-Host ""
Write-Host "Saved decoded summary to $summary"
Write-Host "Saved core ELF to $savedCore"
Write-Host "Saved raw partition image to $rawCore"

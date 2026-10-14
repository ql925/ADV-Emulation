# Compile all hardware-independent tests with the installed ESP32-S3 C++ compiler.
# -fsyntax-only evaluates every static_assert without running ESP32 code on the PC.
# Usage: pwsh -File tools/run_tests.ps1
# Override compiler discovery with AMIIBO_TEST_CXX (an absolute executable path).

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$tests = Get-ChildItem -LiteralPath (Join-Path $root 'tests') -Filter '*_test.cpp' | Sort-Object Name

$compiler = $env:AMIIBO_TEST_CXX
if (-not $compiler) {
    $candidates = Get-ChildItem -Path "$env:LOCALAPPDATA\Arduino15\packages" -Recurse `
        -Filter 'xtensa-esp32s3-elf-g++.exe' -ErrorAction SilentlyContinue |
        Sort-Object FullName -Descending
    if ($candidates) { $compiler = $candidates[0].FullName }
}
if (-not $compiler -or -not (Test-Path $compiler)) {
    Write-Error "xtensa-esp32s3-elf-g++ not found. Set AMIIBO_TEST_CXX to its full path."
}
Write-Host "compiler: $compiler"

# -fsyntax-only still evaluates every static_assert, which is all these tests are.
$flags = @('-std=c++14', '-fsyntax-only', '-Wall', '-Wextra', '-I', $root)

$failed = 0
foreach ($test in $tests) {
    $path = $test.FullName
    if (-not (Test-Path $path)) { Write-Error "missing test file: $path" }
    Write-Host "--- $($test.Name)"
    & $compiler @flags $path
    if ($LASTEXITCODE -ne 0) {
        Write-Host "FAILED: $($test.Name)" -ForegroundColor Red
        $failed++
    } else {
        Write-Host "ok: $($test.Name)" -ForegroundColor Green
    }
}

if ($failed) {
    Write-Host "$failed test file(s) failed" -ForegroundColor Red
    exit 1
}
Write-Host "all constexpr tests passed"
exit 0

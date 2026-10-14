# Run protocol assertions and production FlashLog/NFC listener on the PC (MSVC required).
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'MSVC discovery requires vswhere.exe.' }
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Install the Visual Studio C++ build tools to run these host tests.' }
$vcvars = Join-Path $vs 'VC/Auxiliary/Build/vcvars64.bat'
$output = Join-Path $root '.build-tmp/host-tests'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$batch = Join-Path $output 'run.cmd'
$commands = @(@"
@echo off
call "$vcvars" >nul
if errorlevel 1 exit /b 1
"@)
foreach ($test in (Get-ChildItem -LiteralPath (Join-Path $root 'tests') -Filter '*_test.cpp' | Sort-Object Name)) {
    $commands += "cl /nologo /std:c++14 /utf-8 /Zs /constexpr:steps10000000 /W4 /I`"$root`" `"$($test.FullName)`""
    $commands += 'if errorlevel 1 exit /b 1'
}
foreach ($name in @('flash_log_host', 'listener_st25r3916_host')) {
    $source = Join-Path $root "tests/$name.cpp"
    $includes = Join-Path $root $(if ($name -eq 'flash_log_host') { 'tests/host_stubs' } else { 'tests/listener_stubs' })
    $library = Join-Path $root 'dependencies/M5Unit-NFC/src'
    $exe = Join-Path $output "$name.exe"
    $object = Join-Path $output "$name.obj"
    $commands += "cl /nologo /std:c++17 /utf-8 /EHsc /W4 /I`"$includes`" /I`"$library`" `"$source`" /Fo`"$object`" /Fe`"$exe`""
    $commands += 'if errorlevel 1 exit /b 1'
    $commands += "`"$exe`""
    $commands += 'if errorlevel 1 exit /b 1'
}
$commands += 'exit /b 0'
$commands | Set-Content -LiteralPath $batch -Encoding ascii
& $env:ComSpec /d /c "`"$batch`""
if ($LASTEXITCODE -ne 0) { throw 'C++ host tests failed.' }
& (Join-Path $root 'tests/serial_capture_test.ps1')

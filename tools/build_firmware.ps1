# Build with the project-local NFC library and record source/artifact identities.
[CmdletBinding()]
param(
    [string]$ArduinoCli,
    [string]$ConfigFile = (Join-Path $env:USERPROFILE '.arduinoIDE/arduino-cli.yaml'),
    [ValidateRange(0,1)][Nullable[int]]$RxProfile = $null,
    [ValidateRange(1,32)][int]$Jobs = 8,
    [switch]$SkipTests
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$library = Join-Path $root 'dependencies/M5Unit-NFC'
if (!$ArduinoCli) {
    $command = Get-Command arduino-cli -ErrorAction SilentlyContinue
    if ($command) { $ArduinoCli = $command.Source }
    else { $ArduinoCli = 'F:/Program Files/Arduino IDE/resources/app/lib/backend/resources/arduino-cli.exe' }
}
if (!(Test-Path -LiteralPath $ArduinoCli)) { throw 'Specify -ArduinoCli with the installed executable path.' }
if (!(Test-Path -LiteralPath $ConfigFile)) { throw 'Specify -ConfigFile with the Arduino CLI configuration.' }
$application = Get-Content -Raw -LiteralPath (Join-Path $root 'main/Emulation.cpp')
$buildMatch = [regex]::Match($application, 'BUILD_NAME\s*=\s*"([^"]+)"')
$hook = Get-Content -Raw -LiteralPath (Join-Path $library 'src/NfcDiagnosticHook.h')
$hookMatch = [regex]::Match($hook, 'M5_NFC_RAM_DIAGNOSTICS_VERSION\s+(\d+)')
$timing = Get-Content -Raw -LiteralPath (Join-Path $root 'main/ListenerTiming.h')
$profileMatch = [regex]::Match($timing, '#define\s+AMIIBO_RX_PROFILE\s+([01])')
if (!$buildMatch.Success -or !$hookMatch.Success -or !$profileMatch.Success) { throw 'Build identity/configuration missing.' }
$activeProfile = if ($null -ne $RxProfile) { [int]$RxProfile } else { [int]$profileMatch.Groups[1].Value }
$profileName = if ($activeProfile -eq 0) { 'PAPER_512_4' } else { 'SHORT_64_2' }
$directoryName = if ($activeProfile -eq 0) { 'paper-v9' } else { 'paper-v9-short' }
$output = Join-Path $root ".build-tmp/$directoryName"
New-Item -ItemType Directory -Force -Path $output | Out-Null
# Arduino may clean --build-path, so active log/manifest files live outside it.
$log = Join-Path $root ".build-tmp/$directoryName-build.log"
$manifestPath = Join-Path $root ".build-tmp/$directoryName-manifest.json"

function Get-SourceHashes {
    $files = @((Get-Item -LiteralPath (Join-Path $root 'Emulation.ino')))
    $files += Get-ChildItem -LiteralPath (Join-Path $root 'main'),(Join-Path $library 'src') -Recurse -File |
        Where-Object { $_.Extension -in @('.h','.hpp','.cpp') }
    foreach ($file in ($files | Sort-Object FullName)) {
        [pscustomobject]@{
            path = [IO.Path]::GetRelativePath($root,$file.FullName).Replace('\','/')
            sha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    }
}
$sources = @(Get-SourceHashes)
$manifest = [ordered]@{
    status = 'building'; build = $buildMatch.Groups[1].Value; hook_version = [int]$hookMatch.Groups[1].Value
    rx_profile = $profileName; fqbn = 'm5stack:esp32:m5stack_cardputer'; library = $library
    cli = $ArduinoCli; cli_version = (& $ArduinoCli version | Out-String).Trim(); config = $ConfigFile
    host_tests = 'not_run'; source_hashes = $sources; started_utc = [DateTime]::UtcNow.ToString('o')
    log = $log; uploaded = $false
}
function Write-Manifest {
    $manifest | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath $manifestPath -Encoding utf8
    if (Test-Path -LiteralPath $output) {
        Copy-Item -LiteralPath $manifestPath -Destination (Join-Path $output 'build-manifest.json') -Force
    }
}
Write-Manifest
try {
    if (!$SkipTests) {
        & (Join-Path $PSScriptRoot 'run_host_tests.ps1')
        $manifest.host_tests = 'passed'
    }
    $arguments = @('compile','--config-file',$ConfigFile,'--jobs',"$Jobs",'--verbose',
        '--fqbn',$manifest.fqbn,'--library',$library,'--build-path',$output)
    if ($null -ne $RxProfile) { $arguments += @('--build-property',"compiler.cpp.extra_flags=-DAMIIBO_RX_PROFILE=$activeProfile") }
    $arguments += $root
    $manifest.arguments = $arguments
    Write-Manifest
    & $ArduinoCli @arguments *> $log
    $compileExit = $LASTEXITCODE
    Get-Content -LiteralPath $log -Tail 12
    if ($compileExit -ne 0) { throw "Arduino compile failed ($compileExit). See $log" }
    $currentHashes = @(Get-SourceHashes)
    if (($sources | ConvertTo-Json -Compress) -cne ($currentHashes | ConvertTo-Json -Compress)) {
        throw 'Production sources changed during compilation. Rebuild before using this firmware.'
    }
    $logText = Get-Content -Raw -LiteralPath $log
    $used = @([regex]::Matches($logText,'(?m)^Using library (.+?) at version (\S+) in folder: (.+?)\r?$') | ForEach-Object {
        [pscustomobject]@{name=$_.Groups[1].Value;version=$_.Groups[2].Value;path=$_.Groups[3].Value.Trim()}
    })
    $selected = $used | Where-Object { $_.name -eq 'M5Unit-NFC' }
    if (!$selected -or $selected.path.Replace('\','/').TrimEnd('/') -ine $library.Replace('\','/').TrimEnd('/')) {
        throw 'Verbose build output did not confirm selection of the project-local M5Unit-NFC library.'
    }
    $unified = $used | Where-Object { $_.name -eq 'M5UnitUnified' }
    if (!$unified -or [version]$unified.version -lt [version]'0.5.7') { throw 'M5UnitUnified >= 0.5.7 is required.' }
    $binary = Join-Path $output 'Emulation.ino.bin'
    $binaryText = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($binary))
    if (!$binaryText.Contains($manifest.build) -or !$binaryText.Contains($profileName)) {
        throw 'Firmware binary does not contain the requested build/profile identity.'
    }
    $options = Get-Content -Raw -LiteralPath (Join-Path $output 'build.options.json') | ConvertFrom-Json
    $manifest.status = 'passed'
    $manifest.used_libraries = $used
    $manifest.core_paths = @($options.hardwareFolders -split ',' | Sort-Object -Unique)
    $manifest.binary = $binary
    $manifest.binary_bytes = (Get-Item -LiteralPath $binary).Length
    $manifest.binary_sha256 = (Get-FileHash -LiteralPath $binary -Algorithm SHA256).Hash.ToLowerInvariant()
    $manifest.finished_utc = [DateTime]::UtcNow.ToString('o')
    Write-Manifest
    Write-Host "Firmware: $binary"
    Write-Host "Manifest: $manifestPath"
} catch {
    $manifest.status = 'failed'; $manifest.error = $_.Exception.Message
    Write-Manifest
    throw
}

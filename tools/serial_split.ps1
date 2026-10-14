# Splits a serial capture into one file per exported log.
#
# FlashLog::cat() brackets every export with:
#     ==== FILE /log/nfc-00000001.txt ====
#     ...log body...
#     ==== END NFC diagnostic ====
#
# Usage:
#   pwsh -File tools/serial_split.ps1 -In dump.txt
#   pwsh -File tools/serial_split.ps1 -In dump.txt -OutDir docs/baseline
#
# Output files are named after the log sequence number in the capture, so repeated
# captures never overwrite each other: the script refuses to clobber an existing file
# unless -Force is given.
#
# This script has not been executed in the environment that wrote it (PowerShell was
# unavailable there).

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$In,
    [string]$OutDir,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path $In)) { throw "capture file not found: $In" }
if (-not $OutDir) { $OutDir = Join-Path (Split-Path -Parent (Resolve-Path $In)) 'logs' }
if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Path $OutDir | Out-Null }

$beginPattern = '^==== FILE (?<path>/log/nfc-\d{8}\.txt) ====\s*$'
$endPattern   = '^==== END NFC diagnostic ====\s*$'

# Writes one extracted body next to the capture, named after its sequence number.
function Write-LogFile {
    param(
        [string]$Path,
        [System.Collections.Generic.List[string]]$Lines,
        [string]$OutDir,
        [switch]$Force
    )
    $name = [System.IO.Path]::GetFileNameWithoutExtension($Path) + '.txt'
    $target = Join-Path $OutDir $name
    if ((Test-Path $target) -and -not $Force) {
        Write-Host "skip (exists): $target" -ForegroundColor Yellow
        return
    }
    Set-Content -Path $target -Value $Lines -Encoding utf8
    Write-Host "wrote $target ($($Lines.Count) lines)" -ForegroundColor Green
}

$current = $null
$buffer = New-Object System.Collections.Generic.List[string]
$written = 0
$unterminated = 0

foreach ($line in Get-Content -Path $In) {
    if ($line -match $beginPattern) {
        if ($current) {
            # Previous export was never closed: keep what was captured.
            $unterminated++
            Write-LogFile -Path $current -Lines $buffer -OutDir $OutDir -Force:$Force
            $written++
        }
        $current = $Matches['path']
        $buffer.Clear()
        continue
    }
    if (-not $current) { continue }
    if ($line -match $endPattern) {
        Write-LogFile -Path $current -Lines $buffer -OutDir $OutDir -Force:$Force
        $written++
        $current = $null
        $buffer.Clear()
        continue
    }
    $buffer.Add($line)
}
if ($current) {
    $unterminated++
    Write-LogFile -Path $current -Lines $buffer -OutDir $OutDir -Force:$Force
    $written++
}

Write-Host ""
Write-Host "extracted $written log file(s) into $OutDir"
if ($unterminated) {
    Write-Host "$unterminated export(s) had no '==== END NFC diagnostic ====' marker;" -ForegroundColor Yellow
    Write-Host "the capture may have been cut short." -ForegroundColor Yellow
}
if ($written -eq 0) {
    Write-Host "nothing extracted. Check that the capture contains '==== FILE ... ====' lines," -ForegroundColor Yellow
    Write-Host "and that -In points at the raw capture rather than a single extracted log." -ForegroundColor Yellow
}

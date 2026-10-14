# Capture serial output at 115200 8N1. Commands are sent sequentially.
# logcat waits for its FILE/END markers; -ExitAfter is an idle timeout in ms,
# not a total transfer limit. -Seconds is an optional hard limit for the run.
# Examples:
#   pwsh -File tools/serial_capture.ps1 -Port COM5 -Command logcat -ExitAfter 3000 -Out dump.txt
#   pwsh -File tools/serial_capture.ps1 -Port COM5 -Out dump.txt
[CmdletBinding()]
param(
    [string]$Port,
    [ValidateRange(1, 4000000)][int]$Baud = 115200,
    [string]$Out,
    [string[]]$Command,
    [ValidateRange(0, 2147483647)][int]$ExitAfter = 0,
    [ValidateRange(0, 2147483647)][int]$Seconds = 0,
    [ValidateRange(1, 2147483647)][int]$ResponseTimeout = 30000
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'SerialCaptureState.ps1')
if (-not $Port) {
    $ports = [System.IO.Ports.SerialPort]::GetPortNames()
    if (-not $ports) { throw 'No serial port found. Pass -Port COMx.' }
    if ($ports.Count -gt 1) { Write-Host "multiple ports found: $($ports -join ', ')" }
    $Port = $ports[0]
    Write-Host "using port $Port (override with -Port)"
}

$queue = [System.Collections.Generic.Queue[string]]::new()
foreach ($line in $Command) {
    if ([string]::IsNullOrWhiteSpace($line) -or $line.Contains("`r") -or $line.Contains("`n") -or $line.Length -gt 63) {
        throw 'Each command must be one nonempty line of at most 63 characters.'
    }
    $queue.Enqueue($line)
}
$sp = [System.IO.Ports.SerialPort]::new($Port, $Baud, 'None', 8, 'One')
$sp.WriteTimeout = 1000
$writer = $null
$clock = [System.Diagnostics.Stopwatch]::StartNew()
$active = $null
$typed = ''
$canReadKeys = -not [Console]::IsInputRedirected
$idleTimeout = if ($ExitAfter -gt 0) { $ExitAfter } else { 3000 }
$sendAfter = 150L

try {
    if ($Out) {
        $dir = Split-Path -Parent $Out
        if ($dir -and -not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Path $dir | Out-Null }
        $writer = [System.IO.StreamWriter]::new($Out, $false)
        Write-Host "capturing to $Out"
    }
    $sp.Open()
    Write-Host "opened $Port @ $Baud. Ctrl+C to stop."
    while ($true) {
        $now = $clock.ElapsedMilliseconds
        # Drain input before checking timeouts so already-buffered bytes are not lost.
        $chunk = $sp.ReadExisting()
        if ($chunk) {
            Write-Host -NoNewline $chunk
            if ($writer) { $writer.Write($chunk) }
            if ($active) { Update-SerialResponseState $active $chunk $now }
        }
        if ($active) {
            $status = Get-SerialResponseStatus $active $now
            if ($status -eq 'failed') { throw "$($active.Command): $($active.Failure)" }
            if ($status -eq 'complete') {
                $active = $null
                $sendAfter = $now + 150
                if ($ExitAfter -gt 0 -and $queue.Count -eq 0) { break }
            }
        }
        if ($Seconds -gt 0 -and $now -ge ([long]$Seconds * 1000)) {
            if ($active -or $queue.Count) { throw 'Time limit reached with pending commands; capture may be incomplete.' }
            break
        }
        # Read at most 64 keys per iteration. Never block serial draining on Enter.
        if ($canReadKeys) {
            for ($i = 0; $i -lt 64 -and [Console]::KeyAvailable; $i++) {
                $key = [Console]::ReadKey($true)
                if ($key.Key -eq 'Enter') {
                    Write-Host ''
                    if ($typed.Length) { $queue.Enqueue($typed); $typed = '' }
                } elseif ($key.Key -eq 'Backspace') {
                    if ($typed.Length) { $typed = $typed.Substring(0, $typed.Length - 1); Write-Host -NoNewline "`b `b" }
                } elseif (-not [char]::IsControl($key.KeyChar) -and $typed.Length -lt 63) {
                    $typed += $key.KeyChar
                    Write-Host -NoNewline $key.KeyChar
                }
            }
        }
        if (-not $active -and $queue.Count -gt 0 -and $now -ge $sendAfter) {
            $line = $queue.Dequeue()
            $sp.Write("$line`r`n")
            Write-Host ">>> $line" -ForegroundColor Cyan
            $active = New-SerialResponseState $line $now $idleTimeout $ResponseTimeout
        }
        Start-Sleep -Milliseconds 10
    }
}
finally {
    $sp.Dispose()
    if ($writer) { $writer.Dispose(); Write-Host "capture file: $Out" }
}

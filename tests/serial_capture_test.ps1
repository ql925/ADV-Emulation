$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../tools/SerialCaptureState.ps1')
function Assert-Status($state, [long]$now, [string]$expected) {
    $actual = Get-SerialResponseStatus $state $now
    if ($actual -ne $expected) { throw "Expected $expected at $now, got $actual" }
}

# The reader may stay busy beyond ExitAfter; boot noise is not a FILE response.
$s = New-SerialResponseState 'logcat' 0 3000 30000
Update-SerialResponseState $s "boot`n" 500
Assert-Status $s 5000 waiting
Update-SerialResponseState $s '==== FI' 6000
Update-SerialResponseState $s "LE /log/nfc-00000001.txt ====`r" 6100
Update-SerialResponseState $s "`nbody`n" 6200
Assert-Status $s 7000 waiting
# A long transfer remains valid while data keeps arriving (well beyond 30 s).
for ($t = 8000; $t -le 40000; $t += 2000) {
    Update-SerialResponseState $s "body`n" $t
    Assert-Status $s $t waiting
}
Update-SerialResponseState $s '==== END NFC diag' 41000
Assert-Status $s 41000 waiting
Update-SerialResponseState $s "nostic ====`n" 42000
Assert-Status $s 42000 complete

$s = New-SerialResponseState 'logcat' 0 3000 30000
Update-SerialResponseState $s "==== END NFC diagnostic ====`n" 100
Assert-Status $s 5000 waiting
Assert-Status $s 30000 failed

$s = New-SerialResponseState 'logcat nfc-00000002.txt' 0 3000 30000
Update-SerialResponseState $s "==== FILE /log/nfc-00000002.txt ====`npartial" 100
Assert-Status $s 3099 waiting
Assert-Status $s 3100 failed

foreach ($errorLine in @('Log not found', 'FLASH_LOG=unmountable', 'Use: logs or logcat nfc-00000001.txt')) {
    $s = New-SerialResponseState 'logcat' 0 3000 30000
    Update-SerialResponseState $s "$errorLine`n" 100
    Assert-Status $s 100 failed
}
$s = New-SerialResponseState 'logs' 0 3000 30000
Assert-Status $s 5000 waiting
Update-SerialResponseState $s "FLASH_LOG=saved`n" 6000
Assert-Status $s 8999 waiting
Assert-Status $s 9000 complete
$s = New-SerialResponseState 'logs' 0 3000 30000
Assert-Status $s 30000 failed

# Parse the actual entry script too, without opening a serial device.
$tokens = $null; $errors = $null
[void][System.Management.Automation.Language.Parser]::ParseFile(
    (Join-Path $PSScriptRoot '../tools/serial_capture.ps1'), [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw ($errors | Out-String) }
Write-Host 'serial capture tests passed'

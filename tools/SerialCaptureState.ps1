# Pure response tracking, shared by the serial loop and hardware-free tests.
function New-SerialResponseState {
    param([string]$Command, [long]$Now, [int]$IdleTimeout, [int]$ResponseTimeout)
    [pscustomobject]@{
        Command = $Command
        IsLogcat = $Command -cmatch '^logcat(?: |$)'
        Started = $Now
        LastData = $Now
        IdleTimeout = $IdleTimeout
        ResponseTimeout = $ResponseTimeout
        SawData = $false
        InExport = $false
        Complete = $false
        Failure = $null
        LineBuffer = ''
    }
}

function Update-SerialResponseState {
    param($State, [string]$Chunk, [long]$Now)
    if (-not $Chunk) { return }
    $State.SawData = $true
    $State.LastData = $Now
    if (-not $State.IsLogcat) { return }
    $parts = ($State.LineBuffer + $Chunk).Replace("`r", "`n").Split("`n")
    $State.LineBuffer = $parts[-1]
    if ($State.LineBuffer.Length -gt 8192) { $State.Failure = 'Response line exceeds 8192 characters' }
    for ($i = 0; $i -lt $parts.Length - 1; $i++) {
        $line = $parts[$i]
        if ($line -cmatch '^==== FILE /log/nfc-\d{8}\.txt ====\s*$') {
            $State.InExport = $true
        } elseif ($State.InExport -and $line -cmatch '^==== END NFC diagnostic ====\s*$') {
            $State.Complete = $true
        } elseif (-not $State.InExport -and $line -cmatch '^(Log not found|Use:|FLASH_LOG=|Command too long|Commands:)') {
            $State.Failure = $line
        }
    }
}

function Get-SerialResponseStatus {
    param($State, [long]$Now)
    if ($State.Failure) { return 'failed' }
    if ($State.Complete) { return 'complete' }
    if ($State.SawData -and $Now - $State.LastData -ge $State.IdleTimeout) {
        if (-not $State.IsLogcat) { return 'complete' }
        # Before FILE, unrelated boot/RF output is not a response.
        if ($State.InExport) {
            $State.Failure = 'Log export stalled before its END marker; capture is incomplete'
            return 'failed'
        }
    }
    if (((-not $State.IsLogcat -and -not $State.SawData) -or ($State.IsLogcat -and -not $State.InExport)) -and
        $Now - $State.Started -ge $State.ResponseTimeout) {
        $State.Failure = 'Timed out waiting for a command response'
        return 'failed'
    }
    return 'waiting'
}

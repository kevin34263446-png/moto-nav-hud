# Run the separately built UI-stall image, then always restore the normal image.
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$Programmer,
    [Parameter(Mandatory=$true)][string]$SerialNumber,
    [string]$BuildRoot = (Join-Path $PSScriptRoot '..\firmware\build_cli')
)
$ErrorActionPreference = 'Stop'
$normal = (Resolve-Path (Join-Path $BuildRoot 'app.elf')).Path
$test = (Resolve-Path (Join-Path $BuildRoot 'watchdog_selftest\app.elf')).Path
$log = Join-Path $BuildRoot 'rtos-watchdog.log'
if (-not (Test-Path -LiteralPath $Programmer)) { throw 'Programmer executable missing' }
function Read-Counter {
    $lines = & $Programmer -c port=SWD "sn=$SerialNumber" mode=HOTPLUG -r32 0x40002850 4
    if ($LASTEXITCODE) { throw 'Boot counter read failed' }
    $lines | Add-Content $log
    foreach ($line in $lines) {
        if ($line -match '^0x40002850\s*:\s*([0-9a-fA-F]{8})') {
            return [Convert]::ToUInt32($Matches[1], 16)
        }
    }
    throw 'Boot counter could not be parsed'
}
try {
    & $Programmer -c port=SWD "sn=$SerialNumber" -d $test -v -rst | Tee-Object -FilePath $log
    if ($LASTEXITCODE) { throw 'Self-test flash failed' }
    Start-Sleep -Seconds 2
    $before = Read-Counter
    Write-Host "Boot count before UI stall: $before"
    Start-Sleep -Seconds 12
    $after = Read-Counter
    Write-Host "Boot count after UI stall: $after"
    if ($after -le $before) { throw 'No watchdog reboot observed' }
    "PASS: UI stall caused reboot, counter $before -> $after" | Tee-Object -FilePath $log -Append
} finally {
    & $Programmer -c port=SWD "sn=$SerialNumber" -d $normal -v -rst | Tee-Object -FilePath $log -Append
    if ($LASTEXITCODE) { throw 'CRITICAL: restoring normal firmware failed' }
    Write-Host 'Normal firmware restored and verified.'
}

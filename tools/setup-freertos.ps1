[CmdletBinding()]
param([string]$Destination = (Join-Path $PSScriptRoot '..\..\deps\FreeRTOS-Kernel'))
$ErrorActionPreference = 'Stop'
$expected = 'dbf70559b27d39c1fdb68dfb9a32140b6a6777a0'
if (-not (Test-Path -LiteralPath $Destination)) {
    git clone --depth 1 --branch V11.1.0 https://github.com/FreeRTOS/FreeRTOS-Kernel.git $Destination
    if ($LASTEXITCODE -ne 0) { throw 'FreeRTOS clone failed' }
}
$actual = git -C $Destination rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or $actual -ne $expected) {
    throw "Expected FreeRTOS V11.1.0 commit $expected; found $actual. Existing files were not changed."
}
Write-Host "FreeRTOS V11.1.0 ready: $Destination"

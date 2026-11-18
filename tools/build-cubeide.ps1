[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$CubeIDE,
    [ValidateSet('Debug-BLE','Debug-VCP','Release-BLE','Release-VCP')]
    [string[]]$Configuration = @('Debug-BLE'),
    [string]$Workspace = (Join-Path $PSScriptRoot '..\..\ide-validation-workspace')
)
$ErrorActionPreference = 'Stop'
$fw = (Resolve-Path (Join-Path $PSScriptRoot '..\firmware')).Path
$launcher = Join-Path $CubeIDE 'stm32cubeidec.exe'
if (-not (Test-Path -LiteralPath $launcher)) { throw "Missing launcher: $launcher" }
$logRoot = Join-Path $fw 'build_cli'
New-Item -ItemType Directory -Force -Path $logRoot | Out-Null
$log = Join-Path $logRoot ('cubeide-' + ($Configuration -join '_') + '.log')
$arguments = @('--launcher.suppressErrors', '-nosplash', '-application',
    'org.eclipse.cdt.managedbuilder.core.headlessbuild', '-data', $Workspace,
    '-no-indexer', '-import', $fw, '-printErrorMarkers')
foreach ($config in $Configuration) { $arguments += @('-build', "moto-nav-hud/$config") }
& $launcher @arguments *> $log
$buildExit = $LASTEXITCODE
$logText = Get-Content -LiteralPath $log -Raw
Get-Content -LiteralPath $log | Select-String 'Build Finished|Headless build completed|error:|warning:'
if ($buildExit -ne 0) {
    Get-Content -LiteralPath $log -Tail 25
    throw "CubeIDE failed (exit $buildExit). Full log: $log"
}
foreach ($config in $Configuration) {
    if (-not $logText.Contains("Build of configuration $config for project moto-nav-hud")) {
        throw "CubeIDE did not build requested configuration $config. See $log"
    }
    $elf = Join-Path $fw "$config\moto-nav-hud.elf"
    if (-not (Test-Path -LiteralPath $elf)) { throw "No ELF produced for $config. See $log" }
    Write-Host "Built: $elf"
}
Write-Host "Log: $log"

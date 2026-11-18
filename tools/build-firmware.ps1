# Build repository firmware using local HAL/CMSIS and LVGL dependencies.
[CmdletBinding()]
param(
    [string]$ToolchainBin,
    [string]$DependenciesRoot = (Join-Path $PSScriptRoot '..\..\ws\moto-nav-hud-fw'),
    [string]$FreeRTOSRoot = (Join-Path $PSScriptRoot '..\..\deps\FreeRTOS-Kernel'),
    [switch]$WatchdogSelfTest,
    [ValidateSet('vcp', 'ble')][string]$NavSource = 'vcp'
)
$ErrorActionPreference = 'Stop'
$fw = (Resolve-Path (Join-Path $PSScriptRoot '..\firmware')).Path
if (-not $ToolchainBin) {
    $compiler = Get-Command arm-none-eabi-gcc -ErrorAction SilentlyContinue
    if ($compiler) { $ToolchainBin = Split-Path $compiler.Source }
    else { throw 'Specify -ToolchainBin pointing to the Arm GNU tools/bin directory.' }
}
$gcc = Join-Path $ToolchainBin 'arm-none-eabi-gcc.exe'
$size = Join-Path $ToolchainBin 'arm-none-eabi-size.exe'
$deps = (Resolve-Path $DependenciesRoot).Path
$drivers = Join-Path $deps 'Drivers'
$lvgl = Join-Path $deps 'lvgl'
$rtos = (Resolve-Path $FreeRTOSRoot).Path
foreach ($required in @($gcc, $size, "$drivers\STM32F4xx_HAL_Driver\Src\stm32f4xx_hal.c", "$lvgl\lvgl.h", "$rtos\include\FreeRTOS.h")) {
    if (-not (Test-Path -LiteralPath $required)) { throw "Missing dependency: $required" }
}
$build = Join-Path $fw 'build_cli'
New-Item -ItemType Directory -Force -Path $build | Out-Null
if ($NavSource -eq 'ble') {
    $build = Join-Path $build 'ble'
    New-Item -ItemType Directory -Force -Path $build | Out-Null
}
if ($WatchdogSelfTest) {
    $build = Join-Path $build 'watchdog_selftest'
    New-Item -ItemType Directory -Force -Path $build | Out-Null
}
$common = @('-mcpu=cortex-m4', '-mthumb', '-mfpu=fpv4-sp-d16', '-mfloat-abi=hard',
    '-Os', '-g3', '-ffunction-sections', '-fdata-sections', '-Wall',
    '-DSTM32F446xx', '-DUSE_HAL_DRIVER', '-DLV_CONF_INCLUDE_SIMPLE',
    "-I$fw\Inc", "-I$fw", "-I$deps", "-I$lvgl", "-I$rtos\include", "-I$rtos\portable\GCC\ARM_CM4F",
    "-I$drivers\STM32F4xx_HAL_Driver\Inc", "-I$drivers\CMSIS\Device\ST\STM32F4xx\Include",
    "-I$drivers\CMSIS\Core\Include")
if ($WatchdogSelfTest) { $common += '-DWDG_SELFTEST' }
if ($NavSource -eq 'ble') { $common += '-DNAV_USE_BLE=1' }
# Compile only the HAL modules enabled by this project's hal_conf.h.
$sourceManifest = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'firmware-sources.json') -Raw | ConvertFrom-Json
$sources = @(Get-ChildItem -LiteralPath "$fw\Src" -Filter '*.c' -File | ForEach-Object FullName)
$sources += @($sourceManifest.hal | ForEach-Object { Join-Path "$drivers\STM32F4xx_HAL_Driver\Src" $_ })
$sources += @(Get-ChildItem -LiteralPath "$lvgl\src" -Filter '*.c' -File -Recurse |
    Sort-Object FullName | ForEach-Object FullName)
$sources += "$fw\Startup\startup_stm32f446xx.s"
$sources += @($sourceManifest.freertos) |
    ForEach-Object { Join-Path $rtos $_ }
$objects = @()
$version = & $gcc --version
if ($LASTEXITCODE -ne 0) { throw 'Cannot execute the Arm compiler.' }
Write-Host $version[0]
Write-Host "Compiling $($sources.Count) sources from repository firmware and $deps"
foreach ($source in $sources) {
    # Numbered objects avoid duplicate basenames across LVGL directories.
    $obj = Join-Path $build ('{0:D4}_{1}.o' -f $objects.Count, [IO.Path]::GetFileNameWithoutExtension($source))
    $language = @(if ([IO.Path]::GetExtension($source) -eq '.s') {
        @('-x', 'assembler-with-cpp')
    } else { @('-std=c99') })
    & $gcc @common @language '-c' $source '-o' $obj
    if ($LASTEXITCODE -ne 0) { throw "Compilation failed: $source" }
    $objects += $obj
}
$response = Join-Path $build 'objects.rsp'
$objects | ForEach-Object { '"' + $_.Replace('\', '/') + '"' } |
    Set-Content -LiteralPath $response -Encoding ascii
$elf = Join-Path $build 'app.elf'
$map = Join-Path $build 'app.map'
& $gcc @common "@$response" "-T$fw\STM32F446RETX_FLASH.ld" '--specs=nano.specs' '--specs=nosys.specs' `
    '-Wl,--gc-sections' "-Wl,-Map=$map" '-Wl,--print-memory-usage' '-o' $elf `
    '-Wl,--start-group' '-lc' '-lm' '-Wl,--end-group'
if ($LASTEXITCODE -ne 0) { throw 'Firmware link failed.' }
& $size $elf
if ($LASTEXITCODE -ne 0) { throw 'Firmware size inspection failed.' }
Write-Host "Built: $elf"

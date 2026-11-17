# tools\setup-deps.ps1 —— 恢复固件工程的两大外部依赖
#
# 为什么需要这个脚本：HAL 驱动（约 40MB）和 LVGL（约 15MB）不适合放进 git 仓库，
# 所以仓库里只有我们自己的代码。克隆仓库后先跑一次本脚本，工程就能编译。
#
# 用法：
#   .\tools\setup-deps.ps1              # 两个依赖都准备
#   .\tools\setup-deps.ps1 -DriversOnly
#   .\tools\setup-deps.ps1 -LvglOnly
$ErrorActionPreference = 'Stop'

param(
    [switch]$DriversOnly,
    [switch]$LvglOnly
)

$here = $PSScriptRoot
$fw   = Join-Path $here '..\firmware'

# STM32CubeF4 固件包的默认安装位置（STM32CubeProgrammer / CubeIDE 安装时会放这里）
$pkgCandidates = @(
    "$env:USERPROFILE\STM32Cube\Repository\STM32Cube_FW_F4_V1.28.3",
    "$env:USERPROFILE\STM32Cube\Repository\STM32Cube_FW_F4_V1.28.0"
)

function Setup-Drivers {
    $pkg = $pkgCandidates | Where-Object { Test-Path (Join-Path $_ 'Drivers\STM32F4xx_HAL_Driver\Src') } | Select-Object -First 1
    if (-not $pkg) {
        Write-Host "找不到 STM32CubeF4 固件包。" -ForegroundColor Red
        Write-Host "请从 https://github.com/STMicroelectronics/STM32CubeF4 克隆（注意要 --recurse-submodules）"
        Write-Host "并放到 $env:USERPROFILE\STM32Cube\Repository\STM32Cube_FW_F4_V1.28.3"
        return $false
    }
    New-Item -ItemType Directory -Force -Path (Join-Path $fw 'Drivers') | Out-Null
    Copy-Item -Path (Join-Path $pkg 'Drivers\STM32F4xx_HAL_Driver') -Destination (Join-Path $fw 'Drivers') -Recurse -Force
    Copy-Item -Path (Join-Path $pkg 'Drivers\CMSIS')               -Destination (Join-Path $fw 'Drivers') -Recurse -Force
    # 这几个 *_template.c 会造成重复定义（CubeMX 本来也不会复制它们）
    Get-ChildItem (Join-Path $fw 'Drivers\STM32F4xx_HAL_Driver\Src') -Filter '*_template.c' | Remove-Item -Force
    Write-Host "HAL 驱动已就绪" -ForegroundColor Green
    return $true
}

function Setup-Lvgl {
    if (Test-Path (Join-Path $fw 'lvgl\lvgl.h')) {
        Write-Host "LVGL 已存在，跳过" -ForegroundColor Yellow
        return $true
    }
    $env:GIT_TERMINAL_PROMPT = '0'
    Push-Location $fw
    git clone --depth 1 --branch v8.3.11 https://github.com/lvgl/lvgl.git lvgl
    $ok = ($LASTEXITCODE -eq 0)
    Pop-Location
    if ($ok) {
        foreach ($d in @('.git','docs','examples','demos','tests','scripts','zephyr','env_support','ci','.github')) {
            $t = Join-Path $fw "lvgl\$d"
            if (Test-Path $t) { Remove-Item $t -Recurse -Force -ErrorAction SilentlyContinue }
        }
        Write-Host "LVGL v8.3.11 已就绪" -ForegroundColor Green
    }
    return $ok
}

$okAll = $true
if (-not $LvglOnly)    { $okAll = (Setup-Drivers) -and $okAll }
if (-not $DriversOnly) { $okAll = (Setup-Lvgl)    -and $okAll }

if ($okAll) { Write-Host "`n依赖准备完成，可以导入 CubeIDE 或直接编译了" -ForegroundColor Green }
else        { Write-Host "`n部分依赖未就绪，见上方提示" -ForegroundColor Red; exit 1 }
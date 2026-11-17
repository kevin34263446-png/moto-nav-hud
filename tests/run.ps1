# tests\run.ps1 —— 编译并运行协议解析单元测试（宿主机，不需要硬件）
#   用法：  .\tests\run.ps1
#   改动 firmware/Src/proto_parser.c 或 firmware/Inc/proto.h 之后必须先跑这个。
#   红了不许烧板子。
$ErrorActionPreference = 'Stop'

$here = $PSScriptRoot
$fw   = Join-Path $here '..\firmware'
$inc  = Join-Path $fw 'Inc'
$bin  = Join-Path $here 'test_proto.exe'

$gcc = Get-Command gcc -ErrorAction SilentlyContinue
if (-not $gcc) {
    Write-Host '找不到 gcc。装 MinGW-w64 或 MSYS2，确保 gcc 在 PATH 里。' -ForegroundColor Red
    exit 2
}

& $gcc.Source -std=c99 -Wall -Wextra -Werror -O2 `
    -I $inc `
    -o $bin (Join-Path $here 'test_proto.c') (Join-Path $fw 'Src\proto_parser.c')

if ($LASTEXITCODE -ne 0) {
    Write-Host '编译失败' -ForegroundColor Red
    exit 1
}

& $bin
exit $LASTEXITCODE
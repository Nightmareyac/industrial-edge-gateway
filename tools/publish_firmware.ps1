<#
.SYNOPSIS
    OTA 固件发布：校验镜像 -> 覆盖固定入口 -> 归档 -> 服务端核对

.DESCRIPTION
    把编译产物发布成"设备下次 upgrade 就能拿到的那份固件"。

    做的事按顺序：
      1. 校验向量表（SP / Reset 向量）—— 拦住按 0x08000000 链接的镜像，
         那种镜像推上去 Bootloader 校验不过，现象就是白屏
      2. 校验大小不超过 APP 分区容量
      3. 核对 bin 里内嵌的版本字符串，防止把上一版当新版推出去
      4. 原子替换 ota_release\A_Project.bin（先写 .tmp 再 rename，
         避免设备正好在下载时读到半个文件）
      5. 归档一份带版本号的副本到 ota_release\archive\
      6. 若服务在运行，拉 /health 核对服务端看到的 size / md5

    为什么入口固定叫 A_Project.bin：
    设备端固件里写死的下载路径是 /A_Project.bin。只要服务端换文件，
    设备下次升级就拿新的，永远不用改设备代码、不用重烧基线。
    这就是"推送"和"重新烧录"的区别。

.PARAMETER Version
    要发布的版本号，如 3.1。会去 bin 里找 "Edge Gateway V3.1" 做核对。

.PARAMETER Source
    编译产物路径，默认 MDK-ARM\build\A_Project\A_Project.bin

.PARAMETER ReleaseDir
    发布目录，默认工程根下的 ota_release

.PARAMETER Port
    服务端口，用于 /health 核对，默认 80

.PARAMETER DryRun
    只做校验，不做任何文件改动。发布前想先确认镜像是否合法时用

.PARAMETER SkipVersionCheck
    跳过版本字符串核对（仅在故意发布特殊镜像时使用）

.EXAMPLE
    .\publish_firmware.ps1 -Version 3.1 -DryRun

.EXAMPLE
    .\publish_firmware.ps1 -Version 3.1
#>

param(
    [Parameter(Mandatory = $true)]
    [string]$Version,

    [string]$Source = '',

    [string]$ReleaseDir = '',

    [int]$Port = 80,

    [switch]$DryRun,

    [switch]$SkipVersionCheck
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

# APP 分区容量：0x08010000 ~ 0x080FFFFF，共 0xF0000 = 983040 字节
$AppAreaStart = 0x08010000
$AppAreaEnd   = 0x080FFFFF
$AppAreaSize  = 0xF0000

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjRoot  = Split-Path -Parent $ScriptDir

if (-not $Source)     { $Source     = Join-Path $ProjRoot 'MDK-ARM\build\A_Project\A_Project.bin' }
if (-not $ReleaseDir) { $ReleaseDir = Join-Path $ProjRoot 'ota_release' }

# 允许传 "3.1" 或 "V3.1"
$ver = $Version.Trim()
$ver = $ver -replace '^[Vv]', ''
$verTag = 'V' + $ver


function Write-Step($text) {
    Write-Host ''
    Write-Host ("[步骤] " + $text) -ForegroundColor Cyan
}

function Fail($text) {
    Write-Host ''
    Write-Host "[失败] $text" -ForegroundColor Red
    exit 1
}


# ---------------------------------------------------------------
# 1. 源文件
# ---------------------------------------------------------------
Write-Step "检查编译产物"
if (-not (Test-Path $Source)) {
    Fail "找不到编译产物：$Source`n       先在 EIDE 里编译一次，或用 -Source 指定路径"
}
Write-Host ("  " + $Source)
Write-Host ("  {0} 字节   修改时间 {1}" -f (Get-Item $Source).Length, (Get-Item $Source).LastWriteTime)


# ---------------------------------------------------------------
# 2. 向量表校验（防白屏的核心）
# ---------------------------------------------------------------
Write-Step "校验向量表"

$bytes = [System.IO.File]::ReadAllBytes($Source)
if ($bytes.Length -lt 8) {
    Fail "文件只有 $($bytes.Length) 字节，不是合法的镜像"
}

$sp    = [BitConverter]::ToUInt32($bytes, 0)
$reset = [BitConverter]::ToUInt32($bytes, 4)

Write-Host ("  初始 SP  = 0x{0:X8}" -f $sp)
Write-Host ("  Reset    = 0x{0:X8}" -f $reset)

# SP 必须落在 SRAM(0x20000000-0x20020000) 或 CCM(0x10000000-0x10010000)
$spOk = (($sp -ge 0x20000000) -and ($sp -le 0x20020000)) -or
        (($sp -ge 0x10000000) -and ($sp -le 0x10010000))
if (-not $spOk) {
    Fail ("初始 SP = 0x{0:X8} 非法，不像有效的 ARM 镜像（RAM 地址应在 0x2000xxxx 或 0x1000xxxx）" -f $sp)
}

# 最关键的一条：Reset 向量必须落在 APP 区
if (($reset -ge 0x08000000) -and ($reset -lt $AppAreaStart)) {
    $msg = ("Reset 向量 = 0x{0:X8} 落在 **Bootloader 区**！" -f $reset) +
           "`n       这说明镜像是按 0x08000000 链接的，推上去必然白屏。" +
           "`n       检查两处：" +
           "`n         1. MDK-ARM\.eide\eide.yml 里的内存起始地址" +
           "`n         2. 链接脚本 / scatter 文件的 ROM 起始地址" +
           "`n       都应该是 0x08010000（APP 区），不是 0x08000000"
    Fail $msg
}
if (($reset -lt $AppAreaStart) -or ($reset -gt $AppAreaEnd)) {
    Fail ("Reset 向量 = 0x{0:X8} 不在 APP 区 0x{1:X8}-0x{2:X8}" -f $reset, $AppAreaStart, $AppAreaEnd)
}
Write-Host "  向量表合法：Reset 落在 APP 区" -ForegroundColor Green


# ---------------------------------------------------------------
# 3. 容量
# ---------------------------------------------------------------
Write-Step "校验容量"
if ($bytes.Length -gt $AppAreaSize) {
    Fail ("镜像 {0} 字节，超过 APP 分区容量 {1} 字节（0xF0000）" -f $bytes.Length, $AppAreaSize)
}
Write-Host ("  {0} / {1} 字节，余量 {2} 字节" -f $bytes.Length, $AppAreaSize, ($AppAreaSize - $bytes.Length))


# ---------------------------------------------------------------
# 4. 版本字符串核对
# ---------------------------------------------------------------
Write-Step "核对内嵌版本"
$txt = [System.Text.Encoding]::ASCII.GetString($bytes)
$found = @([regex]::Matches($txt, 'Edge Gateway V[0-9]+\.[0-9]+') |
           ForEach-Object { $_.Value } | Select-Object -Unique)

$expect = "Edge Gateway $verTag"
if ($found.Count -eq 0) {
    if ($SkipVersionCheck) {
        Write-Host "  bin 里没找到版本字符串，已按 -SkipVersionCheck 跳过" -ForegroundColor Yellow
    }
    else {
        Fail ("bin 里找不到 'Edge Gateway Vx.y' 字符串，无法确认版本。`n" +
              "       可能是 APP_VERSION_STR 被改动过，或编译产物不是这个工程生成的")
    }
}
else {
    Write-Host ("  找到：" + ($found -join ', '))
    if ($found -notcontains $expect) {
        if ($SkipVersionCheck) {
            Write-Host ("  预期 $expect 但没找到，已按 -SkipVersionCheck 跳过") -ForegroundColor Yellow
        }
        else {
            Fail ("版本不匹配：预期 '$expect'，实际是 '$($found -join ', ')'`n" +
                  "       要么改了 Core\Inc\app_config.h 里的 APP_VERSION_STR 但没重新编译，`n" +
                  "       要么 -Version 传错了。确认无误可加 -SkipVersionCheck")
        }
    }
    else {
        Write-Host "  版本一致" -ForegroundColor Green
    }
}


# ---------------------------------------------------------------
# 5. 写入发布目录
# ---------------------------------------------------------------
$target = Join-Path $ReleaseDir 'A_Project.bin'
$archiveDir = Join-Path $ReleaseDir 'archive'
$archive = Join-Path $archiveDir ("A_Project_v{0}.bin" -f $ver)

if ($DryRun) {
    Write-Step "DryRun：跳过写入"
    Write-Host ("  将覆盖 {0}" -f $target)
    Write-Host ("  将归档 {0}" -f $archive)
    Write-Host ''
    Write-Host "[OK] 校验全部通过，去掉 -DryRun 即可正式发布" -ForegroundColor Green
    exit 0
}

Write-Step "写入发布目录"
if (-not (Test-Path $ReleaseDir)) {
    New-Item -ItemType Directory -Path $ReleaseDir -Force | Out-Null
}
if (-not (Test-Path $archiveDir)) {
    New-Item -ItemType Directory -Path $archiveDir -Force | Out-Null
}

# 原子替换：先写同目录临时文件，再 rename。
# 直接 Copy-Item 覆盖时，如果设备正好在下载，会读到"前一半旧、后一半新"的文件
$tmp = $target + '.tmp'
Copy-Item $Source $tmp -Force
Move-Item $tmp $target -Force
Write-Host ("  {0}  <- {1}" -f $target, (Split-Path $Source -Leaf))

Copy-Item $target $archive -Force
Write-Host ("  {0}  (归档)" -f $archive)


# ---------------------------------------------------------------
# 6. 本地与服务端核对
# ---------------------------------------------------------------
Write-Step "核对结果"
$localMd5 = (Get-FileHash $target -Algorithm MD5).Hash.ToLower()
$localSize = (Get-Item $target).Length
Write-Host ("  本地  {0} 字节  md5={1}" -f $localSize, $localMd5)

try {
    $resp = Invoke-WebRequest -UseBasicParsing -Uri ("http://127.0.0.1:{0}/health" -f $Port) -TimeoutSec 5
    $health = $resp.Content | ConvertFrom-Json
    $item = $health.firmwares | Where-Object { $_.name -eq 'A_Project.bin' }

    if (-not $item) {
        Write-Host "  [警告] 服务端 /health 里没列出 A_Project.bin" -ForegroundColor Yellow
    }
    elseif ($item.md5 -ne $localMd5) {
        Write-Host ("  [警告] 服务端 md5={0}，与本地不一致" -f $item.md5) -ForegroundColor Yellow
    }
    else {
        Write-Host ("  服务端 {0} 字节  md5 一致，已可下载" -f $item.size) -ForegroundColor Green
    }
}
catch {
    Write-Host ("  [提示] 服务未响应（{0}），本地文件已就位，服务起来后即可下载" -f $_.Exception.Message) -ForegroundColor Yellow
}

Write-Host ''
Write-Host "================== 下一步 ==================" -ForegroundColor Cyan
Write-Host "  固件已发布：$verTag（$localSize 字节）"
Write-Host "  1. 确认设备 MQTT 在线"
Write-Host "  2. 巴法云给设备主题发 upgrade"
Write-Host ("  3. 屏幕出现 d:xxxxx/{0} 递增，重启后标题变为 {1}" -f $localSize, $verTag)
Write-Host "  4. 回查日志：powershell -File tools\ota_service.ps1 -Action Log -Tail 20"
Write-Host "===========================================" -ForegroundColor Cyan

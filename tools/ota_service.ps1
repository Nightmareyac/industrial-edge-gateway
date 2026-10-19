<#
.SYNOPSIS
    OTA 固件服务管理（安装 / 启停 / 状态 / 日志）

.DESCRIPTION
    管理一个常驻的 OTA 固件 HTTP 服务。服务由 Windows 计划任务承载，
    以 SYSTEM 身份、开机自动启动、无窗口后台运行，因此：

      - 关掉终端 / 退出 IDE / 注销当前用户，服务都不受影响
      - 不需要登录也能跑（设备半夜重启照样能升级）
      - 进程意外退出后 1 分钟自动重启（最多 3 次）

    为什么不用 `python -m http.server`：
      那个是前台进程，绑在当前控制台上，窗口一关就没了；
      而且它不写日志文件，OTA 失败时无法回查"服务器到底收没收到请求"。

.PARAMETER Action
    Install    注册计划任务并立即启动（需管理员）
    Uninstall  停止并删除计划任务（需管理员）
    Start      启动服务
    Stop       停止服务
    Restart    重启服务
    Status     查看任务状态 + 端口监听 + /health 自检
    Log        查看访问日志末尾

.EXAMPLE
    .\ota_service.ps1 Status
    .\ota_service.ps1 Restart
    .\ota_service.ps1 Log -Tail 50
#>

param(
    [ValidateSet('Install', 'Uninstall', 'Start', 'Stop', 'Restart', 'Status', 'Log')]
    [string]$Action = 'Status',

    # HTTP 端口。设备端代码里写的是 80，改这里要同步改固件
    [int]$Port = 80,

    # 固件目录。发布新版本就是把新 bin 放进这个目录
    [string]$FirmwareRoot = 'D:\A_Project\ota_release',

    # Log 动作显示的行数
    [int]$Tail = 20
)

$ErrorActionPreference = 'Stop'

# 关掉 Invoke-WebRequest 的进度条，否则会和状态输出挤在同一行里
$ProgressPreference = 'SilentlyContinue'

$TaskName  = 'OTA_Firmware_Server'
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ServerPy  = Join-Path $ScriptDir 'ota_server.py'
$LogPath   = Join-Path $ScriptDir 'ota_server.log'


function Test-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $pr = New-Object Security.Principal.WindowsPrincipal($id)
    return $pr.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}


function Find-Pythonw {
    # 优先按版本从新到旧找固定路径；以 SYSTEM 身份运行时 PATH 里
    # 没有当前用户的 AppData 目录，只靠 `Get-Command python` 会找不到
    $candidates = @(
        "$env:LOCALAPPDATA\Programs\Python\Python313\pythonw.exe",
        "$env:LOCALAPPDATA\Programs\Python\Python312\pythonw.exe",
        "$env:LOCALAPPDATA\Programs\Python\Python311\pythonw.exe",
        'C:\Python313\pythonw.exe'
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) { return $c }
    }

    $py = Get-Command python.exe -ErrorAction SilentlyContinue
    if ($py) {
        $pyw = Join-Path (Split-Path $py.Source) 'pythonw.exe'
        if (Test-Path $pyw) { return $pyw }
    }
    throw '找不到 pythonw.exe，请确认 Python 已安装'
}


function Get-ListenLine {
    # 用 netstat 而不是 Get-NetTCPConnection：前者在各种权限/沙箱环境下都稳定
    return (netstat -ano | Select-String -Pattern ":$Port\s+.*LISTENING" | Select-Object -First 1)
}


function Show-Status {
    Write-Host ''
    Write-Host "======== OTA 固件服务状态 ========" -ForegroundColor Cyan

    $task = Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
    if (-not $task) {
        Write-Host "计划任务 : 未注册（运行 install_ota_service.cmd 安装）" -ForegroundColor Yellow
    }
    else {
        $info = Get-ScheduledTaskInfo -TaskName $TaskName
        Write-Host ("计划任务 : {0}" -f $task.State)
        Write-Host ("上次运行 : {0}" -f $info.LastRunTime)
        Write-Host ("上次结果 : 0x{0:X}  (0x41301=正在运行, 0x0=已完成)" -f $info.LastTaskResult)
    }

    $listen = Get-ListenLine
    if ($listen) {
        Write-Host ("端口监听 : {0}" -f $listen.Line.Trim()) -ForegroundColor Green
    }
    else {
        Write-Host "端口监听 : 无（服务未启动或端口被占用）" -ForegroundColor Red
    }

    Write-Host ("固件目录 : {0}" -f $FirmwareRoot)
    Write-Host ("访问日志 : {0}" -f $LogPath)

    try {
        $resp = Invoke-WebRequest -UseBasicParsing -Uri ("http://127.0.0.1:{0}/health" -f $Port) -TimeoutSec 5
        Write-Host "自检     : OK" -ForegroundColor Green
        Write-Host $resp.Content
    }
    catch {
        Write-Host ("自检     : 失败 - {0}" -f $_.Exception.Message) -ForegroundColor Red
    }

    Write-Host "==================================" -ForegroundColor Cyan
    Write-Host ''
}


function Stop-ServerProcess {
    # 计划任务被停止后，pythonw 子进程不一定会立刻退出，
    # 按命令行特征精确清理，避免误杀其它 python 进程
    Stop-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 600

    Get-CimInstance Win32_Process -Filter "Name='pythonw.exe' OR Name='python.exe'" -ErrorAction SilentlyContinue |
        Where-Object { $_.CommandLine -like '*ota_server.py*' } |
        ForEach-Object {
            Write-Host ("  结束进程 PID {0}" -f $_.ProcessId)
            Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue
        }
    Start-Sleep -Milliseconds 400
}


function Install-Service {
    if (-not (Test-Admin)) { throw '需要管理员权限' }
    if (-not (Test-Path $ServerPy)) { throw ("找不到服务脚本：{0}" -f $ServerPy) }
    if (-not (Test-Path $FirmwareRoot)) { throw ("固件目录不存在：{0}" -f $FirmwareRoot) }

    $pyw = Find-Pythonw

    # 路径统一加引号，将来把工程挪到含空格的目录也不用改
    $arg = '"{0}" --port {1} --root "{2}" --log "{3}"' -f $ServerPy, $Port, $FirmwareRoot, $LogPath

    $act = New-ScheduledTaskAction -Execute $pyw -Argument $arg -WorkingDirectory $ScriptDir

    # 开机启动 + 延迟 15 秒：等网络栈和防火墙服务先就绪
    $trg = New-ScheduledTaskTrigger -AtStartup
    $trg.Delay = 'PT15S'

    $prn = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest

    # ExecutionTimeLimit=0 表示不限时长。默认策略可能会在数天后
    # 把"运行时间过长"的任务强制结束，服务就悄悄没了
    $set = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit ([TimeSpan]::Zero) -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1) -StartWhenAvailable

    Register-ScheduledTask -TaskName $TaskName -Action $act -Trigger $trg -Principal $prn -Settings $set -Force -Description 'OTA firmware HTTP server (port 80)' | Out-Null

    Write-Host ("[OK] 已注册计划任务 {0}" -f $TaskName) -ForegroundColor Green
    Write-Host ("[OK] 解释器 {0}" -f $pyw)

    # 先清掉可能残留的旧实例，避免端口冲突
    Stop-ServerProcess
    Start-ScheduledTask -TaskName $TaskName
    Start-Sleep -Seconds 4
    Show-Status
}


function Uninstall-Service {
    if (-not (Test-Admin)) { throw '需要管理员权限' }

    Stop-ServerProcess
    Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false -ErrorAction SilentlyContinue
    Write-Host ("[OK] 已删除计划任务 {0}" -f $TaskName) -ForegroundColor Green

    if (Get-ListenLine) {
        Write-Host ("[!] 端口 {0} 仍被占用，可能被其它程序占着：netstat -ano | findstr :{0}" -f $Port) -ForegroundColor Yellow
    }
    else {
        Write-Host ("[OK] 端口 {0} 已释放" -f $Port)
    }
}


switch ($Action) {

    'Install' { Install-Service }

    'Uninstall' { Uninstall-Service }

    'Start' {
        if (-not (Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue)) {
            throw ("任务 {0} 未注册，请先运行 -Action Install" -f $TaskName)
        }
        Start-ScheduledTask -TaskName $TaskName
        Start-Sleep -Seconds 3
        Show-Status
    }

    'Stop' {
        Stop-ServerProcess
        Write-Host '[OK] 服务已停止' -ForegroundColor Green
    }

    'Restart' {
        Stop-ServerProcess
        Start-Sleep -Seconds 1
        Start-ScheduledTask -TaskName $TaskName
        Start-Sleep -Seconds 3
        Show-Status
    }

    'Status' { Show-Status }

    'Log' {
        if (Test-Path $LogPath) {
            # 日志是 UTF-8 写的，必须显式指定，否则中文会变成乱码
            Get-Content $LogPath -Encoding UTF8 -Tail $Tail
        }
        else {
            Write-Host ("日志文件不存在：{0}" -f $LogPath) -ForegroundColor Yellow
        }
    }
}

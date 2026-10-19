@echo off
rem ===================================================================
rem  OTA 固件服务 —— 安装（注册为开机自启计划任务）
rem
rem  右键"以管理员身份运行"，或直接双击（脚本会自己请求提升）。
rem
rem  为什么不用"开个黑窗口挂着"：
rem    黑窗口属于当前会话，关掉它 / 退出 IDE / 注销用户就没了，
rem    设备一发 upgrade 就是连接被拒。计划任务跑在 session 0，
rem    不弹窗、不占桌面、不需要登录。
rem
rem  实际逻辑在同目录的 ota_service.ps1，这里只做提权和转发。
rem ===================================================================

chcp 65001 >nul 2>&1
setlocal

net session >nul 2>&1
if errorlevel 1 (
    echo [OTA] 需要管理员权限，正在请求提升...
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0ota_service.ps1" -Action Install

echo.
pause

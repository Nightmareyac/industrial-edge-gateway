@echo off
rem ===================================================================
rem  OTA 固件服务 —— 卸载（停止 + 删除计划任务 + 清理残留进程）
rem  右键"以管理员身份运行"，或直接双击（脚本会自己请求提升）。
rem
rem  注意：只删除任务，不删除 ota_release 里的固件和日志文件。
rem ===================================================================

chcp 65001 >nul 2>&1
setlocal

net session >nul 2>&1
if errorlevel 1 (
    echo [OTA] 需要管理员权限，正在请求提升...
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0ota_service.ps1" -Action Uninstall

echo.
pause

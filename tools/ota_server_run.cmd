@echo off
rem ===================================================================
rem  OTA 固件服务 —— 启动入口（供计划任务调用）
rem
rem  单独抽成一个 .cmd 的原因：
rem  schtasks /TR 对引号的处理非常糟糕，直接把
rem  "python.exe ota_server.py --root ..." 塞进去十有八九会被拆错，
rem  最后报"找不到文件"。指向一个 .cmd 则完全不用处理嵌套引号。
rem ===================================================================

chcp 65001 >nul 2>&1
setlocal

rem 切到脚本所在目录，保证相对路径稳定（计划任务启动时工作目录是 System32）
cd /d "%~dp0"

rem ---- 定位 python.exe ----
rem 优先用固定路径：以 SYSTEM 身份运行时，PATH 里没有当前用户的
rem AppData 目录，`where python` 会直接找不到。
set "PY_EXE="
if exist "C:\Users\Administrator\AppData\Local\Programs\Python\Python313\python.exe" (
    set "PY_EXE=C:\Users\Administrator\AppData\Local\Programs\Python\Python313\python.exe"
)
if not defined PY_EXE (
    for /f "delims=" %%P in ('where python 2^>nul') do (
        if not defined PY_EXE set "PY_EXE=%%P"
    )
)
if not defined PY_EXE (
    echo [OTA] 找不到 python.exe，请检查安装或修改本脚本里的路径
    exit /b 1
)

"%PY_EXE%" "%~dp0ota_server.py" --port 80 --root "%~dp0..\ota_release" --log "%~dp0ota_server.log"

exit /b %errorlevel%

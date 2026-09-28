@echo off
chcp 65001 >nul
cd /d %~dp0
rem server start wait then open owner page (first run needs model warmup)
start "" /min cmd /c "timeout /t 10 /nobreak >/dev/null & start http://localhost:8765"
:loop
kiosk_tracking.exe --no-window --log=pipeline.log %*
if %errorlevel%==0 exit /b 0
echo %date% %time% crash code=%errorlevel% restart in 3s >> restarts.log
timeout /t 3 /nobreak >nul
goto loop

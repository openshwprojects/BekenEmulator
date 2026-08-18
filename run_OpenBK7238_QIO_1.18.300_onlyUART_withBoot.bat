@echo off
set "SIM_SCRIPT=%~dp0src\main.py"
set TARGET_FILE=%~1

if "%TARGET_FILE%"=="" (
    set "TARGET_FILE=%~dp0firmwares\OpenBK7238_QIO_1.18.300.bin"
)

echo Running simulator (UART Only) for: %TARGET_FILE%
python "%SIM_SCRIPT%" "%TARGET_FILE%" --with-boot --only-uart -chip BK7238
pause

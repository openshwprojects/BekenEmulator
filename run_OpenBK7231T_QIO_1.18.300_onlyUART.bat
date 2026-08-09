@echo off
set SIM_SCRIPT=%~dp0bk7231_sim.py
set TARGET_FILE=%~1

if "%TARGET_FILE%"=="" (
    set TARGET_FILE=%~dp0firmwares\OpenBK7231T_QIO_1.18.300.bin
)

echo Running simulator (UART Only) for: %TARGET_FILE%
python "%SIM_SCRIPT%" "%TARGET_FILE%" --only-uart
pause

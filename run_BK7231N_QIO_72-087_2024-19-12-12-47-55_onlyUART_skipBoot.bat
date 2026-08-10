@echo off
set SIM_SCRIPT=%~dp0bk7231_sim.py
set TARGET_FILE=%~1

if "%TARGET_FILE%"=="" (
    set TARGET_FILE=%~dp0references\FlashDumps\IoT\BK7231N\BK7231N_QIO_72-087_2024-19-12-12-47-55.bin
)

echo Running simulator (UART Only) for: %TARGET_FILE%
python "%SIM_SCRIPT%" "%TARGET_FILE%" --only-uart
pause

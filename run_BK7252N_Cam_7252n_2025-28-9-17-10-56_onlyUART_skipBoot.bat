@echo off
set "SIM_SCRIPT=%~dp0src\main.py"
set TARGET_FILE=%~1

if "%TARGET_FILE%"=="" (
    set "TARGET_FILE=%~dp0references\FlashDumps\IoT\BK7252N\Cam_readResult_BK7252N_QIO_7252n_2025-28-9-17-10-56.bin"
)

echo Running simulator (UART Only) for: %TARGET_FILE%
python "%SIM_SCRIPT%" "%TARGET_FILE%" --only-uart -chip BK7252N
pause

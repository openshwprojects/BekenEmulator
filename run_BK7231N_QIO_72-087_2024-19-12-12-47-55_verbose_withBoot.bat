@echo off
set "SIM_SCRIPT=%~dp0src\main.py"
set TARGET_FILE=%~1

if "%TARGET_FILE%"=="" (
    set "TARGET_FILE=%~dp0references\FlashDumps\IoT\BK7231N\BK7231N_QIO_72-087_2024-19-12-12-47-55.bin"
)

echo Running simulator (Verbose) for: %TARGET_FILE%
python "%SIM_SCRIPT%" "%TARGET_FILE%" --with-boot
pause

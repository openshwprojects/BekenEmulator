@echo off
set "SIM_SCRIPT=%~dp0src\main.py"
set TARGET_FILE=%~1

if "%TARGET_FILE%"=="" (
    set "TARGET_FILE=%~dp0firmwares\BK7231T_QIO_Woox_R5111_2023-14-10-23-46-06.bin"
)

echo Running simulator (Verbose) for: %TARGET_FILE%
python "%SIM_SCRIPT%" "%TARGET_FILE%"
pause

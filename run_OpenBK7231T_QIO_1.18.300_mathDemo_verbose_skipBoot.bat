@echo off
set SIM_SCRIPT=%~dp0bk7231_sim.py
set TARGET_FILE=%~1

if "%TARGET_FILE%"=="" (
    set "TARGET_FILE=%~dp0firmwares\OpenBK7231T_QIO_1.18.300_mathDemo.bin"
)

echo Running simulator (Verbose) for: %TARGET_FILE%
python "%SIM_SCRIPT%" "%TARGET_FILE%"
pause

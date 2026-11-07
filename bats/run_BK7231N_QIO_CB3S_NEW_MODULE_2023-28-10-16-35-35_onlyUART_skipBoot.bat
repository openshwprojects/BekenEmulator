@echo off
set "SIM_BIN=%~dp0..\bekensim.exe"
set TARGET_FILE=%~1

if "%TARGET_FILE%"=="" (
    set "TARGET_FILE=%~dp0..\references\FlashDumps\IoT\BK7231N\BK7231N_QIO_CB3S_NEW_MODULE_2023-28-10-16-35-35.bin"
)

echo Running simulator (UART Only) for: %TARGET_FILE%
"%SIM_BIN%" "%TARGET_FILE%" --only-uart -key TUYA
pause

@echo off
set "SIM_BIN=%~dp0bekensim.exe"
set TARGET_FILE=%~1

if "%TARGET_FILE%"=="" (
    set "TARGET_FILE=%~dp0references\FlashDumps\IoT\BK7238\BK7238_QIO_GBDQA_63V_16A_2026-09-7-22-13-43.bin"
)

echo Running simulator (UART Only) for: %TARGET_FILE%
"%SIM_BIN%" "%TARGET_FILE%" --with-boot --only-uart -chip BK7238
pause

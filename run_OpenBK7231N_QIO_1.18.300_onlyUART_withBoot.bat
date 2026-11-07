@echo off
set "SIM_BIN=%~dp0bekensim.exe"
set TARGET_FILE=%~1

if "%TARGET_FILE%"=="" (
    set "TARGET_FILE=%~dp0firmwares\OpenBK7231N_QIO_1.18.300.bin"
)

echo Running simulator (UART Only) for: %TARGET_FILE%
"%SIM_BIN%" "%TARGET_FILE%" --with-boot --only-uart -key TUYA
pause

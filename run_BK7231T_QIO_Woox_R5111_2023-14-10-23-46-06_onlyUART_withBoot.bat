@echo off
set "SIM_BIN=%~dp0bekensim.exe"
set TARGET_FILE=%~1

if "%TARGET_FILE%"=="" (
    set "TARGET_FILE=%~dp0firmwares\BK7231T_QIO_Woox_R5111_2023-14-10-23-46-06.bin"
)

echo Running simulator (UART Only) for: %TARGET_FILE%
"%SIM_BIN%" "%TARGET_FILE%" --with-boot --only-uart -key TUYA
pause

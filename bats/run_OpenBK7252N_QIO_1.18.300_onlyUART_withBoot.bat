@echo off
set "SIM_BIN=%~dp0..\bekensim.exe"
set TARGET_FILE=%~1

if "%TARGET_FILE%"=="" (
    set "TARGET_FILE=%~dp0..\firmwares\OpenBK7252N_QIO_1.18.300.bin"
)

echo Running simulator (UART Only) for: %TARGET_FILE%
"%SIM_BIN%" "%TARGET_FILE%" --with-boot --only-uart -chip BK7252N
pause

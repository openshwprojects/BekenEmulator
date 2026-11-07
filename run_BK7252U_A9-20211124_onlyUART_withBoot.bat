@echo off
set "SIM_BIN=%~dp0bekensim.exe"
set TARGET_FILE=%~1

if "%TARGET_FILE%"=="" (
    set "TARGET_FILE=%~dp0references\FlashDumps\IoT\BK7252U\QFN68\BK7252-A9-20211124.bin"
)

echo Running simulator (UART Only) for: %TARGET_FILE%
"%SIM_BIN%" "%TARGET_FILE%" --with-boot --only-uart -chip BK7252
pause

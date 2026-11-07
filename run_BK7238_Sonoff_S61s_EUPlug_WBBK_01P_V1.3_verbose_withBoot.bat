@echo off
set "SIM_BIN=%~dp0bekensim.exe"
set TARGET_FILE=%~1

if "%TARGET_FILE%"=="" (
    set "TARGET_FILE=%~dp0references\FlashDumps\IoT\BK7238\Sonoff_S61s_EUPlug_WBBK_01P_V1.3.bin"
)

echo Running simulator (Verbose) for: %TARGET_FILE%
"%SIM_BIN%" "%TARGET_FILE%" --with-boot -chip BK7238
pause

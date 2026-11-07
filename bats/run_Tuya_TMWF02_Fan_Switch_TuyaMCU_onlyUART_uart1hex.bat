@echo off
set "SIM_BIN=%~dp0..\bekensim.exe"
set TARGET_FILE=%~1

if "%TARGET_FILE%"=="" (
    set "TARGET_FILE=%~dp0..\references\FlashDumps\IoT\BK7231T\Tuya_TMWF02_Fan_Switch_(FS_WB_01_schemaID-000002xxgr)_key34ak4q5rmrkef_kxtcfbazhsvjqcfz_TuyaMCU_1.1.71.bin"
)

echo Running simulator (UART2 log + UART1/TuyaMCU as HEX) for: %TARGET_FILE%
"%SIM_BIN%" "%TARGET_FILE%" --only-uart --uart1-hex -key TUYA
pause

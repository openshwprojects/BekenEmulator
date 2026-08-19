@echo off
set "SIM_SCRIPT=%~dp0src\main.py"
set TARGET_FILE=%~1

if "%TARGET_FILE%"=="" (
    set "TARGET_FILE=%~dp0references\FlashDumps\IoT\BK7231T\Tuya_TMWF02_Fan_Switch_(FS_WB_01_schemaID-000002xxgr)_key34ak4q5rmrkef_kxtcfbazhsvjqcfz_TuyaMCU_1.1.71.bin"
)

echo Running simulator (UART2 log + UART1/TuyaMCU as HEX) for: %TARGET_FILE%
python "%SIM_SCRIPT%" "%TARGET_FILE%" --only-uart --uart1-hex -key TUYA
pause

@echo off
REM Stock Tuya firmware (TuyaOS 3.11.12) for a BK7231N BEOK TOL47WIFI-WP-WF
REM thermostat. A *paired* dump - both KV copies (current 0x1ed000, mirror
REM 0x1cf000) carry data, so the SDK skips manufacturing test, reaches normal
REM operation, opens the MCU link at 9600 baud and streams TuyaMCU heartbeats.
REM Expect many: [UART1/MCU] 55 aa 00 00 00 00 ff
set "SIM_SCRIPT=%~dp0src\main.py"
set TARGET_FILE=%~1
if "%TARGET_FILE%"=="" (
    set "TARGET_FILE=%~dp0firmwares\BK7231N_Tuya_BEOK_TOL47WIFI_Thermostat_TuyaMCU_TuyaOS_3.11.12.bin"
)
echo Running simulator (UART2 log + UART1/TuyaMCU as HEX) for: %TARGET_FILE%
python "%SIM_SCRIPT%" "%TARGET_FILE%" --only-uart --uart1-hex -key TUYA
pause

@echo off
REM Stock Tuya firmware (Tuya IoT SDK, NOT OpenBeken) for a BK7231N
REM temperature/humidity LCD sensor that talks to an MCU over serial.
REM UART2 is the text log, UART1 (the MCU link) is shown as HEX so any
REM 55 AA TuyaMCU frames are visible. Encrypted image - needs the TUYA key.
set "SIM_SCRIPT=%~dp0src\main.py"
set TARGET_FILE=%~1

if "%TARGET_FILE%"=="" (
    set "TARGET_FILE=%~dp0firmwares\BK7231N_Tuya_TempHum_LCD_Sensor_TuyaMCU_2025-26-9.bin"
)

echo Running simulator (UART2 log + UART1/TuyaMCU as HEX) for: %TARGET_FILE%
python "%SIM_SCRIPT%" "%TARGET_FILE%" --only-uart --uart1-hex -key TUYA
pause

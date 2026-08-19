@echo off
REM Stock Tuya firmware (Tuya IoT SDK 3.1.28) for a BK7231N ETWF4301 thermostat
REM (AXZN / TM1640 panel) - the MCU owns the display and sensors, the Beken is
REM just the radio, so the two talk over UART1.
REM
REM This is the FIRST stock-Tuya dump we found that actually drives its MCU:
REM it is a *paired* dump ("have actived over 15 min, not enter mf_init"), so it
REM skips manufacturing test, reaches normal operation, and emits TuyaMCU
REM heartbeats unprompted - byte-identical to OpenBeken's own driver.
REM Expect repeated: [UART1/MCU] 55 aa 00 00 00 00 ff
REM
REM Also exercises the physical flash-addressing model: both KV copies
REM (0x1ed000 current, 0x1cf000 mirror) must read valid with matching counts.
set "SIM_SCRIPT=%~dp0src\main.py"
set TARGET_FILE=%~1

if "%TARGET_FILE%"=="" (
    set "TARGET_FILE=%~dp0firmwares\BK7231N_Tuya_Ettroit_ETWF4301_Thermostat_TuyaMCU_3.1.28.bin"
)

echo Running simulator (UART2 log + UART1/TuyaMCU as HEX) for: %TARGET_FILE%
python "%SIM_SCRIPT%" "%TARGET_FILE%" --only-uart --uart1-hex -key TUYA
pause

@echo off
REM Stock Tuya firmware (TuyaOS 3.11.12) for a BK7231N PJ1103C dual-clamp power
REM meter. The MCU owns the current clamps and all metering; the Beken is only
REM the radio, so the two talk over UART1 at 9600 baud.
REM A *paired* dump (both KV copies written), so it skips manufacturing test,
REM reaches normal operation and streams TuyaMCU heartbeats.
REM Expect many: [UART1/MCU] 55 aa 00 00 00 00 ff
set "SIM_SCRIPT=%~dp0src\main.py"
set TARGET_FILE=%~1
if "%TARGET_FILE%"=="" (
    set "TARGET_FILE=%~dp0firmwares\BK7231N_Tuya_PJ1103C_DualClampPowerMeter_TuyaMCU_TuyaOS_3.11.12.bin"
)
echo Running simulator (UART2 log + UART1/TuyaMCU as HEX) for: %TARGET_FILE%
python "%SIM_SCRIPT%" "%TARGET_FILE%" --only-uart --uart1-hex -key TUYA
pause

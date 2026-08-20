@echo off
REM Stock Tuya firmware (TuyaOS 3.3.44) for a BK7231N Arlec Razer CHR381HA RGB
REM LED strip - a PWM-driven light, not an MCU device: the Beken drives the LED
REM channels directly through hardware PWM (cool on P6, warm on P8, 1000 Hz),
REM so there is no TuyaMCU traffic on UART1 by design.
REM A paired dump: it reads its protected key and reaches normal operation.
set "SIM_SCRIPT=%~dp0src\main.py"
set TARGET_FILE=%~1
if "%TARGET_FILE%"=="" (
    set "TARGET_FILE=%~dp0firmwares\BK7231N_Tuya_Arlec_Razer_RGBStrip_PWM_TuyaOS_3.3.44.bin"
)
echo Running simulator for: %TARGET_FILE%
python "%SIM_SCRIPT%" "%TARGET_FILE%" --only-uart -key TUYA
pause

@echo off
REM Stock Tuya firmware (Tuya IoT SDK 2.3.1) for a BK7231N "zmai90" energy meter
REM built around an RN8209C metering chip on UART. Encrypted image, needs the
REM TUYA key. UART2 is the text log, UART1 (the RN8209C link) shown as HEX.
REM NOTE: this firmware reads its protected key (0x1ee000) fine but then drops
REM into the mf_test manufacturing-test thread and does not reach normal
REM operation, so no RN8209C polling / UART1 traffic is seen in emulation.
set "SIM_SCRIPT=%~dp0src\main.py"
set TARGET_FILE=%~1

if "%TARGET_FILE%"=="" (
    set "TARGET_FILE=%~dp0firmwares\BK7231N_Tuya_zmai90_RN8209C_EnergyMeter.bin"
)

echo Running simulator (UART2 log + UART1 as HEX) for: %TARGET_FILE%
python "%SIM_SCRIPT%" "%TARGET_FILE%" --only-uart --uart1-hex -key TUYA
pause

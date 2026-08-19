@echo off
REM mathDemo image with an OpenBeken config injected at flash 0x1e1000 whose
REM initCommandLine (offset 0x5E0) is:
REM     startDriver TuyaMCU
REM TuyaMCU_Init opens UART1 itself at 9600, so no uartInit is needed. OBK then
REM talks first, with no MCU attached: TuyaMCU_RunStateMachine_V3 begins with
REM heartbeat_timer == 0, so the first per-second tick sends a HEARTBEAT and
REM repeats it every 4 seconds while unanswered.
REM Expect: [UART1/MCU] 55 aa 00 00 00 00 ff
REM Regenerate with:
REM   python tools\make_obk_config.py firmwares\OpenBK7231T_QIO_1.18.300_mathDemo.bin ^
REM     firmwares\OpenBK7231T_QIO_1.18.300_mathDemo_obkStartupCommand_tuyaMCU.bin ^
REM     -c "startDriver TuyaMCU"
set "SIM_SCRIPT=%~dp0src\main.py"
set TARGET_FILE=%~1

if "%TARGET_FILE%"=="" (
    set "TARGET_FILE=%~dp0firmwares\OpenBK7231T_QIO_1.18.300_mathDemo_obkStartupCommand_tuyaMCU.bin"
)

echo Running simulator (UART2 log + UART1/TuyaMCU as HEX) for: %TARGET_FILE%
python "%SIM_SCRIPT%" "%TARGET_FILE%" --only-uart --uart1-hex -key TUYA
pause

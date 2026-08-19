@echo off
REM mathDemo image with an OpenBeken config injected at flash 0x1e1000 whose
REM initCommandLine (offset 0x5E0) is:
REM     backlog uartInit 115200; uartSendHex BADF00D12345
REM OBK's own uartSendHex writes to UART_PORT_INDEX_0 = BK_UART_1, so the bytes
REM appear on the UART1 hex stream while UART2 stays the text log. This is a
REM plain UART demo - not TuyaMCU; the payload is arbitrary marker bytes.
REM Expect: [UART1/MCU] ba df 00 d1 23 45
REM Regenerate with:
REM   python tools\make_obk_config.py firmwares\OpenBK7231T_QIO_1.18.300_mathDemo.bin ^
REM     firmwares\OpenBK7231T_QIO_1.18.300_mathDemo_obkStartupCommand_uartSendHex.bin ^
REM     -c "backlog uartInit 115200; uartSendHex BADF00D12345"
set "SIM_SCRIPT=%~dp0src\main.py"
set TARGET_FILE=%~1

if "%TARGET_FILE%"=="" (
    set "TARGET_FILE=%~dp0firmwares\OpenBK7231T_QIO_1.18.300_mathDemo_obkStartupCommand_uartSendHex.bin"
)

echo Running simulator (UART2 log + UART1 as HEX) for: %TARGET_FILE%
python "%SIM_SCRIPT%" "%TARGET_FILE%" --only-uart --uart1-hex -key TUYA
pause

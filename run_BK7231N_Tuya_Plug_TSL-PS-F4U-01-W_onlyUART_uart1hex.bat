@echo off
REM Stock Tuya firmware (Tuya IoT SDK 2.3.3, NOT OpenBeken) for a BK7231N smart
REM plug - oem_bk7231n_plug 1.1.17. Encrypted image, needs the TUYA key.
REM Exercises the protected key block at 0x1EE000, which lives above the
REM CRC-stripped logical end of the image and is AES-128-ECB encrypted; the
REM emulator must serve the raw dump bytes there or the device sees blank flash.
REM Expect: "key_addr: 0x1ee000" then "get key:" with the real key bytes.
set "SIM_SCRIPT=%~dp0src\main.py"
set TARGET_FILE=%~1

if "%TARGET_FILE%"=="" (
    set "TARGET_FILE=%~dp0firmwares\BK7231N_Tuya_Plug_TSL-PS-F4U-01-W_1.1.17.bin"
)

echo Running simulator (UART2 log + UART1 as HEX) for: %TARGET_FILE%
python "%SIM_SCRIPT%" "%TARGET_FILE%" --only-uart --uart1-hex -key TUYA
pause

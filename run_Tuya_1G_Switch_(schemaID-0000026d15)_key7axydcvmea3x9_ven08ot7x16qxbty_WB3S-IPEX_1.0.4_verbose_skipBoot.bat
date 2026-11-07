@echo off
set "SIM_BIN=%~dp0bekensim.exe"
set TARGET_FILE=%~1

if "%TARGET_FILE%"=="" (
    set "TARGET_FILE=%~dp0references\FlashDumps\IoT\BK7231T\Tuya_1G_Switch_(schemaID-0000026d15)_key7axydcvmea3x9_ven08ot7x16qxbty_WB3S-IPEX_1.0.4.bin"
)

echo Running simulator (Verbose) for: %TARGET_FILE%
"%SIM_BIN%" "%TARGET_FILE%"  -key TUYA
pause

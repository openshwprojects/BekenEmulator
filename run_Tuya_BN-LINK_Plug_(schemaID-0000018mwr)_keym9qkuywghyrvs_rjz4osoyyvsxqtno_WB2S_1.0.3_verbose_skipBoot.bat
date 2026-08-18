@echo off
set "SIM_SCRIPT=%~dp0src\main.py"
set TARGET_FILE=%~1

if "%TARGET_FILE%"=="" (
    set "TARGET_FILE=%~dp0references\FlashDumps\IoT\BK7231T\Tuya_BN-LINK_Plug_(schemaID-0000018mwr)_keym9qkuywghyrvs_rjz4osoyyvsxqtno_WB2S_1.0.3.bin"
)

echo Running simulator (Verbose) for: %TARGET_FILE%
python "%SIM_SCRIPT%" "%TARGET_FILE%" -key TUYA
pause

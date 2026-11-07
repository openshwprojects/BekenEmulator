@echo off
set "SIM_BIN=%~dp0bekensim.exe"
set TARGET_FILE=%~1

if "%TARGET_FILE%"=="" (
    set "TARGET_FILE=%~dp0references\FlashDumps\IoT\BK7231T\Tuya_BN-LINK_Plug_(schemaID-0000018mwr)_keym9qkuywghyrvs_rjz4osoyyvsxqtno_WB2S_1.0.3.bin"
)

echo Running simulator (Verbose) for: %TARGET_FILE%
"%SIM_BIN%" "%TARGET_FILE%" -key TUYA
pause

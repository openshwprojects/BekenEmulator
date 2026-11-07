@echo off
set "SIM_BIN=%~dp0..\bekensim.exe"
set TARGET_FILE=%~1

if "%TARGET_FILE%"=="" (
    set "TARGET_FILE=%~dp0..\references\FlashDumps\IoT\BK7231N\Tuya_LSC_GLS_A60_(schemaID-e1mo65kg)_key8u54q9dtru5jw_g8e2k9js10mqv9nq_CBLC5_1.6.0.bin"
)

echo Running simulator (Verbose) for: %TARGET_FILE%
"%SIM_BIN%" "%TARGET_FILE%"  -key TUYA
pause

@echo off
set SIM_SCRIPT=%~dp0bk7231_sim.py
set TARGET_FILE=%~1

if "%TARGET_FILE%"=="" (
    set TARGET_FILE=%~dp0references\FlashDumps\IoT\BK7231N\Tuya_LSC_GLS_A60_(schemaID-e1mo65kg)_key8u54q9dtru5jw_g8e2k9js10mqv9nq_CBLC5_1.6.0.bin
)

echo Running simulator (UART Only) for: %TARGET_FILE%
python "%SIM_SCRIPT%" "%TARGET_FILE%" --with-boot --only-uart
pause

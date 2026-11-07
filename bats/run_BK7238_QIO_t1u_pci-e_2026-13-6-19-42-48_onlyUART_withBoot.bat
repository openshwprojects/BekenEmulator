@echo off
set "SIM_BIN=%~dp0..\bekensim.exe"
set TARGET_FILE=%~1

if "%TARGET_FILE%"=="" (
    set "TARGET_FILE=%~dp0..\references\FlashDumps\IoT\BK7238\BK7238_QIO_t1u_pci-e_PC_card_2_2026-13-6-19-42-48.bin"
)

echo Running simulator (UART Only) for: %TARGET_FILE%
"%SIM_BIN%" "%TARGET_FILE%" --with-boot --only-uart -chip BK7238
pause

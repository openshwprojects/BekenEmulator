@echo off
REM mathDemo image with an OpenBeken config injected at flash 0x1e1000 whose
REM initCommandLine (offset 0x5E0) is:
REM     echo Test12343242343243
REM Expect "CFG_InitAndLoad: Correct config has been loaded" followed by
REM "Info:CMD:Test12343242343243" once Main_Init_After_Delay runs.
REM Regenerate with:
REM   python tools\make_obk_config.py firmwares\OpenBK7231T_QIO_1.18.300_mathDemo.bin ^
REM     firmwares\OpenBK7231T_QIO_1.18.300_mathDemo_obkStartupCommand_echo.bin ^
REM     -c "echo Test12343242343243"
set "SIM_SCRIPT=%~dp0src\main.py"
set TARGET_FILE=%~1

if "%TARGET_FILE%"=="" (
    set "TARGET_FILE=%~dp0firmwares\OpenBK7231T_QIO_1.18.300_mathDemo_obkStartupCommand_echo.bin"
)

echo Running simulator (startup command: echo) for: %TARGET_FILE%
python "%SIM_SCRIPT%" "%TARGET_FILE%" --only-uart -key TUYA
pause

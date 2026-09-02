@echo off
REM Stock Tuya RGB controller on BK7238 (plaintext image, no -key) with the
REM RivieraWaves BLE core modelled (--ble-core, src/blecore.py) and FIQ delivery.
REM Without --ble-core the controller arms deep sleep once and never wakes, and
REM the NimBLE host times out on every HCI command (ble_hs_hci_wait_for_ack,
REM rc = 19) and loops on assert. With it, the controller runs its real
REM sleep/wake loop, answers the host, and Tuya's BLE service logs
REM "ble adv updated". No radio is modelled - nothing goes on air.
set "SIM_SCRIPT=%~dp0src\main.py"
set TARGET_FILE=%~1
if "%TARGET_FILE%"=="" (
    set "TARGET_FILE=%~dp0firmwares\BK7238_Tuya_RGBController_TY-02-3CH_PWM_T1-3S_1.0.14.bin"
)
echo Running simulator (UART only, BLE core modelled) for: %TARGET_FILE%
python "%SIM_SCRIPT%" "%TARGET_FILE%" --only-uart -chip BK7238 --ble-core
pause

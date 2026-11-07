@echo off
REM OpenBeken's own web panel in your browser, served by the emulated firmware.
REM
REM OpenBeken boots into its default open access point; the Wi-Fi MAC is
REM modelled (src\wifimac.py), a virtual station joins the AP and takes a DHCP
REM lease from the device (src\wifipeer.py, src\netstack.py), and 127.0.0.1:PORT
REM on this PC is forwarded to the device's port 80. Open http://127.0.0.1/
REM (or http://127.0.0.1:PORT/) once "[NET] ping reply" shows up.
REM
REM Usage: run_..._wifi_AP_webPanel.bat [image] [port]   (default port 80; either
REM order, a number is taken as the port)
REM
REM The first run boots from scratch (a few minutes) and saves the device state
REM to scratch\<image>_ap.snap as soon as the AP is up; later runs resume from
REM that snapshot and the panel is back within a minute. Delete the .snap file
REM to boot from scratch again. Close this window to stop the device.
set "SIM_BIN=%~dp0..\bekensim.exe"
set "TARGET_FILE="
set "PORT="
for %%A in (%*) do (
    echo %%~A| findstr /r "^[0-9][0-9]*$" >nul && set "PORT=%%~A" || set "TARGET_FILE=%%~A"
)
if "%TARGET_FILE%"=="" set "TARGET_FILE=%~dp0..\firmwares\OpenBK7231T_QIO_1.18.300.bin"
if "%PORT%"=="" set "PORT=80"
for %%F in ("%TARGET_FILE%") do set "SNAP=%~dp0..\scratch\%%~nF_ap.snap"
if not exist "%~dp0..\scratch" mkdir "%~dp0..\scratch"
if exist "%SNAP%" goto resume

echo First run: booting %TARGET_FILE% from scratch, saving %SNAP% when the AP is up.
echo The panel will be on http://127.0.0.1:%PORT%/ after the "[NET] ping reply" line.
"%SIM_BIN%" "%TARGET_FILE%" --only-uart -key TUYA --wifi-peer auto --net-forward %PORT%:80 --snapshot-save "%SNAP%" --snapshot-at "sending broadcast_deauth"
goto end

:resume
echo Resuming %TARGET_FILE% from %SNAP%.
echo The panel will be on http://127.0.0.1:%PORT%/ after the "[NET] ping reply" line.
"%SIM_BIN%" "%TARGET_FILE%" --only-uart -key TUYA --wifi --snapshot-load "%SNAP%" --wifi-peer auto --net-forward %PORT%:80

:end
pause

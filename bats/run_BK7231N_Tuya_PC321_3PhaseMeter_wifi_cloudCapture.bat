@echo off
REM A PAIRED stock Tuya dump reaching for its cloud, captured offline.
REM
REM This meter was dumped after its owner had set it up, so its flash still
REM holds their network name. --wifi-peer auto plays whatever network the
REM device asks for, so it joins, takes a DHCP lease, announces its own cloud
REM identity on the LAN (Tuya protocol 3.3, decrypted with the key shared by
REM every Tuya device - look for the [NET] Tuya LAN broadcast line with its
REM gwId and productKey) and opens TLS to its cloud at m2.tuyaeu.com:8886.
REM
REM Nothing leaves this machine. Offline is the default: names resolve to
REM 10.200.x.x and each connection is accepted locally and decoded as a [NET]
REM line. Pass --net-out to let it actually reach the internet instead.
REM
REM Other dumps worth trying the same way (see tests/self_test.py):
REM   BK7231N_Tuya_A03CB3S_Alarm_TuyaMCU_2.1.17.bin        paired, joins 'KN'
REM   BK7238_Tuya_RGBController_TY-02-3CH_PWM_T1-3S_1.0.14.bin  raises its own
REM                                                       SmartLife pairing AP
REM   BK7231N_Tuya_BEOK_TOL47WIFI_Thermostat_...bin        EZ-mode sniffing
REM   BK7231T_QIO_Woox_R5111_...bin  with --wifi-ssid tuya_mdev_test1
REM                                                       enters factory test
set "SIM_BIN=%~dp0..\bekensim.exe"
set TARGET_FILE=%~1
if "%TARGET_FILE%"=="" (
    set "TARGET_FILE=%~dp0..\firmwares\BK7231N_Tuya_PC321_3PhaseMeter_TuyaMCU_2.0.2.bin"
)
echo Running simulator (stock Tuya, cloud traffic captured offline) for: %TARGET_FILE%
"%SIM_BIN%" "%TARGET_FILE%" --only-uart -key TUYA --xvr-selfclear --tuyamcu --tuyamcu-pid gqmmtjclqb7reg5p --tuyamcu-raw --wifi-peer auto --pcap "%~dp0..\scratch\tuya_cloud.pcap"
pause

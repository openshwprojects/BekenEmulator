@echo off
REM OpenBeken configured as a Wi-Fi station (open network "BekenSim", MQTT
REM broker mqtt.bekensim.test, client id obkSim) that also syncs NTP and sends
REM an HTTP GET to api.bekensim.test every 4 s. Image built with
REM   python tools\make_obk_config.py firmwares\OpenBK7231T_QIO_1.18.300.bin OUT
REM     --ssid BekenSim --mqtt-host mqtt.bekensim.test --mqtt-client obkSim
REM     -c "backlog startDriver NTP; ntp_setServer 192.168.1.1; addRepeatingEvent 4 -1 sendGet http://api.bekensim.test/hello?dev=obk"
REM The virtual peer plays the access point (--wifi-peer auto --wifi-ssid
REM BekenSim) and the virtual network is its DHCP server, DNS, NTP and gateway.
REM Offline by default: names resolve to 10.200.x.x sinkhole addresses, the
REM HTTP GET gets a 200 from the offline responder, MQTT talks to the offline
REM broker, and every request and publish is logged as a [NET] line. Add
REM --net-out to let its connections reach the real internet instead.
REM 127.0.0.1:8081 is forwarded to the device's own web server.
set "SIM_BIN=%~dp0bekensim.exe"
set TARGET_FILE=%~1
if "%TARGET_FILE%"=="" (
    set "TARGET_FILE=%~dp0firmwares\OpenBK7231T_QIO_1.18.300_obkConfig_staOpen_BekenSim_netTraffic.bin"
)
echo Running simulator (Wi-Fi station, outbound traffic captured) for: %TARGET_FILE%
"%SIM_BIN%" "%TARGET_FILE%" --only-uart -key TUYA --wifi-peer auto --wifi-ssid BekenSim --net-forward 8081:80 --pcap "%~dp0scratch\obk_sta.pcap"
pause

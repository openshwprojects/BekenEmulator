/* AUTO-GENERATED from tests/self_test.py by alt/tools/gen_cases.py. Do not edit. */
#include "testcase.h"

static const char *const c0_args[] = {"--only-uart","-key","NOT_A_REAL_KEY"};
static const ExpectStr c0_exp[] = {{"Invalid -key value",1,0},{"TUYA",1,0},{"32 hex characters",1,0}};
static const char *const c1_args[] = {"--only-uart","-chip","BK9999"};
static const ExpectStr c1_exp[] = {{"Unknown -chip value",1,0},{"BK7238",1,0},{"BK7252N",1,0}};
static const char *const c2_args[] = {"--only-uart","-chip","BK7252","--ble-core"};
static const ExpectStr c2_exp[] = {{"--ble-core models the BLE 5.x core",1,0},{"this chip has no such core",1,0}};
static const char *const c3_args[] = {"--only-uart"};
static const ExpectStr c3_exp[] = {{"does not look like ARM code",1,0},{"try: -key TUYA",1,0}};
static const char *const c4_args[] = {"--only-uart","-key","TUYA","--net-forward","eighty"};
static const ExpectStr c4_exp[] = {{"bad --net-forward 'eighty'",1,0},{"want LPORT:DPORT",1,0}};
static const char *const c5_args[] = {"--only-uart","-key","TUYA","--adc","P26"};
static const ExpectStr c5_exp[] = {{"bad --adc 'P26'",1,0},{"want CH:VALUE",1,0}};
static const char *const c6_args[] = {"--only-uart","-key","TUYA","--wifi-peer","auto","--net-http-get","/cm\?cmnd=echo%20HttpOverEmulatedWifiOK","--net-forward","{free_port}:80"};
static const ExpectStr c6_exp[] = {{"[WIFI] device acts as an access point (it sends beacons)",1,0},{"[WIFI] found BSS 'OpenBK7231T_",1,0},{"[WIFI] associated with 'OpenBK7231T_",1,0},{"[NET] DHCP: we got 192.168.4.100/255.255.255.0 from the device (router 192.168.4.1)",1,0},{"[NET] ping reply from 192.168.4.1",1,0},{"Info:CMD:HttpOverEmulatedWifiOK",1,0},{"[NET] HTTP GET /cm\?cmnd=echo%20HttpOverEmulatedWifiOK -> HTTP/1.1 200 OK",1,0},{"[NET] TCP forward: 192.168.4.100:",1,0},{"[HTTP_PROBE] GET /index -> HTTP/1.1 200 OK",1,0},{"[HTTP_PROBE] page contains: drivers active",1,0},{"[HTTP_PROBE] page contains: MQTT State:",1,0},{"[HTTP_PROBE] page contains: version 1.18.300",1,0}};
static const char *const c6_hexp0[] = {"drivers active","MQTT State:","version 1.18.300"};
static const HttpProbe c6_probes[] = {{"[NET] HTTP GET /cm\?cmnd=echo%20HttpOverEmulatedWifiOK -> HTTP/1.1 200 OK","/index","GET",0,0,c6_hexp0,3}};
static const char *const c7_args[] = {"--only-uart","-chip","BK7238","--wifi-peer","auto","--net-http-get","/cm\?cmnd=echo%20BK7238HttpOverWifiOK"};
static const ExpectStr c7_exp[] = {{"[WIFI] found BSS 'OpenBK7238_",1,0},{"[WIFI] associated with 'OpenBK7238_",1,0},{"[NET] DHCP: we got 192.168.4.100/255.255.255.0 from the device (router 192.168.4.1)",1,0},{"[NET] ping reply from 192.168.4.1",1,0},{"Info:CMD:BK7238HttpOverWifiOK",1,0},{"[NET] HTTP GET /cm\?cmnd=echo%20BK7238HttpOverWifiOK -> HTTP/1.1 200 OK",1,0}};
static const char *const c8_args[] = {"--only-uart","-key","TUYA","--wifi-peer","auto","--wifi-ssid","BekenSim","--net-http-get","/cm\?cmnd=echo%20StationHttpGetOK"};
static const ExpectStr c8_exp[] = {{"Info:MAIN:Using SSID [BekenSim]",1,0},{"[WIFI] station c8:47:8c:00:00:00 associated to 'BekenSim'",1,0},{"[NET] DHCP: device c8:47:8c:00:00:00 got 192.168.1.100",1,0},{"WIFI_STA_CONNECTED",1,0},{"Info:CMD:StationHttpGetOK",1,0},{"[NET] HTTP GET /cm\?cmnd=echo%20StationHttpGetOK -> HTTP/1.1 200 OK",1,0},{"[NET] DNS mqtt.bekensim.test -> 10.200.",1,0},{"MQTT CONNECT",1,0}};
static const char *const c9_args[] = {"--only-uart","-key","TUYA","--wifi-peer","auto","--wifi-ssid","BekenSec","--wifi-pass","simulator123"};
static const ExpectStr c9_exp[] = {{"[WIFI] station c8:47:8c:00:00:00 associated to 'BekenSec' (AID 1)",1,0},{"[WIFI] station c8:47:8c:00:00:00 associated to a protected network; starting the four-way handshake",1,0},{"[WIFI] station c8:47:8c:00:00:00 completed the four-way handshake; the link is encrypted and open for traffic",1,0},{"WIFI_STA_CONNECTED",1,0},{"[NET] DHCP: device c8:47:8c:00:00:00 got 192.168.1.100",1,0},{"Info:NTP:Unix time:",1,0},{"sends: HTTP GET /wpa2\?ok=1 HTTP/1.1 host api.bekensim.test",1,0}};
static const char *const c10_args[] = {"--only-uart","-key","TUYA","--wifi-peer","auto","--wifi-ssid","BekenSim","--mqtt-publish","cmnd/obkSim/echo=MqttPushReachedTheFirmware","--pcap","{pcap}","--pcap-ip","{pcap_ip}"};
static const ExpectStr c10_exp[] = {{"[NET] DHCP: device c8:47:8c:00:00:00 got 192.168.1.100",1,0},{"[NET] NTP 192.168.1.100 -> 192.168.1.1 answered with the host clock",1,0},{"Info:NTP:Unix time:",1,0},{"[NET] DNS api.bekensim.test -> 10.200.",1,0},{"sends: HTTP GET /hello\?dev=obk HTTP/1.1 host api.bekensim.test",1,0},{"[NET] HTTP GET /hello\?dev=obk HTTP/1.1 answered 200 (offline)",2,0},{"httpclient - no response buff, skipped 143",1,0},{"[NET] DNS mqtt.bekensim.test -> 10.200.",1,0},{"sends: MQTT CONNECT (v3.1.1, client id 'obkSim'",1,0},{"connected to the offline broker",1,0},{"[NET] MQTT SUBSCRIBE obkSim/+/set",1,0},{"[NET] MQTT SUBSCRIBE cmnd/obkSim/+",1,0},{"[NET] MQTT PUBLISH obkSim/connected = online",1,0},{"Info:MQTT:mqtt_connection_cb: Successfully connected",1,0},{"[NET] MQTT -> device PUBLISH cmnd/obkSim/echo = MqttPushReachedTheFirmware",1,0},{"Info:CMD:MqttPushReachedTheFirmware",1,0}};
static const char *const c10_pcap[] = {"beacon","authentication","association request","association response","data","GET /hello\?dev=obk","BekenEmulator offline network","obkSim/connected"};
static const char *const c10_ippcap[] = {"GET /hello\?dev=obk","api.bekensim.test","BekenEmulator offline network","obkSim","cmnd/obkSim/echo"};
static const char *const c11_args[] = {"--only-uart","-key","TUYA"};
static const ExpectStr c11_exp[] = {{"Info:MAIN:quicktick",60,0}};
static const char *const c12_args[] = {"--only-uart","-key","TUYA","--wifi-peer","auto"};
static const ExpectStr c12_exp[] = {{"[WIFI] associated with 'OpenBK7231T_",1,0},{"[NET] DHCP: we got 192.168.4.100",1,0},{"[NET] ping reply from 192.168.4.1",1,0}};
static const char *const c13_args[] = {"--only-uart","-key","TUYA"};
static const ExpectStr c13_exp[] = {{"OpenBK7231T, version 1.18.300",1,0},{"Main_Init_Delay",1,0},{"Info:MQTT:MQTT_RegisterCallback called for",1,0},{", idle ",1,0},{"MQTT 0(0), bWifi 0, secondsWithNoPing -1",2,1},{"no flash configuration, use default",1,0},{"set ip info: 192.168.4.1,255.255.255.0,192.168.4.1",1,0},{"ssid:OpenBK7231T_",1,0}};
static const char *const c14_args[] = {"--only-uart","-key","TUYA"};
static const ExpectStr c14_exp[] = {{"Info:MAIN:Advanced math test started",1,0},{"Info:MAIN:Integer: (123 * 456) / 10 = 5608",1,0},{"Info:MAIN:Float basic: (3.141590 * 1.860000) / (3.141590 - 1.0) = 2.728513",1,0},{"Info:MAIN:Casting: float_to_int = 272, int_to_float = 1684.084106",1,0},{"Info:MAIN:quicktick",1,0},{"CFG_InitAndLoad: Config crc or ident mismatch",1,0}};
static const char *const c15_args[] = {"--only-uart","-key","TUYA","--uart1-rx","echo UartRxConsoleOK\nnosuchcmd_uarttest"};
static const ExpectStr c15_exp[] = {{"CFG_InitAndLoad: Correct config has been loaded",1,0},{"Info:CMD:StartupBeforeConsole",1,0},{"Info:CMD:UartRxConsoleOK",1,0},{"Error:CMD:cmd nosuchcmd_uarttest NOT found",1,0}};
static const char *const c16_args[] = {"--only-uart","-key","TUYA","--uart1-rx","setChannel 5 12+28"};
static const ExpectStr c16_exp[] = {{"CFG_InitAndLoad: Correct config has been loaded",1,0},{"Info:CMD:StartupBeforeConsole",1,0},{"Info:GEN:CHANNEL_Set channel 5 has changed to 40",1,0}};
static const char *const c17_args[] = {"--only-uart","-key","TUYA","--uart1-rx","backlog echo BacklogOne; echo BacklogTwo"};
static const ExpectStr c17_exp[] = {{"CFG_InitAndLoad: Correct config has been loaded",1,0},{"Info:CMD:StartupBeforeConsole",1,0},{"Info:CMD:BacklogOne",1,0},{"Info:CMD:BacklogTwo",1,0}};
static const char *const c18_args[] = {"--only-uart","-key","TUYA","--adc","P23:2150","--uart1-rx","backlog SetPinChannel 23 8; SetPinRole 23 ADC"};
static const ExpectStr c18_exp[] = {{"CFG_InitAndLoad: Correct config has been loaded",1,0},{"Info:CMD:StartupBeforeConsole",1,0},{"Info:MQTT:Channel has changed! Publishing 2150 to channel 8",1,0}};
static const char *const c18_tags[] = {"SARADC"};
static const char *const c19_args[] = {"--only-uart","-key","TUYA"};
static const ExpectStr c19_exp[] = {{"CFG_InitAndLoad: Correct config has been loaded",1,0},{"Info:CMD:Test12343242343243",1,0}};
static const char *const c20_args[] = {"--only-uart","--uart1-hex","-key","TUYA"};
static const ExpectStr c20_exp[] = {{"CFG_InitAndLoad: Correct config has been loaded",1,0},{"[UART1/MCU] ba df 00 d1 23 45",1,0}};
static const char *const c21_args[] = {"--only-uart","--uart1-hex","--tuyamcu","--tuyamcu-dp","1:bool:1","--tuyamcu-dp","2:value:100","--tuyamcu-dp","3:string:HELLO","--tuyamcu-dp","4:enum:2","--tuyamcu-dp","5:bitmap:0x0F","--tuyamcu-inject","55AA0004000003","--tuyamcu-inject","55AA001C00001B","--tuyamcu-inject","55AA0000000000","--tuyamcu-inject","DEADBEEF55AA0099000098","-key","TUYA"};
static const ExpectStr c21_exp[] = {{"CFG_InitAndLoad: Correct config has been loaded",1,0},{"Started TuyaMCU.",1,0},{"CMD_AddChangeHandler: added Channel1 with cmd setChannel 20 1",1,0},{"[UART1/MCU] 55 aa 00 00 00 00 ff",1,0},{"ProcessIncoming[v=0]: cmd 0 (Hearbeat) len 8",1,0},{"cmd 1 (QueryProductInformation)",1,0},{"received {\"p\":\"bekenemulator000\",\"v\":\"1.0.0\"}",1,0},{"cmd 2 (MCUconf)",1,0},{"ProcessIncoming: TUYA_CMD_MCU_CONF, TODO!",1,0},{"55 aa 00 08 00 00 07",1,0},{"ParseState: id 1 type 1-bool len 1",1,0},{"ParseState: byte 1",1,0},{"ParseState: id 2 type 2-val len 4",1,0},{"ParseState: int32 100",1,0},{"ParseState: id 3 type 3-str len 5",1,0},{"ParseState: id 4 type 4-enum len 1",1,0},{"ParseState: byte 2",1,0},{"ParseState: id 5 type 5-bitmap len 1",1,0},{"ParseState: byte 15",1,0},{"CHANNEL_Set channel 1 has changed to 1",1,0},{"CHANNEL_Set channel 2 has changed to 100",1,0},{"CHANNEL_Set channel 4 has changed to 2",1,0},{"executing command setChannel 20 1",1,0},{"CHANNEL_Set channel 20 has changed to 1",1,0},{"55 aa 00 06 00 05 14 01 00 01 01 21",1,0},{"cmd 4 (WiFiReset) len 7",1,0},{"ProcessIncoming: 0x04 replying",1,0},{"55 aa 00 04 00 00 03",1,0},{"cmd 28 (SetTime) len 7",1,0},{"ProcessIncoming: received TUYA_CMD_SET_TIME, so sending back time",1,0},{"MCU time to set: 0",1,0},{"55 aa 00 1c 00 08 01 46 01 01",1,0},{"discarding packet bad expected checksum, expected 0 and got checksum 255",1,0},{"Consumed 4 unwanted non-header byte in Tuya MCU buffer",1,0},{"Skipped data (part) DE AD BE EF",1,0},{"cmd 153 (Unknown) len 7",1,0},{"ProcessIncoming: unhandled type 153",1,0}};
static const char *const c21_tags[] = {"TuyaMCU DP","channels","TuyaMCU errors"};
static const char *const c22_args[] = {"--only-uart","-key","TUYA"};
static const ExpectStr c22_exp[] = {{"CFG_InitAndLoad: Correct config has been loaded",1,0},{"Info:BERRY:Hello 9",1,0}};
static const char *const c23_args[] = {"--only-uart","-key","TUYA"};
static const ExpectStr c23_exp[] = {{"CFG_InitAndLoad: Correct config has been loaded",1,0},{"Info:CMD:Channel 1 is 1",1,0}};
static const PinWant c23_pins[] = {{7,-1},{9,2}};
static const char *const c24_args[] = {"--only-uart","-key","TUYA"};
static const ExpectStr c24_exp[] = {{"CFG_InitAndLoad: Correct config has been loaded",1,0},{"Info:CMD:Channel 2 is 1",1,0}};
static const PinWant c24_pins[] = {{7,2},{9,-1}};
static const char *const c25_args[] = {"--only-uart","-key","TUYA"};
static const ExpectStr c25_exp[] = {{"CFG_InitAndLoad: Correct config has been loaded",1,0},{"Info:CMD:AlwaysPinsDone",1,0}};
static const PinWant c25_pins[] = {{14,2},{15,0},{16,-1}};
static const char *const c26_args[] = {"--only-uart","-key","TUYA"};
static const ExpectStr c26_exp[] = {{"CFG_InitAndLoad: Correct config has been loaded",1,0},{"Info:CMD:Channel 1 is 50",1,0}};
static const PinWant c26_pins[] = {{9,72}};
static const PwmWant c26_pwm[] = {{3,18571,9285}};
static const char *const c27_args[] = {"--only-uart","-key","TUYA"};
static const ExpectStr c27_exp[] = {{"CFG_InitAndLoad: Correct config has been loaded",1,0},{"Info:CMD:Channel 3 is 25",1,0}};
static const PinWant c27_pins[] = {{7,72}};
static const PwmWant c27_pwm[] = {{1,10400,2600}};
static const char *const c28_args[] = {"--only-uart","-key","TUYA"};
static const ExpectStr c28_exp[] = {{"CFG_InitAndLoad: Correct config has been loaded",1,0},{"Info:CMD:Channel 4 is 10",1,0},{"Info:CMD:Channel 5 is 75",1,0}};
static const PinWant c28_pins[] = {{24,72},{26,72}};
static const PwmWant c28_pwm[] = {{4,2600,260},{5,2600,1950}};
static const char *const c29_args[] = {"--only-uart","-key","TUYA"};
static const ExpectStr c29_exp[] = {{"CFG_InitAndLoad: Correct config has been loaded",1,0},{"Info:CMD:Channel 1 is 40",1,0}};
static const PinWant c29_pins[] = {{8,72}};
static const PwmWant c29_pwm[] = {{2,7027,2810}};
static const char *const c30_args[] = {"--only-uart","-key","TUYA"};
static const ExpectStr c30_exp[] = {{"CFG_InitAndLoad: Correct config has been loaded",1,0},{"Info:CMD:Channel 1 is 25",1,0},{"Info:CMD:Channel 2 is 70",1,0}};
static const PinWant c30_pins[] = {{7,72},{9,72}};
static const PwmWant c30_pwm[] = {{1,5777,1444},{3,5777,4043}};
static const char *const c31_args[] = {"--only-uart","-chip","BK7231U"};
static const ExpectStr c31_exp[] = {{"OpenBK7231U, version 1.18.300",1,0},{", idle ",1,0},{"MQTT 0(0), bWifi 0, secondsWithNoPing -1",2,1}};
static const char *const c32_args[] = {"--only-uart","-chip","BK7238"};
static const ExpectStr c32_exp[] = {{"OpenBK7238, version 1.18.300",1,0},{", idle ",1,0},{"MQTT 0(0), bWifi 0, secondsWithNoPing -1",2,1}};
static const char *const c33_args[] = {"--only-uart","-key","TUYA"};
static const ExpectStr c33_exp[] = {{"OpenBK7231N, version 1.18.300",1,0},{"calibration_main over",1,0},{"app_init finished",1,0},{"Info:MAIN:Main_Init_After_Delay done",1,0},{", idle ",1,0},{"MQTT 0(0), bWifi 0, secondsWithNoPing -1",2,1}};
static const char *const c34_args[] = {"--only-uart"};
static const ExpectStr c34_exp[] = {{"OpenBK7231N, version 1.18.300",1,0},{"calibration_main over",1,0},{"app_init finished",1,0},{"Info:MAIN:Main_Init_After_Delay done",1,0},{", idle ",1,0},{"MQTT 0(0), bWifi 0, secondsWithNoPing -1",2,1}};
static const char *const c35_args[] = {"--only-uart","-chip","BK7252"};
static const ExpectStr c35_exp[] = {{"OpenBK7252, version 1.18.300",1,0},{", idle ",1,0},{"MQTT 0(0), bWifi 0, secondsWithNoPing -1",2,1}};
static const char *const c36_args[] = {"--only-uart","-chip","BK7252N"};
static const ExpectStr c36_exp[] = {{"OpenBK7252N, version 1.18.300",1,0},{", idle ",1,0},{"MQTT 0(0), bWifi 0, secondsWithNoPing -1",2,1}};
static const char *const c37_args[] = {"--only-uart","-chip","BK7238"};
static const ExpectStr c37_exp[] = {{"bk_misc_init_start_type",1,0},{"[Flash]init over",1,0},{"SDK Rev: 3.0.70",1,0},{"chip id=7238 device id=21128000",1,0},{"calibration_main over",1,0},{"enter normal mode",1,0}};
static const char *const c38_args[] = {"--only-uart","--trace-pc","0x52c48","--trace-pc","0x4b0b8","--trace-pc","0x1283c"};
static const ExpectStr c38_exp[] = {{"TUYA IOT SDK V:2.3.1",1,0},{"oem_bk7321_bk_1_switch:1.1.3",1,0},{"IO - relay[0]:",1,0},{"Initializing TCP/IP stack",1,0},{"[EMU_PC] reached 0x00052c48",1,0},{"[EMU_PC] reached 0x0004b0b8",1,0},{"[EMU_PC] reached 0x0001283c",1,0}};
static const char *const c39_args[] = {"--only-uart","--ble-core","--wifi-peer","auto","--pcap","{pcap}"};
static const ExpectStr c39_exp[] = {{"chip id=7231a device id=18520001",1,0},{"calibration_main over",1,0},{"rwble_hl_init ok",1,0},{"BLE Rev: B5-3.0.46-P0",1,0},{"gapm_cmp_evt:BLE_STACK_OK",1,0},{"[BLE_ADV]   Complete Name",1,0},{"DREOtf04sbe",1,0},{"iot_uart_read begin",1,0},{"[WIFI] device acts as an access point (it sends beacons): peer takes the station role",1,0},{"[WIFI] associated with 'DREOtf04sbe' (AID 1)",1,0},{"[NET] DHCP: we got 192.168.0.100/255.255.255.0 from the device (router 192.168.0.1)",1,0},{"[NET] ping reply from 192.168.0.1 (seq 1)",1,0}};
static const char *const c39_pcap[] = {"beacon","authentication","association request","association response","data","DREOtf04sbe"};
static const char *const c39_tags[] = {"BLE","BLE core","FIQ"};
static const char *const c40_args[] = {"--only-uart","--uart1-hex","--tuyamcu","-key","TUYA"};
static const ExpectStr c40_exp[] = {{"Initializing TCP/IP stack",1,0},{"STACK INIT OK",1,0},{"CREATE DB SUCCESS",1,0},{"appm start advertising",1,0},{"[UART1/MCU] 55 aa 00 00 00 00 ff",1,0},{"55 aa 00 01 00 00 00",1,0},{"55 aa 00 02 00 00 01",1,0}};
static const char *const c41_args[] = {"--only-uart","-key","TUYA"};
static const ExpectStr c41_exp[] = {{"simple_flash.c:432] key_addr: 0x1ee000",1,0},{"simple_flash.c:500] get key:",1,0},{"0xcb 0x4e 0x3e 0xa4 0x0 0x30 0x9d 0xab 0x65 0x6d 0x8d 0xbf 0xe4 0xb9 0x3f 0x35",1,0},{"TUYA Notice][tuya_main.c:311] **********[oem_bk7231s_light_ty] [2.9.6] compiled at Oct 29 2020 14:38:00**********",1,0},{"tuya_main.c:341] mf_init succ",1,0},{"< TUYA IOT SDK V:1.0.2 BS:40.00_PT:2.2_LAN:3.3_CAD:1.0.2_CD:1.0.0 >",1,0},{"appm start advertising",1,0},{"current product ssid name:tuya_mdev_test1",1,0},{"scan_start_req_handler",1,0}};
static const char *const c41_unexp[] = {"PROD FACTORY START"};
static const char *const c42_args[] = {"--only-uart","-key","TUYA","--wifi-peer","auto","--wifi-ssid","tuya_mdev_test1","--pcap","{pcap}"};
static const ExpectStr c42_exp[] = {{"current product ssid name:tuya_mdev_test1",1,0},{"[WIFI] device acts as a station (an SSID was configured): peer takes the access point role",1,0},{"light_prod.c:1068] PROD FACTORY START!",1,0},{"First prod test!",1,0}};
static const char *const c42_pcap[] = {"beacon","probe request","probe response","tuya_mdev_test1"};
static const char *const c43_args[] = {"--only-uart","-chip","BK7238","--ble-core","--wifi-peer","auto","--pcap","{pcap}"};
static const ExpectStr c43_exp[] = {{"[WIFI] device acts as an access point (it sends beacons): peer takes the station role",1,0},{"[WIFI] found BSS 'SmartLife-AA4C' 00:33:7a:9d:aa:4d ch 6 (open)",1,0},{"[WIFI] associated with 'SmartLife-AA4C' (AID 1)",1,0},{"[NET] DHCP: we got 192.168.176.100/255.255.255.0 from the device (router 192.168.176.1)",1,0},{"[NET] ARP: device 192.168.176.1 is at 00:33:7a:9d:aa:4d",1,0},{"[NET] ping reply from 192.168.176.1 (seq 1)",1,0},{"Tuya LAN protocol 3.4/3.5 announcement, command 0x11",1,0}};
static const char *const c43_pcap[] = {"beacon","authentication","association request","association response","data","SmartLife-AA4C"};
static const char *const c44_args[] = {"--only-uart","--xvr-selfclear","--tuyamcu","--tuyamcu-pid","gqmmtjclqb7reg5p","--tuyamcu-raw","-key","TUYA","--wifi-peer","auto","--pcap","{pcap}"};
static const ExpectStr c44_exp[] = {{"[WIFI] device probes for 'HojnikB WiFi': adopting it as our SSID",1,0},{"[WIFI] station 4c:a9:19:14:71:50 associated to 'HojnikB WiFi' (AID 1)",1,0},{"[NET] DHCP: device 4c:a9:19:14:71:50 got 192.168.1.100",1,0},{"[NET] Tuya LAN broadcast from 192.168.1.100",1,0},{"\"gwId\":\"bf0e0f689d000d56656gmp\"",1,0},{"\"productKey\":\"gqmmtjclqb7reg5p\"",1,0},{"-> 18.185.31.196:8886 (accepted for capture)",1,0},{"[NET] DNS m2.tuyaeu.com -> 10.200.",1,0},{"(m2.tuyaeu.com):8886 (accepted for capture)",1,0},{"sends: TLS ClientHello",2,0},{"client hello: version 1.2, 4 cipher suites (TLS_PSK_WITH_AES_256_CBC_SHA",1,0},{"no server name",1,0},{"answered with TLS_PSK_WITH_AES_256_CBC_SHA, waiting for the key identity",1,0},{"mbedtls_ssl_handshake returned 7600 -30208",1,0}};
static const char *const c44_pcap[] = {"beacon","probe request","authentication","association request","association response","data","HojnikB WiFi"};
static const char *const c45_args[] = {"--only-uart","-key","TUYA","--xvr-selfclear","--wifi-peer","auto"};
static const ExpectStr c45_exp[] = {{"[WIFI] device probes for 'randomssid': adopting it as our SSID",1,0},{"[WIFI] station cc:8c:bf:4d:51:56 associated to 'randomssid' (AID 1)",1,0},{"[NET] DHCP: device cc:8c:bf:4d:51:56 got 192.168.1.100",1,0},{":443 (accepted for capture)",1,0},{"client hello: version 1.2, 8 cipher suites",1,0},{"TLS_PSK_WITH_AES_128_CCM",1,0},{"no server name",1,0},{"answered with TLS_PSK_WITH_AES_256_CBC_SHA, waiting for the key identity",1,0},{"mbedtls_ssl_handshake returned 7600 -30208",1,0},{"resolve mqtt host Fail:m2.tuyaeu.com",1,0}};
static const char *const c46_args[] = {"--only-uart","--tuyamcu","--tuyamcu-pid","9cqoypoauuvhdjq4","--tuyamcu-raw","-key","TUYA","--wifi-peer","auto"};
static const ExpectStr c46_exp[] = {{"hal_machw_enter_monitor_mode",1,0},{"[WIFI] device RX filter 0x7fffffde: monitor mode, every frame on air accepted",1,0},{"[WIFI] device RX filter 0x3503848c: back to normal reception",1,0},{"ssid:SmartLife-FD43, key:, channel: 6",1,0},{"[WIFI] device changed role (it sends beacons)",1,0},{"[WIFI] found BSS 'SmartLife-FD43' 80:64:7c:ed:fd:42 ch 6 (open)",1,0}};
static const char *const c47_args[] = {"--only-uart","--xvr-selfclear","--tuyamcu","--tuyamcu-pid","ztoh9ka787lzjkpy","--tuyamcu-raw","-key","TUYA","--wifi-peer","auto"};
static const ExpectStr c47_exp[] = {{"[WIFI] device probes for 'KN': adopting it as our SSID",1,0},{"[WIFI] station fc:3c:d7:3b:1a:d6 associated to 'KN' (AID 1)",1,0},{"[NET] DHCP: device fc:3c:d7:3b:1a:d6 got 192.168.1.100",1,0},{"[NET] Tuya LAN broadcast from 192.168.1.100",1,0},{"\"gwId\":\"bf9935888c84222522cnd4\"",1,0},{"\"productKey\":\"ztoh9ka787lzjkpy\"",1,0},{"\"version\":\"3.4\"",1,0}};
static const char *const c48_args[] = {"--only-uart","--uart1-hex","-key","TUYA","--xvr-selfclear","--ble-core","--tuyamcu","--tuyamcu-pid","hwebffr1rcfvue3p","--wifi-peer","auto"};
static const ExpectStr c48_exp[] = {{"key_addr: 0x1ee000",1,0},{"get key:",1,0},{"0x6b 0x24 0x0 0x73 0xf4 0x11 0x45 0xed 0x40 0xf1 0x9 0x9f 0x16 0xd3 0xb5 0xc0",1,0},{"STACK INIT OK",1,0},{"[BLE_ADV]   Service Data",1,0},{"mf_init succ",1,0},{"[UART1/MCU] 55 aa 00 01 00 00 00",1,0},{"wifi mcu init. pid:hwebffr1rcfvue3p",1,0},{"gw_cntl.gw_if.product_key:hwebffr1rcfvue3p, input:hwebffr1rcfvue3p",1,0},{"device_init succ",1,0},{"hwebffr1rcfvue3p  uuid=0xA201",1,0},{"[UART1/MCU] 55 aa 00 02 00 01 00 02",1,0}};
static const char *const c48_unexp[] = {"abi/pid/tp invalid","len:33 err"};
static const char *const c48_tags[] = {"BLE","BLE core","FIQ"};
static const char *const c49_args[] = {"--only-uart","--uart1-hex","-key","TUYA","-chip","BK7231N","--wifi-peer","auto","--wifi-ssid","tuya_mdev_test1","--pcap","{pcap}"};
static const ExpectStr c49_exp[] = {{"< TUYA IOT SDK V:2.3.3 BS:40.00_PT:2.2_LAN:3.4_CAD:1.0.5_CD:1.0.0 >",1,0},{"IOT DEFS < WIFI_GW:1 DEBUG:1 KV_FILE:0 SHUTDOWN_MODE:0 LITTLE_END:1 TLS_MODE:2",1,0},{"ENABLE_LAN_ENCRYPTION:1",1,0},{"oem_bk7231n_plug:1.1.17",1,0},{"firmware compiled at Jun 13 2023 20:36:20",1,0},{"key_addr: 0x1ee000",1,0},{"get key:",1,0},{"0xe7 0x10 0xac 0x15 0x88 0x2b 0x38 0x50 0xb2 0x9a 0xd 0xfa 0x35 0xbe 0x99 0x37",1,0},{"ble sdk re_inited",1,0},{"ty bt sdk init success finish",1,0},{"gapm_cmp_evt:BLE_STACK_OK",1,0},{"CREATE DB OK",1,0},{"adv_state:7",1,0},{"mf_init succ",1,0},{"channel 4 Init ok",1,0},{"tuya_ele_hw_init success",1,0},{"app_init finished",1,0},{"current product ssid name:tuya_mdev_test1",1,0},{"scan_start_req_handler",1,0},{"Scan completed in",1,0},{"enter prod mode",1,0}};
static const char *const c49_unexp[] = {"lld.c:404","ASSERTION FAILED"};
static const char *const c49_pcap[] = {"beacon","probe request","probe response","tuya_mdev_test1"};
static const char *const c49_tags[] = {"BLE","BLE core","FIQ"};
static const char *const c50_args[] = {"--only-uart","--uart1-hex","-key","TUYA","--xvr-selfclear"};
static const ExpectStr c50_exp[] = {{"< TUYA IOT SDK V:2.3.1 BS:40.00_PT:2.2_LAN:3.3_CAD:1.0.3_CD:1.0.0 >",1,0},{"firmware compiled at Apr 26 2022 15:12:39",1,0},{"init protected data length 460",1,0},{"key_addr: 0x1ee000",1,0},{"get key:",1,0},{"0x80 0xd9 0xca 0xc2 0x32 0xb4 0x9c 0x30 0x6e 0x8b 0xc2 0x3d 0xf4 0x4c 0x39 0x7c",1,0},{"upd product_id type:0 nqbs1onwskmmmaac",1,0},{"wf_nw_status_cb,wifi_status:0",1,0}};
static const char *const c51_args[] = {"--only-uart","-chip","BK7238"};
static const ExpectStr c51_exp[] = {{"device_id=0x21128000",1,0},{"[FUNC]rwnxl_init",1,0},{"[FUNC]calibration_main",1,0},{"tuya_upgrade_main need upgrade\?",1,0},{"sa_crc_r[18] sa_crc_g[18] sa_crc_b[18]",1,0},{"val_cur:2835, val_max:7000, val_min:1050",1,0},{"startup default",1,0},{"light basic service init OK",1,0},{"upd product_id type:1 keyw97qjscyfmsyn",1,0},{"initial BLE...",1,0}};
static const PinWant c51_pins[] = {{0,120},{7,-1},{10,120}};
static const char *const c51_tags[] = {"BLE"};
static const char *const c52_args[] = {"--only-uart","-chip","BK7238","--ble-core"};
static const ExpectStr c52_exp[] = {{"device_id=0x21128000",1,0},{"initial BLE...",1,0},{"ble_gap.c:2505] Start Adv",1,0},{"ble adv updated",1,0},{"__devos_init_evt success",1,0}};
static const char *const c52_tags[] = {"BLE","BLE core","FIQ"};
static const char *const c53_args[] = {"--only-uart","--ble-core"};
static const ExpectStr c53_exp[] = {{"Vesync SDK version: v1.2.1",1,0},{"ble mac:fc-58-4a-a1-d3-c3",1,0},{"rwble_hl_init ok",1,0},{"gapm_cmp_evt:GAPM_GEN_RAND_NB",2,0},{"gapm_cmp_evt:BLE_STACK_OK",1,0},{"[THD]vesync_ble_task",1,0},{"company=0x06D0",1,0},{"ff d0 06 01 c3 d3 a1 4a 58 fc",1,0},{"REG_PWM_GROUP_CTRL= 0x1c14",1,0},{"[THD]netcfg_task",1,0},{"wpa rx E SCAN_STARTED",1,0}};
static const char *const c53_unexp[] = {"lld.c","create cmd db fail","ble is not ready"};
static const char *const c53_tags[] = {"BLE","BLE core","FIQ"};
static const char *const c54_args[] = {"--only-uart","--uart1-hex","--ble-core","--tuyamcu","--tuyamcu-pid","qasrgndvbbtrula9","--tuyamcu-raw","-key","TUYA"};
static const ExpectStr c54_exp[] = {{"bk7231n_common_user_config_ty:2.1.17",1,0},{"[ble_appm_send_gapm_reset_cmd]",1,0},{"gapm_cmp_evt:GAPM_GEN_RAND_NB",2,0},{"gapm_cmp_evt:BLE_STACK_OK",1,0},{"[ble_appm_start_advertising]",1,0},{"mf_init succ",1,0},{"uuid=0xA201",1,0},{"qasrgndvbbtrula9  uuid=0xA201",1,0},{"[UART1/MCU] 55 aa 00 00 00 00 ff",1,0},{"55 aa 00 01 00 00 00",1,0},{"55 aa 00 02 00 00 01",1,0},{"product_key:qasrgndvbbtrula9",1,0},{"55 aa 00 03 00 01 01 04",1,0}};
static const char *const c54_unexp[] = {"lld.c","prod len"};
static const char *const c54_tags[] = {"BLE","BLE core","FIQ"};
static const char *const c55_args[] = {"--only-uart","--uart1-hex","--tuyamcu","--tuyamcu-pid","wcihaluccfsoayqa","--tuyamcu-raw","-key","TUYA"};
static const ExpectStr c55_exp[] = {{"bk7231t_common_user_config_ty:1.1.80",1,0},{"key_addr: 0x1ee000",1,0},{"mf_init succ",1,0},{"STACK INIT OK",1,0},{"CREATE DB SUCCESS",1,0},{"appm start advertising",1,0},{"[UART1/MCU] 55 aa 00 00 00 00 ff",1,0},{"55 aa 00 01 00 00 00",1,0},{"product_key:wcihaluccfsoayqa",1,0},{"wifi mcu init. pid:wcihaluccfsoayqa firmwarekey:key34ak4q5rmrkef v1:1.1.80",1,0},{"gw_cntl.gw_if.product_key:wcihaluccfsoayqa, input:wcihaluccfsoayqa",1,0},{"55 aa 00 02 00 00 01",1,0}};
static const char *const c55_tags[] = {"BLE","TuyaMCU"};
static const char *const c56_args[] = {"--only-uart","--uart1-hex","--tuyamcu","--tuyamcu-pid","i3k1tsas1esbewba","--tuyamcu-raw","-key","TUYA"};
static const ExpectStr c56_exp[] = {{"bk7231n_common_user_config_ty:3.1.28",1,0},{"have actived over 15 min, not enter mf_init",1,0},{"mf_init succ",1,0},{"current kv info, addr: 1ed000, cnt: 97, is valid: 0",1,0},{"mirror kv info, addr: 1cf000, cnt: 97, is valid: 0",1,0},{"[UART1/MCU] 55 aa 00 00 00 00 ff",1,0},{"55 aa 00 01 00 00 00",1,0},{"gw_cntl->gw_if.product_key:i3k1tsas1esbewba, input:i3k1tsas1esbewba",1,0},{"55 aa 00 02 00 00 01",1,0}};
static const char *const c57_args[] = {"--only-uart","--uart1-hex","--tuyamcu","--tuyamcu-pid","9cqoypoauuvhdjq4","--tuyamcu-raw","-key","TUYA"};
static const ExpectStr c57_exp[] = {{"< TuyaOS V:3.11.12 BS:40.00_PT:2.3_LAN:3.5_CAD:1.0.5_CD:1.0.0 >",1,0},{"mf_init succ",1,0},{"current kv info, addr: 1ed000, cnt: 8, is valid: 0",1,0},{"mirror kv info, addr: 1cf000, cnt: 8, is valid: 0",1,0},{"0x53 0xc1 0xb1 0x85 0xe3 0xbf 0xb6 0x3 0xe8 0x43 0xad 0xfb 0x83 0x82 0x69 0xdf",1,0},{"read baud rate 9600",1,0},{"[UART1/MCU] 55 aa 00 00 00 00 ff",1,0},{"55 aa 00 01 00 00 00",1,0},{"gw_cntl->gw_if.product_key:9cqoypoauuvhdjq4, input:9cqoypoauuvhdjq4",1,0},{"55 aa 00 02 00 00 01",1,0}};
static const char *const c58_args[] = {"--only-uart","--uart1-hex","--tuyamcu","--tuyamcu-pid","wifech3utowiyknu","--tuyamcu-raw","-key","TUYA"};
static const ExpectStr c58_exp[] = {{"< TuyaOS V:3.11.12 BS:40.00_PT:2.3_LAN:3.5_CAD:1.0.5_CD:1.0.0 >",1,0},{"mf_init succ",1,0},{"current kv info, addr: 1ed000, cnt: 16, is valid: 0",1,0},{"mirror kv info, addr: 1cf000, cnt: 16, is valid: 0",1,0},{"0xfe 0x2f 0xe2 0x48 0x9 0x3f 0x8d 0x4a 0xad 0x1a 0xc1 0xf3 0x79 0x2f 0x56 0xf6",1,0},{"read baud rate 9600",1,0},{"[UART1/MCU] 55 aa 00 00 00 00 ff",1,0},{"55 aa 00 01 00 00 00",1,0},{"upd product_id type:0 wifech3utowiyknu",1,0},{"55 aa 00 02 00 00 01",1,0}};
static const char *const c59_args[] = {"--only-uart","--uart1-hex","-key","TUYA"};
static const ExpectStr c59_exp[] = {{"CFG_InitAndLoad: Correct config has been loaded",1,0},{"Started BL0942.",1,0},{"[UART1/MCU] a8 1d 55 00 00 e5 a8 19 8f 00 00 af",1,0},{"[UART1/MCU] 58 aa",2,1}};
static const char *const c60_args[] = {"--only-uart","--uart1-hex","--tuyamcu","--tuyamcu-pid","o9a6at9cyfchb47y","--tuyamcu-raw","-key","TUYA"};
static const ExpectStr c60_exp[] = {{"< TuyaOS V:3.8.18 BS:40.00_PT:2.3_LAN:3.5_CAD:1.0.5_CD:1.0.0 >",1,0},{"mf_init succ",1,0},{"current kv info, addr: 1ed000, cnt: 16, is valid: 0",1,0},{"mirror kv info, addr: 1cf000, cnt: 16, is valid: 0",1,0},{"0x9a 0xe5 0x6a 0x9b 0x84 0x26 0x71 0x1a 0x9f 0x6a 0xdb 0x77 0x34 0xf9 0xae 0xf7",1,0},{"read baud rate 9600",1,0},{"[UART1/MCU] 55 aa 00 00 00 00 ff",1,0},{"55 aa 00 01 00 00 00",1,0},{"gw_cntl->gw_if.product_key:o9a6at9cyfchb47y, input:o9a6at9cyfchb47y",1,0},{"55 aa 00 02 00 00 01",1,0}};
static const char *const c61_args[] = {"--only-uart","--uart1-hex","--tuyamcu","--tuyamcu-pid","dsmsam7xpb3ht7rl","--tuyamcu-raw","-key","TUYA"};
static const ExpectStr c61_exp[] = {{"bk7231n_common_user_config_ty:3.1.17",1,0},{"mf_init succ",1,0},{"[UART1/MCU] 55 aa 00 00 00 00 ff",1,0},{"55 aa 00 01 00 00 00",1,0},{"gw_cntl->gw_if.product_key:dsmsam7xpb3ht7rl, input:dsmsam7xpb3ht7rl",1,0},{"55 aa 00 02 00 00 01",1,0}};
static const char *const c62_args[] = {"--only-uart","--uart1-hex","--tuyamcu","--tuyamcu-pid","ni2ztksbndubd9rf","--tuyamcu-raw","-key","TUYA"};
static const ExpectStr c62_exp[] = {{"bk7231n_common_user_config_ty:3.1.28",1,0},{"have actived over 15 min, not enter mf_init",1,0},{"mf_init succ",1,0},{"[UART1/MCU] 55 aa 00 00 00 00 ff",1,0},{"55 aa 00 01 00 00 00",1,0},{"gw_cntl->gw_if.product_key:ni2ztksbndubd9rf, input:ni2ztksbndubd9rf",1,0},{"55 aa 00 02 00 00 01",1,0}};
static const char *const c63_args[] = {"--only-uart","--uart1-hex","--xvr-selfclear","--tuyamcu","--tuyamcu-pid","tjtigg991kvoiiqi","--tuyamcu-raw","-key","TUYA"};
static const ExpectStr c63_exp[] = {{"bk7231n_common_user_config_ty:2.1.17",1,0},{"mf_init succ",1,0},{"[UART1/MCU] 55 aa 00 00 00 00 ff",1,0},{"55 aa 00 01 00 00 00",1,0},{"55 aa 00 02 00 00 01",1,0}};
static const char *const c64_args[] = {"--only-uart","--uart1-hex","--xvr-selfclear","--tuyamcu","--tuyamcu-pid","gqmmtjclqb7reg5p","--tuyamcu-raw","-key","TUYA"};
static const ExpectStr c64_exp[] = {{"bk7231n_common_user_config_ty:2.0.2",1,0},{"mf_init succ",1,0},{"[UART1/MCU] 55 aa 00 00 00 00 ff",1,0},{"55 aa 00 01 00 00 00",1,0},{"55 aa 00 02 00 00 01",1,0}};
static const char *const c65_args[] = {"--only-uart","--uart1-hex","--xvr-selfclear","--tuyamcu","--tuyamcu-pid","ztoh9ka787lzjkpy","--tuyamcu-raw","-key","TUYA"};
static const ExpectStr c65_exp[] = {{"bk7231n_common_user_config_ty:2.1.17",1,0},{"mf_init succ",1,0},{"[UART1/MCU] 55 aa 00 00 00 00 ff",1,0},{"55 aa 00 01 00 00 00",1,0},{"55 aa 00 02 00 00 01",1,0}};
static const char *const c66_args[] = {"--only-uart","-key","TUYA"};
static const ExpectStr c66_exp[] = {{"key_addr: 0x1ee000",1,0},{"oem_bk7231s_light_ty_oldDp:1.1.1",1,0},{"< TUYA IOT SDK V:1.0.2 BS:40.00_PT:2.2_LAN:3.3_CAD:1.0.2_CD:1.0.0 >",1,0},{"mf_init succ",1,0}};
static const PinWant c66_pins[] = {{24,72},{26,72}};
static const PwmWant c66_pwm[] = {{4,8666,0},{5,8666,8666}};
static const char *const c67_args[] = {"--only-uart","-key","TUYA"};
static const ExpectStr c67_exp[] = {{"< TUYA IOT SDK V:2.0.0 BS:30.06_PT:2.2_LAN:3.3_CAD:1.0.2_CD:1.0.0 >",1,0},{"mf_init succ",1,0},{"have actived over 15 min, not enter mf_init",1,0}};
static const PinWant c67_pins[] = {{6,72},{7,72},{8,72},{24,72},{26,72}};
static const PwmWant c67_pwm[] = {{0,26000,0},{1,26000,0},{2,26000,-1},{4,26000,0},{5,26000,0}};
static const char *const c68_args[] = {"--only-uart","-key","TUYA"};
static const ExpectStr c68_exp[] = {{"oem_bk7231s_ty_ffc_db_ldv",1,0},{"< TUYA IOT SDK V:2.0.0 BS:30.06_PT:2.2_LAN:3.3_CAD:1.0.2_CD:1.0.0 >",1,0},{"mf_init succ",1,0}};
static const PinWant c68_pins[] = {{8,72},{9,72}};
static const PwmWant c68_pwm[] = {{2,13000,0},{3,13000,13000}};
static const char *const c69_args[] = {"--only-uart","-key","TUYA"};
static const ExpectStr c69_exp[] = {{"[oem_bk7231s_light_ty] [1.1.2]",1,0},{"< TUYA IOT SDK V:2.0.0 BS:30.06_PT:2.2_LAN:3.3_CAD:1.0.2_CD:1.0.0 >",1,0},{"mf_init succ",1,0}};
static const PinWant c69_pins[] = {{6,72},{7,72},{8,72},{24,72},{26,72}};
static const PwmWant c69_pwm[] = {{0,43333,0},{1,43333,0},{2,43333,0},{4,43333,0},{5,43333,0}};
static const char *const c70_args[] = {"--only-uart","-key","TUYA"};
static const ExpectStr c70_exp[] = {{"[oem_bk7231s_strip_ty] [1.0.5]",1,0},{"< TUYA IOT SDK V:2.0.0 BS:30.05_PT:2.2_LAN:3.3_CAD:1.0.2_CD:1.0.0 >",1,0},{"mf_init succ",1,0}};
static const PinWant c70_pins[] = {{9,72},{24,72},{26,72}};
static const PwmWant c70_pwm[] = {{3,26000,0},{4,26000,0},{5,26000,0}};
static const char *const c71_args[] = {"--only-uart","-key","TUYA","--xvr-selfclear"};
static const ExpectStr c71_exp[] = {{"< TUYA IOT SDK V:2.3.3 BS:40.00_PT:2.2_LAN:3.4_CAD:1.0.5_CD:1.0.0 >",1,0},{"oem_bk7231n_light_ty:1.3.21",1,0},{"have actived over 15 min, not enter mf_init",1,0},{"wpa_supplicant_req_scan",1,0},{"gw is long time(60) disconnect(stat:5), reconnect!!",1,0}};
static const PinWant c71_pins[] = {{6,72},{7,72},{9,72},{24,72},{26,72}};
static const PwmWant c71_pwm[] = {{0,26000,0},{1,26000,0},{3,26000,0},{4,26000,0},{5,26000,0}};
static const char *const c72_args[] = {"--only-uart","-key","TUYA"};
static const ExpectStr c72_exp[] = {{"< TuyaOS V:3.3.40 BS:40.00_PT:2.3_LAN:3.5_CAD:1.0.5_CD:1.0.0 >",1,0},{"mf_init succ",1,0}};
static const PinWant c72_pins[] = {{6,72},{7,72},{8,72},{24,72},{26,72}};
static const PwmWant c72_pwm[] = {{0,5200,0},{1,5200,0},{2,5200,0},{4,5200,0},{5,5200,0}};
static const char *const c73_args[] = {"--only-uart","-key","TUYA"};
static const ExpectStr c73_exp[] = {{"< TuyaOS V:3.3.43 BS:40.00_PT:2.3_LAN:3.5_CAD:1.0.5_CD:1.0.0 >",1,0},{"mf_init succ",1,0}};
static const PinWant c73_pins[] = {{6,72},{7,72},{8,72},{24,72},{26,72}};
static const PwmWant c73_pwm[] = {{0,1625,0},{1,1625,0},{2,1625,0},{4,1625,0},{5,1625,0}};
static const char *const c74_args[] = {"--only-uart","-key","TUYA"};
static const ExpectStr c74_exp[] = {{"< TuyaOS V:3.3.44 BS:40.00_PT:2.3_LAN:3.5_CAD:1.0.5_CD:1.0.0 >",1,0},{"oem_bk7231n_slide_strip_ty:1.3.3",1,0},{"mf_init succ",1,0},{"key_addr: 0x1ee000",1,0},{"get key:",1,0},{"0x34 0x8b 0xdd 0xe7 0x7b 0x9c 0x63 0xe0 0x44 0x1e 0xa7 0x1f 0x21 0x6b 0x5 0xc",1,0}};
static const PinWant c74_pins[] = {{6,72},{8,72}};
static const PwmWant c74_pwm[] = {{0,26000,0},{2,26000,0}};
static const char *const c75_args[] = {"--only-uart","-chip","BK7238","--ble-core"};
static const ExpectStr c75_exp[] = {{"device_id=0x21128000",1,0},{"< TuyaOS V:3.11.24 BS:40.00_PT:2.3_LAN:3.5_CAD:1.0.5_CD:1.0.0 >",1,0},{"name:oem_t1_light_ty:1.0.43",1,0},{"key_addr: 0x1f5000",1,0},{"0x4d 0x5d 0xbf 0x3 0x35 0x65 0x17 0x4c 0x91 0x38 0x56 0x78 0x19 0x4 0xf 0x58",1,0},{"have actived over 15 min, not enter mf_init",1,0},{"mf_init succ",1,0},{"wifi soc init. pid:keytfesxxsqewnqy firmwarekey:keytfesxxsqewnqy ver:1.0.43",1,0},{"ssid:SmartLife-2074, key:, channel: 6",1,0},{"uuid=0xFD50",1,0},{"keytfesxxsqewnqy  uuid=0xFD50",1,0},{"ble adv updated",1,0},{"tfm_timing_storage.c:168] storage sucess",1,0}};
static const char *const c75_unexp[] = {"uf_open light_data err","Err write addr","bk_reboot"};
static const PinWant c75_pins[] = {{6,72},{26,72}};
static const PwmWant c75_pwm[] = {{0,26000,14170},{5,26000,0}};
static const char *const c75_tags[] = {"BLE","BLE core","FIQ"};
static const char *const c76_args[] = {"--only-uart","--uart1-hex","-chip","BK7238","--ble-core","--tuyamcu","--tuyamcu-pid","jbe3snv4tki8oo9c","--tuyamcu-raw","--wifi-peer","auto"};
static const ExpectStr c76_exp[] = {{"device_id=0x21128000",1,0},{"< TuyaOS V:3.11.16 BS:40.00_PT:2.3_LAN:3.5_CAD:1.0.5_CD:1.0.0 >",1,0},{"t1_common_user_config_ir_ty sw ver:1.0.1",1,0},{"key_addr: 0x1f5000",1,0},{"0x88 0xd3 0x2a 0x22 0x15 0xba 0x68 0x4b 0xca 0x9b 0x19 0x65 0x68 0xbf 0x16 0xd7",1,0},{"have actived over 15 min, not enter mf_init",1,0},{"mf_init succ",1,0},{"read baud rate 9600",1,0},{"[UART1/MCU] 55 aa 00 00 00 00 ff 55 aa 00 01 00 00 00 55 aa 00 02 00 00 01",1,0},{"gw_cntl->gw_if.product_key:jbe3snv4tki8oo9c, input:jbe3snv4tki8oo9c",1,0},{"gw_cntl->gw_if.firmware_key:keyny8wdynkekrgw, input:keyny8wdynkekrgw",1,0},{"enter success_proc",1,0},{"uuid=0xFD50",1,0},{"jbe3snv4tki8oo9c  uuid=0xFD50",1,0},{"ble adv updated",1,0},{"[UART1/MCU] 55 aa 00 03 00 01 01 04",1,0},{"[WIFI] found BSS 'SmartLife-C8F2'",1,0},{"[WIFI] associated with 'SmartLife-C8F2' (AID 1)",1,0},{"[NET] DHCP: we got 192.168.176.100/255.255.255.0 from the device (router 192.168.176.1)",1,0},{"[NET] ping reply from 192.168.176.1",1,0},{"Tuya LAN broadcast from 192.168.176.1:6667: Tuya LAN protocol 3.4/3.5 announcement, command 0x11, 265 bytes",1,0}};
static const PinWant c76_pins[] = {{0,120},{1,120},{6,-1},{10,120},{11,120},{26,-1}};
static const char *const c76_tags[] = {"BLE","BLE core","FIQ"};
static const char *const c77_args[] = {"--only-uart","-chip","BK7238","--ble-core","--wifi-peer","auto","--pcap","{pcap}","--pcap-ip","{pcap_ip}"};
static const ExpectStr c77_exp[] = {{"device_id=0x21128000",1,0},{"sdk_info:< TuyaOS V:3.8.31 BS:40.00_PT:2.3_LAN:3.5_CAD:1.0.5_CD:1.0.0 >",1,0},{"name:oem_t1_irbox_mol_ty:0.0.5",1,0},{"key_addr: 0x1f5000",1,0},{"gw_cntl->gw_wsm.stat:2",1,0},{"have actived over 15 min, not enter mf_init",1,0},{"mf_init succ",1,0},{"wifi soc init. pid:keycmhjv873dhvsn firmwarekey:keycmhjv873dhvsn ver:0.0.5",1,0},{"already bind",1,0},{"uuid=0xFD50",1,0},{"50 fd 4b 0c 01 10 d7 be 6e",1,0},{"ble adv updated",1,0},{"[WIFI] device acts as a station (it authenticates): peer takes the access point role",1,0},{"[WIFI] device probes for 'DOM-1': adopting it as our SSID",1,0},{"[WIFI] station 80:64:7c:21:ff:7b associated to 'DOM-1'",1,0},{"writed fci to flash ssid=DOM-1",1,0},{"[NET] DHCP: device 80:64:7c:21:ff:7b got 192.168.1.100 (gateway/DNS 192.168.1.1)",1,0},{"connect to mqtt broker mqtts://m3-eu.lifeaiot.com:8883 port 8883",1,0},{"[NET] DNS m3-eu.lifeaiot.com -> 10.200.0.1 (offline sinkhole)",1,0},{"sends: TLS ClientHello (no server name)",1,0},{"client hello: version 1.2, 4 cipher suites (TLS_PSK_WITH_AES_128_GCM_SHA256, TLS_PSK_WITH_AES_128_CBC_SHA256, 0xc037, 0x00ff), no server name",1,0},{"answered with TLS_PSK_WITH_AES_128_GCM_SHA256, waiting for the key identity",1,0},{"pre-shared key identity: 49 bytes",1,0},{"handshake authenticated; serving encrypted MQTT offline",1,0},{"connected to the offline broker",1,0},{"MQTT SUBSCRIBE smart/device/in/",1,0},{"MQTT PUBLISH smart/device/out/",1,0}};
static const char *const c77_unexp[] = {"mbedtls_ssl_handshake returned 0x7280","client Finished authentication failed"};
static const PinWant c77_pins[] = {{0,120},{1,120},{8,60},{9,60},{10,120},{11,120},{26,0}};
static const char *const c77_pcap[] = {"beacon","probe request","authentication","association request","association response","data","DOM-1","m3-eu"};
static const char *const c77_ippcap[] = {"m3-eu","lifeaiot"};
static const char *const c77_tags[] = {"BLE","BLE core","FIQ"};
static const char *const c78_args[] = {"--only-uart","-key","TUYA"};
static const ExpectStr c78_exp[] = {{"< TUYA IOT SDK V:2.0.0 BS:30.06_PT:2.2_LAN:3.3_CAD:1.0.2_CD:1.0.0 >",1,0},{"oem_bk7231s_dltj:1.0.3",1,0},{",module: is WB2S",1,0},{"have actived over 15 min, not enter mf_init",1,0},{"mf_init succ",1,0},{"gw_cntl.gw_if.product_key:keym9qkuywghyrvs, input:keym9qkuywghyrvs",1,0},{"dltj_run_int",1,0},{"channel 0 Init ok",1,0},{"CREATE DB SUCCESS",1,0}};
static const char *const c79_args[] = {"--only-uart","-key","TUYA"};
static const ExpectStr c79_exp[] = {{"< TUYA IOT SDK V:1.0.1 BS:40.00_PT:2.2_LAN:3.3_CAD:1.0.2_CD:1.0.0 >",1,0},{"oem_bk7231s_rnd_zero_dctrl_switch:1.0.4",1,0},{"module:WB3S-IPEX",1,0},{"gw_cntl.gw_if.product_key:key7axydcvmea3x9, input:key7axydcvmea3x9",1,0},{"Channel [0] config !!!",1,0},{"IO - relay[0]:\tpin-6\ttype high",1,0},{"IO - button[0]:\tpin-7\ttype low",1,0},{"IO - wifi_stat:\tpin-10\ttype high",1,0},{"CREATE DB SUCCESS",1,0},{"appm start advertising",1,0}};
static const char *const c80_args[] = {"--only-uart","-key","TUYA","-chip","BK7231N","--wifi-peer","auto"};
static const ExpectStr c80_exp[] = {{"BleSwitch=1",1,0},{"first bright up",1,0},{"mf_init succ",1,0},{"< TuyaOS V:3.3.44 BS:40.00_PT:2.3_LAN:3.5_CAD:1.0.5_CD:1.0.0 >",1,0},{"oem_bk7231n_light_ty:1.6.0",1,0},{"wifi soc init. pid:key8u54q9dtru5jw firmwarekey:key8u54q9dtru5jw ver:1.6.0",1,0},{"gw_cntl->gw_if.product_key:key8u54q9dtru5jw, input:key8u54q9dtru5jw",1,0},{"enter success_proc",1,0},{"ffc remote OK",1,0},{"BLE IP Rev: 76f0dd2",1,0},{"tkl_hci_callback_register",1,0},{"llm_init:312",2,0},{"ble sdk re_inited",1,0},{"ty bt sdk init success finish",1,0},{"app_init finished",1,0},{"wpa_supplicant_req_scan",1,0},{"[BLE_ADV] EM 0x00910000",1,0},{"Start Adv",1,0},{"ble adv && resp changed",1,0},{"[WIFI] device acts as a station",1,0},{"[NET] DHCP: device",1,0},{"mqtt ip:",1,0},{"sends: TLS ClientHello",1,0},{"pre-shared key identity: 32 bytes",1,0},{"handshake authenticated; serving encrypted MQTT offline",1,0},{"connected to the offline broker",1,0},{"MQTT SUBSCRIBE ",1,0},{"MQTT PUBLISH ",1,0}};
static const char *const c80_unexp[] = {"mbedtls_ssl_handshake returned 0x7280","client Finished authentication failed"};
static const char *const c80_tags[] = {"BLE","BLE core","FIQ"};
static const char *const c81_args[] = {"--only-uart","-key","TUYA","-chip","BK7231N","--wifi-peer","auto","--wifi-ssid","tuya_mdev_test1","--pcap","{pcap}"};
static const ExpectStr c81_exp[] = {{"oem_bk7231n_irbox_mol_ty:1.2.1",1,0},{"rwble_hl_init ok",1,0},{"gapm_cmp_evt:GAPM_GEN_RAND_NB",2,0},{"gapm_cmp_evt:BLE_STACK_OK",1,0},{"CREATE DB SUCCESS",1,0},{"[BLE_ADV]   Service Data",1,0},{"uuid=0xA201",1,0},{"mf_init succ",1,0},{"ir_send =7",1,0},{"ir_recv =8",1,0},{"gw_cntl.gw_if.product_key:key54vrth5askhsj, input:key54vrth5askhsj",1,0},{"ty_uart_ir_main_components_version:1.0.9",1,0},{"app_init finished",1,0},{"scan_start_req_handler",1,0},{"Soft_AP_start",1,0},{"hal_machw_enter_monitor_mode",1,0},{"wifi_stat:1",1,0}};
static const char *const c81_unexp[] = {"assert_param","lld.c:404","ASSERTION FAILED"};
static const char *const c81_pcap[] = {"beacon","probe request","probe response","authentication","tuya_mdev_test1","filter"};
static const char *const c81_tags[] = {"BLE","BLE core","FIQ","WiFi MAC"};
static const char *const c82_args[] = {"--only-uart","-key","TUYA","--xvr-selfclear","--ble-core","--wifi-peer","auto","--wifi-ssid","tuya_mdev_test1","--pcap","{pcap}"};
static const ExpectStr c82_exp[] = {{"< TUYA IOT SDK V:2.3.3 BS:40.00_PT:2.2_LAN:3.4_CAD:1.0.5_CD:1.0.0 >",1,0},{"oem_bk7231n_dctrl_switch:1.3.5",1,0},{"gapm_cmp_evt:GAPM_GEN_RAND_NB",2,0},{"gapm_cmp_evt:BLE_STACK_OK",1,0},{"CREATE DB OK",1,0},{"[ble_appm_start_advertising]",1,0},{"mf_init succ",1,0},{"module:CB3S",1,0},{"channel init success",1,0},{"app_init finished",1,0},{"current product ssid name:tuya_mdev_test1",1,0},{"scan_start_req_handler",1,0},{"The current rssi is -45dB",1,0},{"product test mode",1,0}};
static const char *const c82_unexp[] = {"assert_param"};
static const char *const c82_pcap[] = {"beacon","probe request","probe response","tuya_mdev_test1"};
static const char *const c82_tags[] = {"BLE","BLE core","FIQ"};
static const char *const c83_args[] = {"--only-uart","-chip","BK7238","--ble-core","--wifi-peer","auto"};
static const ExpectStr c83_exp[] = {{"device_id=0x21128000",1,0},{"< TuyaOS V:3.11.16 BS:40.00_PT:2.3_LAN:3.5_CAD:1.0.5_CD:1.0.0 >",1,0},{"oem_t1_cutout_self_switch:1.1.0",1,0},{",module: is T1-U",1,0},{",over_cur: is 63000",1,0},{",rl_on1_pin: is 8",1,0},{",rl_off1_pin: is 6",1,0},{",bt1_pin: is 17",1,0},{",led1_pin: is 21",1,0},{"product have measure , chip is 3",1,0},{"key_addr: 0x1f5000",1,0},{"calibration_main over",1,0},{"app_init finished",1,0},{"mf_init succ",1,0},{"channel 0 Init ok",1,0},{"tuya_ele_hw_init success",1,0},{"initial BLE...",1,0},{"rwip_heap_env addr:",1,0},{"enter normal mode",1,0},{"ty bt sdk init success finish",1,0},{"dltj start success",1,0},{"tuya_measure_chip ok",1,0},{"connect to mqtt broker mqtts://m3.tuyacn.com:8886 port 8886",1,0},{"pre-shared key identity: 49 bytes",1,0},{"handshake authenticated; serving encrypted MQTT offline",1,0},{"connected to the offline broker",1,0},{"MQTT SUBSCRIBE smart/device/in/",1,0},{"MQTT PUBLISH smart/device/out/",1,0}};
static const char *const c83_unexp[] = {"mbedtls_ssl_handshake returned 0x7280","client Finished authentication failed"};
static const char *const c83_tags[] = {"BLE","BLE core","FIQ"};
static const char *const c84_args[] = {"--only-uart","-chip","BK7238","--ble-core","--wifi-peer","auto","--pcap","{pcap}"};
static const ExpectStr c84_exp[] = {{"device_id=0x21128000",1,0},{"< TuyaOS V:3.11.16 BS:40.00_PT:2.3_LAN:3.5_CAD:1.0.5_CD:1.0.0 >",1,0},{"name:t1_kaijika:1.0.4",1,0},{",module: is CBU",1,0},{",ch_num: is 3",1,0},{",zero_io_pin: is 24",1,0},{",bt1_pin: is 21",1,0},{",bt2_pin: is 8",1,0},{"mf_init successfully",1,0},{"channel 0 Init ok",1,0},{"channel 1 Init ok",1,0},{"channel 2 Init ok",1,0},{"wifi soc init. pid:jlicasd8h9mzckrz firmwarekey:NULL ver:1.0.4",1,0},{"enter success_proc",1,0},{"device_init ok",1,0},{"ssid:SmartLife-D1BF, key:, channel: 6",1,0},{"BLE HCI Version = 11",1,0},{"Stack sync",1,0},{"[BLE_ADV]   Service Data",1,0},{"jlicasd8h9mzckrz  uuid=0xFD50",1,0},{"ble adv updated",1,0},{"[WIFI] associated with 'SmartLife-D1BF' (AID 1)",1,0},{"[NET] DHCP: we got 192.168.176.100/255.255.255.0 from the device (router 192.168.176.1)",1,0},{"[NET] ping reply from 192.168.176.1 (seq 1)",1,0}};
static const char *const c84_pcap[] = {"beacon","authentication","association request","association response","data","SmartLife-D1BF"};
static const char *const c84_tags[] = {"BLE","BLE core","FIQ"};
static const char *const c85_args[] = {"--only-uart","-chip","BK7252N"};
static const ExpectStr c85_exp[] = {{"RT-Thread Flash Abstraction Layer (V0.4.0) initialize success.",1,0},{"ROMFS File System initialized!",1,0},{"Enter normal mode...",1,0},{"bk_wlan_app_init finished",1,0},{"rf_thread_init ok",1,0},{"xc_main, sw_ver=3.00.21.01_250723!",1,0},{"button node have Button1",1,0},{"button node have Button2",1,0},{"---adc channel:0---",1,0},{"return xdev_opt, magic=11223344",1,0},{"IOTSDK Ver = 3.00.42.01_241230",1,0},{"power status is off, wait to power on",1,0},{"wake up time out to enter deep sleep",1,0},{"---enter_deep sleep",1,0},{"---enter deep sleep :wake up with gpio 0~31 ps: 0x2020 0x20",1,0}};
static const char *const c86_args[] = {"--only-uart","-key","TUYA"};
static const ExpectStr c86_exp[] = {{"< TUYA IOT SDK V:1.0.2 BS:40.00_PT:2.2_LAN:3.3_CAD:1.0.2_CD:1.0.0 >",1,0},{"oem_bk7231s_rnd_zero_dctrl_switch:1.0.7",1,0},{"STACK INIT OK",1,0},{"CREATE DB SUCCESS",1,0},{"appm start advertising",1,0},{"mf_init succ",1,0},{"module:WB3S",1,0},{"ch_num:3",1,0},{"IO - relay[0]:\tpin-6\ttype high",1,0},{"IO - button[0]:\tpin-24\ttype low",1,0},{"IO - led[0]:\tpin-14\ttype high",1,0},{"IO - relay[1]:\tpin-9\ttype high",1,0},{"IO - button[1]:\tpin-10\ttype low",1,0},{"IO - led[1]:\tpin-26\ttype high",1,0},{"IO - relay[2]:\tpin-7\ttype high",1,0},{"IO - button[2]:\tpin-8\ttype low",1,0},{"IO - led[2]:\tpin-23\ttype high",1,0},{"device_init ok",1,0},{"scan_start_req_handler",1,0}};
static const char *const c87_args[] = {"--only-uart","--ble-core"};
static const ExpectStr c87_exp[] = {{"SDK Rev: 3.0.46",1,0},{"chip id=7231a device id=18520001",1,0},{"rwble_hl_init ok",1,0},{"gapm_cmp_evt:BLE_STACK_OK",1,0},{"[BLE_ADV]   Complete Name",1,0},{"DNS_D07_410B8E",1,0},{"5.0.38",1,0},{"ap_ssid:DNS_D07_410B8E",1,0},{"water_hight     1",1,0},{"water_mode      1",1,0},{"RGB mode        2",1,0},{"RGB color       00FF00",1,0},{"myconfig.water.water_sw  1",1,0},{"JK_GET_WATER_VALUE 0",1,0}};
static const char *const c87_unexp[] = {"lld.c:404"};
static const char *const c87_tags[] = {"BLE","BLE core","FIQ"};
static const char *const c88_args[] = {"--only-uart","-chip","BK7231N"};
static const ExpectStr c88_exp[] = {{"FlashDB V1.1.2 is initialize success",1,0},{"chip id=7231c device id=20521023",1,0},{"calibration_main over",1,0},{"rwble_hl_init ok",1,0},{"gapm_cmp_evt:GAPM_GEN_RAND_NB",2,0},{"gapm_cmp_evt:BLE_STACK_OK",1,0},{"[THD]ves_ble",1,0},{"company=0x06D0",1,0},{"ff d0 06 01 d3 cf b8 4a 58 fc",1,0},{"Device model: BSDOG02",1,0},{"Vesync SDK version: v1.2.1-1d1881fe",1,0},{"Firmware version: 1.0.01",1,0},{"REG_PWM_GROUP_CTRL= 0x1c14",1,0},{"[THD]ves_netcfg",1,0},{"wpa rx E SCAN_STARTED",1,0}};
static const char *const c88_unexp[] = {"Unsupported chip or dev","lld.c:404"};
static const char *const c88_tags[] = {"BLE","BLE core","FIQ"};
static const char *const c89_args[] = {"--only-uart","-chip","BK7231N"};
static const ExpectStr c89_exp[] = {{"SDK Rev: 3.0.56 afc66d1",1,0},{"FlashDB V1.1.2 is initialize success",1,0},{"chip id=7231c device id=20521023",1,0},{"calibration_main over",1,0},{"ble mac:fc-58-4a-a1-d3-c3",1,0},{"rwble_hl_init ok",1,0},{"gapm_cmp_evt:GAPM_GEN_RAND_NB",2,0},{"gapm_cmp_evt:BLE_STACK_OK",1,0},{"company=0x06D0",1,0},{"ff d0 06 01 c3 d3 a1 4a 58 fc",1,0},{"Device model: WHOGPLUG",1,0},{"Vesync SDK version: v1.2.1-03528e63",1,0},{"Firmware version: 1.0.02",1,0},{"REG_PWM_GROUP_CTRL= 0x1c14",1,0},{"[THD]ves_netcfg",1,0},{"wpa rx E SCAN_STARTED",1,0}};
static const char *const c89_unexp[] = {"Unsupported chip or dev","lld.c:404"};
static const char *const c89_tags[] = {"BLE","BLE core","FIQ"};
static const char *const c90_args[] = {"--only-uart","-chip","BK7231N","--wifi-peer","auto","--wifi-ssid","JX-ChanCe-M2"};
static const ExpectStr c90_exp[] = {{"SDK Rev: 3.0.56 fa12785",1,0},{"chip id=7231c device id=20521023",1,0},{"calibration_main over",1,0},{"rwble_hl_init ok",1,0},{"gapm_cmp_evt:GAPM_GEN_RAND_NB",2,0},{"gapm_cmp_evt:BLE_STACK_OK",1,0},{"======>> INIT COMPLETE",1,0},{"Prod: 1, Version: 1.0.4",1,0},{"burninfo.cmei: 111092600037589",1,0},{"burninfo.sn: 1716059038400037588",1,0},{"burninfo.wifimac: bc-fd-0c-8e-3f-78",1,0},{"[WIFI] station bc:fd:0c:8e:3f:78 associated to 'JX-ChanCe-M2'",1,0},{"[NET] DHCP: device bc:fd:0c:8e:3f:78 got 192.168.1.100",1,0},{"connect JX-ChanCe-M2 success",1,0},{"find m2 ssid",1,0},{"do m2 cali",1,0},{"cali, p_ref: 870, v_ref: 2200, i_ref: 395, e_ref: 272",1,0},{"bl0937 cali start",1,0}};
static const char *const c90_unexp[] = {"Unsupported chip or dev","lld.c:404"};
static const char *const c90_tags[] = {"BLE","BLE core","FIQ","WiFi MAC"};
static const char *const c91_args[] = {"--only-uart","-chip","BK7231N"};
static const ExpectStr c91_exp[] = {{"SDK Rev: 3.0.56 ee14ae7f",1,0},{"OSK Rev: F-3.0.35 ee14ae7f",1,0},{"going Chip",1,0},{"chip[DL][svc_matter_main.cpp:403] ql_NetCfgInitAndCalcuate",1,0},{"chip[DL][svc_matter_main.cpp:406] ql_LoadLogFlag",1,0},{"init success",1,0}};
static const char *const c91_unexp[] = {"Unsupported chip or dev"};
static const char *const c92_args[] = {"--only-uart","--wifi-peer","auto"};
static const ExpectStr c92_exp[] = {{"oem_bk7321_bk_1_switch:1.1.3",1,0},{"read uf buf {\"fsw_cnt_key\":2}",1,0},{"rst_cnt:[3]",1,0},{"onoff cnt is ok! reset device..",1,0},{"[WDT] watchdog expired; device reset #1",1,0},{"read uf buf {\"fsw_cnt_key\":0}",1,0},{"rst_cnt:[1]",1,0},{"serial_no:68572db62696",1,0},{"gw_cntl.gw_if.product_key:keys83qyuhuqrdn7, input:keys83qyuhuqrdn7",1,0},{"device_init ok",1,0},{"temp in flash is:315",1,0},{"scan_start_req_handler",1,0},{"hal_machw_enter_monitor_mode",1,0},{"wifi_stat:1",1,0}};
static const char *const c92_tags[] = {"WDT","WiFi MAC"};
static const char *const c93_args[] = {"--only-uart","--wifi-peer","auto","--wifi-ssid","tuya_mdev_test1"};
static const ExpectStr c93_exp[] = {{"< TUYA IOT SDK V:2.0.0 BS:30.01_PT:2.2_LAN:3.3_CAD:1.0.1_CD:1.0.0 >",1,0},{"oem_bk7321_bk_1_switch:1.0.6",1,0},{"IO - relay[0]:\tpin-18\ttype high",1,0},{"IO - button[0]:\tpin-6\ttype low",1,0},{"IO - wifi_stat:\tpin-7\ttype low",1,0},{"hw_set_channel :1,channel_no :0",1,0},{"temp in flash is:315",1,0},{"[FUNC]func_init_extended OVER!!!",1,0},{"mf_init succ",1,0},{"current product ssid name:tuya_mdev_test1",1,0},{"scan_start_req_handler",1,0},{"flag:1,rssi:-45",1,0},{"product test mode",1,0},{"wifi_stat:1",1,0}};
static const char *const c93_tags[] = {"WiFi MAC"};
static const char *const c94_args[] = {"--only-uart","--wifi-peer","auto","--wifi-ssid","tuya_mdev_test1"};
static const ExpectStr c94_exp[] = {{"< TUYA IOT SDK V:2.0.0 BS:30.01_PT:2.2_LAN:3.3_CAD:1.0.1_CD:1.0.0 >",1,0},{"oem_bk7321_bk_1_switch:1.0.4",1,0},{"IO - relay[0]:\tpin-18\ttype high",1,0},{"IO - button[0]:\tpin-6\ttype low",1,0},{"IO - wifi_stat:\tpin-7\ttype low",1,0},{"hw_set_channel :1,channel_no :0",1,0},{"temp in flash is:315",1,0},{"rst_cnt:1",1,0},{"current product ssid name:tuya_mdev_test1",1,0},{"scan_start_req_handler",1,0},{"flag:1,rssi:-45",1,0},{"product test mode",1,0},{"wifi_stat:1",1,0}};
static const PinWant c94_pins[] = {{6,60},{18,0}};
static const char *const c94_tags[] = {"WiFi MAC"};
static const char *const c95_args[] = {"--only-uart"};
static const ExpectStr c95_exp[] = {{"< TUYA IOT SDK V:2.0.0 BS:30.01_PT:2.2_LAN:3.3_CAD:1.0.1_CD:1.0.0 >",1,0},{"oem_bk7321_bk_1_switch:1.0.4",1,0},{"IO - relay[0]:\tpin-18\ttype high",1,0},{"IO - button[0]:\tpin-6\ttype low",1,0},{"IO - wifi_stat:\tpin-7\ttype low",1,0},{"relay_stat:1",1,0},{"hw_set_channel :1,channel_no :0",1,0},{"temp in flash is:315",1,0},{"[FUNC]func_init_extended OVER!!!",1,0},{"mf_init succ",1,0},{"rst_cnt:2",1,0},{"current product ssid name:tuya_mdev_test1",1,0},{"scan_start_req_handler",1,0}};
static const PinWant c95_pins[] = {{6,60},{7,0},{18,2}};
static const char *const c96_args[] = {"--only-uart","-chip","BK7231U"};
static const ExpectStr c96_exp[] = {{"Welcome to AliOS Things",1,0},{"reset_counts get from flash:3",1,0},{"RGBLight_Param.LightSwitch:1",1,0},{"RGBLight_Param.Brightness:50",1,0},{"RGBLight_Param.ColorTemperature:4500",1,0},{"RGBLight_Param.Red:255",1,0},{"init pwm pin !",1,0},{"scan_start_req_handler",1,0},{"[ResetCounts] reset_counts:3",1,0},{"[ResetCounts] active the reset function",1,0}};
static const PinWant c96_pins[] = {{7,72},{8,72},{9,72},{24,72},{26,72}};
static const char *const c97_args[] = {"--only-uart","-chip","BK7231U"};
static const ExpectStr c97_exp[] = {{"Welcome to AliOS Things",1,0},{"reset_counts get from flash:1",1,0},{"RGBLight_Param.LightSwitch:1",1,0},{"RGBLight_Param.Brightness:50",1,0},{"RGBLight_Param.ColorTemperature:4500",1,0},{"init pwm pin !",1,0},{"scan_start_req_handler",1,0},{"[ResetCounts] reset_counts:1",1,0}};
static const char *const c97_unexp[] = {"[ResetCounts] active the reset function"};
static const PinWant c97_pins[] = {{7,72},{8,72},{9,72},{24,72},{26,72}};
static const PwmWant c97_pwm[] = {{0,52000,0}};
static const char *const c98_args[] = {"--only-uart","-chip","BK7231U"};
static const ExpectStr c98_exp[] = {{"Thread Operating System",1,0},{"3.1.0 build Apr  8 2021",1,0},{"lwIP-2.0.2 initialized!",1,0},{"register station wlan device sucess!",1,0},{"register soft-ap wlan device sucess!",1,0},{"beken wlan hw init",1,0},{"| bootloader | beken_onchip_crc | 0x00000000 | 0x00010000 |",1,0},{"| app        | beken_onchip_crc | 0x00010000 | 0x00110000 |",1,0},{"RT-Thread Flash Abstraction Layer (V0.4.0) initialize success.",1,0}};
static const char *const c99_args[] = {"--only-uart","-chip","BK7231U"};
static const ExpectStr c99_exp[] = {{"Thread Operating System",1,0},{"3.1.0 build Apr  8 2021",1,0},{"read txpwr tab from flash success",1,0},{"temp in flash is:280",1,0},{"xtal inital:280, 70, 16",1,0},{"lwIP-2.0.2 initialized!",1,0},{"register station wlan device sucess!",1,0},{"register soft-ap wlan device sucess!",1,0},{"| download   | beken_onchip     | 0x00132000 | 0x000c4000 |",1,0},{"RT-Thread Flash Abstraction Layer (V0.4.0) initialize success.",1,0}};
static const char *const c100_args[] = {"--only-uart","-chip","BK7231U","--wifi"};
static const ExpectStr c100_exp[] = {{"[WIFI] device RX filter 0x7fffffde: monitor mode",1,0},{"enter [cc8000::V2.3.72::V2.04.51] main ok ; tick=172",1,0},{"-----rw_main task init----",1,0},{"-----rw_main  start----",1,0}};
static const PinWant c100_pins[] = {{2,60},{3,60},{20,60},{21,60}};
static const char *const c100_tags[] = {"WiFi MAC"};
static const char *const c101_args[] = {"--only-uart","-chip","BK7231U","--wifi-peer","auto"};
static const ExpectStr c101_exp[] = {{"enter [cc8000::V2.2.18::V2.03.47] main ok ; tick=173",1,0},{"-----rw_main task init----",1,0},{"-----rw_main  start----",1,0},{"[WIFI] found BSS 'SmartLight-5783' 1c:d6:bd:d4:57:82 ch 7 (open)",1,0},{"[WIFI] associated with 'SmartLight-5783' (AID 1)",1,0},{"[NET] DHCP: we got 192.168.10.2/255.255.255.0 from the device (router 192.168.10.1)",1,0},{"[NET] ARP: device 192.168.10.1 is at 1c:d6:bd:d4:57:82",1,0},{"[NET] ping reply from 192.168.10.1 (seq 1)",1,0}};
static const PinWant c101_pins[] = {{2,60},{3,60},{20,60},{21,60},{23,72}};
static const char *const c101_tags[] = {"WiFi MAC"};
static const char *const c102_args[] = {"--only-uart","-chip","BK7231U","--wifi-peer","auto"};
static const ExpectStr c102_exp[] = {{"[WIFI] device RX filter 0x7fffffde: monitor mode",1,0},{"enter [cc8000::V2.3.72::V2.04.51] main ok ; tick=172",1,0},{"-----rw_main task init----",1,0},{"-----rw_main  start----",1,0}};
static const char *const c102_unexp[] = {"found BSS 'SmartLight"};
static const PinWant c102_pins[] = {{2,60},{3,60},{20,60},{21,60}};
static const char *const c102_tags[] = {"WiFi MAC"};
static const char *const c103_args[] = {"--only-uart","-chip","BK7252N","--xvr-selfclear","--wifi-peer","auto"};
static const ExpectStr c103_exp[] = {{"[WIFI] device RX filter 0x7fffffde: monitor mode",1,0},{"[WIFI] device RX filter 0x3503848c: back to normal reception",1,0},{"[WIFI] device acts as a station (it probes for networks): peer takes the access point role",1,0}};
static const PinWant c103_pins[] = {{5,2},{7,60},{9,0},{14,888},{15,888},{16,888},{17,888},{18,888},{19,888}};
static const char *const c103_tags[] = {"WiFi MAC","XVR"};
static const char *const c104_args[] = {"--only-uart","-chip","BK7252N"};
static const ExpectStr c104_exp[] = {{"RT-Thread Flash Abstraction Layer (V0.4.0) initialize success.",1,0},{"ROMFS File System initialized!",1,0},{"Enter normal mode...",1,0},{"bk_wlan_app_init finished",1,0},{"rf_thread_init ok",1,0},{"xc_main, sw_ver=3.00.21.01_250512!",1,0},{"--- battary_status :139 ---",1,0},{"return xdev_opt, magic=11223344",1,0},{"IOTSDK Ver = 3.00.42.01_241230",1,0},{"date: 2025-3-1, time: 8:1:50",1,0},{"power status is off, wait to power on",1,0},{"wake up time out to enter deep sleep",1,0},{"---enter deep sleep :wake up with gpio 0~31 ps: 0x2020 0x20",1,0}};
static const PinWant c104_pins[] = {{2,0},{4,63},{5,63},{9,0},{12,60},{13,60},{14,888},{15,888},{16,888},{17,888},{18,888},{19,888},{22,0},{23,0}};
static const char *const c105_args[] = {"--only-uart","-chip","BK7252N"};
static const ExpectStr c105_exp[] = {{"RT-Thread Flash Abstraction Layer (V0.4.0) initialize success.",1,0},{"ROMFS File System initialized!",1,0},{"Enter normal mode...",1,0},{"bk_wlan_app_init finished",1,0},{"rf_thread_init ok",1,0},{"power status is on",1,0},{"-------->xled_start",1,0},{"did = PPH0A906615F0207B0",1,0},{"signkey = CRfaBC8kuPZ05YMIJHOzEw==",1,0},{"scode = 760441",1,0},{"gdomain = prod.glbs.xcthings.com",1,0},{"wifi mac_s address is c8:47:8c:4c:81:2c -- s",1,0},{"AVSDK VER: 3.00.42.01_241230, build:Dec 30 2024-17:39:37",1,0},{"OSAL_VER:1.00.01.64_240206",1,0},{"osal_plat_thr_init,name=vtask_wor,stack_size=4096",1,0},{"osal_plat_thr_init,name=unpack,stack_size=4096",1,0},{"osal_plat_thr_init,name=xrpc.r,stack_size=4096",1,0},{"osal_plat_thr_init,name=xrpc.w,stack_size=4096",1,0},{"vtask_add_work ok",1,0},{"read wifi error,rc:0",1,0}};
static const PinWant c105_pins[] = {{2,0},{4,63},{5,63},{9,0},{12,60},{13,60},{14,888},{15,888},{16,888},{17,888},{18,888},{19,888},{22,2},{23,2}};
static const char *const c106_args[] = {"--only-uart","-chip","BK7252N"};
static const ExpectStr c106_exp[] = {{"RT-Thread Flash Abstraction Layer (V0.4.0) initialize success.",1,0},{"ROMFS File System initialized!",1,0},{"Enter normal mode...",1,0},{"bk_wlan_app_init finished",1,0},{"rf_thread_init ok",1,0},{"2025-03-01 00:03:05",1,0},{"power status is on",1,0},{"-------->xled_start",1,0},{"did = PPH0A906615F0207B0",1,0},{"scode = 760441",1,0},{"gdomain = prod.glbs.xcthings.com",1,0},{"wifi mac_s address is c8:47:8c:4c:81:2c -- s",1,0},{"AVSDK VER: 3.00.42.01_241230",1,0},{"OSAL_VER:1.00.01.64_240206",1,0},{"osal_plat_thr_init,name=vtask_wor,stack_size=4096",1,0},{"osal_plat_thr_init,name=xrpc.r,stack_size=4096",1,0},{"osal_plat_thr_init,name=xrpc.w,stack_size=4096",1,0},{"vtask_add_work ok",1,0},{"read wifi error,rc:0",1,0}};
static const PinWant c106_pins[] = {{2,0},{4,63},{5,63},{9,0},{12,60},{13,60},{14,888},{15,888},{16,888},{17,888},{18,888},{19,888},{22,2},{23,2}};
static const char *const c107_args[] = {"--only-uart","-chip","BK7252N"};
static const ExpectStr c107_exp[] = {{"RT-Thread Flash Abstraction Layer (V0.4.0) initialize success.",1,0},{"ROMFS File System initialized!",1,0},{"Enter normal mode...",1,0},{"bk_wlan_app_init finished",1,0},{"rf_thread_init ok",1,0},{"did = PPH0A90E1F42CF3350",1,0},{"signkey = V9Ja2Pz1D9mssDt5zBov9A==",1,0},{"scode = 574818",1,0},{"gdomain = prod.glbs.xcthings.com",1,0},{"wifi mac_s address is c8:47:8c:70:a1:b2 -- s",1,0},{"AVSDK VER: 3.00.42.01_241230, build:Dec 30 2024-17:39:37",1,0},{"OSAL_VER:1.00.01.64_240206, Dec  3 2024-19:10:18",1,0},{"osal_plat_thr_init,name=vtask_wor,stack_size=4096",1,0},{"wait vtask start!",1,0}};
static const PinWant c107_pins[] = {{2,0},{4,63},{5,63},{9,0},{12,60},{13,60},{14,888},{15,888},{16,888},{17,888},{18,888},{19,888},{22,2},{23,2}};
static const char *const c108_args[] = {"--only-uart","-chip","BK7252"};
static const ExpectStr c108_exp[] = {{"Thread Operating System",1,0},{"3.1.0 build Apr  1 2024",1,0},{"get rfcali_mode:0",1,0},{"tssi_th:b-145, g-130",1,0},{"txpwr table for ble ch0/19/39 inused",1,0},{"NO TXID found in flash, use lpf i&q:61, 61",1,0},{"RT-Thread Flash Abstraction Layer (V0.4.0) initialize success.",1,0},{"creat jpegdec thread!",1,0},{"------------------------->vbat_detect_test",1,0},{"[ut_xciot.c      /01155] : ---adc channel:0---",1,0},{"tc_entity_init",1,0},{"ROMFS File System initialized!",1,0},{"FLASH File System initialzation failed!",1,0},{"msh />",1,0}};
static const PinWant c108_pins[] = {{0,120},{1,120},{9,0}};
static const char *const c109_args[] = {"--only-uart","-chip","BK7252"};
static const ExpectStr c109_exp[] = {{"Thread Operating System",1,0},{"3.1.0 build Aug  7 2024",1,0},{"test_flash_read_for_mac",1,0},{"tssi_th:b-130, g-123",1,0},{"NO TXID_LPFCAP found in flash, use def 61, 61",1,0},{"register station wlan device sucess!",1,0},{"register soft-ap wlan device sucess!",1,0},{"beken wlan hw init",1,0},{"drivers  set dac vol:65 - indx:11,dig:30,ana:1a",1,0},{"set adc vol: 80 - 80",1,0},{"drv_pm_init",1,0},{"com.naxclow.Camera_v0-1.1.20",1,0},{"DevType (A9_V2.7_ALERT_A_48P",1,0},{"msh />",1,0}};
static const PinWant c109_pins[] = {{0,120},{1,120},{9,0},{10,120},{11,120}};
static const char *const c110_args[] = {"--only-uart","-chip","BK7252"};
static const ExpectStr c110_exp[] = {{"Thread Operating System",1,0},{"3.1.0 build May 14 2022",1,0},{"OSK Rev: R-3.0.22",1,0},{"SDK Rev: 3.0.33",1,0},{"IP Rev: W4-3.0.33-P0",1,0},{"rfcali_mode:0",1,0},{"tssi:b-125, g-115",1,0},{"xtal_cali:25",1,0},{"[FUNC]func_init_extended OVER!!!",1,0},{"start_type:0",1,0},{"lwIP-2.0.2 initialized!",1,0},{"igmp_mac_filter add 224.0.0.1 01:00:5E:00:00:01",2,0},{"beken wlan hw init",1,0},{"drv_pm_init",1,0},{"Partition table NOT found on flash: beken_onchip_crc (len: 24) from offset: 0x0000f000.",1,0},{"RT-Thread Flash Abstraction Layer (V0.4.0) initialize failed.",1,0},{"msh />",1,0}};
static const PinWant c110_pins[] = {{0,120},{1,120},{10,120},{11,120}};
static const char *const c111_args[] = {"--only-uart","-chip","BK7252"};
static const ExpectStr c111_exp[] = {{"Thread Operating System",1,0},{"3.1.0 build May  9 2023",1,0},{"test_flash_read_for_mac",1,0},{"No TLV header found in flash",1,0},{"tssi_th:b-120, g-115",1,0},{"[sys_wdtDeviceInitStart - 38]-Debue: start wdt device",1,0},{"register station wlan device sucess!",1,0},{"register soft-ap wlan device sucess!",1,0},{"beken wlan hw init",1,0},{"drivers  set dac vol:65 - indx:11,dig:30,ana:1a",1,0},{"set adc vol: 80 - 80",1,0},{"drv_pm_init",1,0},{"com.naxclow.Camera_v0-1.1.20",1,0},{"tc_entity_init",1,0},{"sign_netConfSignInit_n = (NULL)",1,0},{"[0]202305091627",1,0},{"sys_test_flash_read [0800C0140548]",1,0},{"sign_readFlashDEVICE_id state 0",1,0},{"[1]0800C0140548",1,0}};
static const PinWant c111_pins[] = {{0,120},{1,120},{9,0},{10,120},{11,120}};
static const char *const c112_args[] = {"--only-uart","-chip","BK7252","--wifi-peer","sta"};
static const ExpectStr c112_exp[] = {{"[WIFI] device RX filter 0x7fffffde: monitor mode, every frame on air accepted (sniffing, as in EZ pairing)",1,0},{"[WIFI] device RX filter 0x3503848c: back to normal reception",1,0},{"[WIFI] found BSS 'EEE-196629-TLESS' 00:00:00:12:56:77 ch 11 (open)",1,0},{"[WIFI] authenticated with 00:00:00:12:56:77",1,0},{"[WIFI] associated with 'EEE-196629-TLESS' (AID 1)",1,0},{"[NET] DHCP: we got 192.168.169.101/255.255.255.0 from the device (router 192.168.169.100)",1,0},{"[NET] ARP: device 192.168.169.100 is at 00:00:00:12:56:77",1,0},{"[NET] ping reply from 192.168.169.100 (seq 1)",1,0}};
static const PinWant c112_pins[] = {{0,120},{1,120},{4,60},{7,60},{10,120},{11,120},{12,60},{14,120},{15,120},{16,120},{17,120},{18,120},{19,120}};
static const char *const c113_args[] = {"--only-uart","-chip","BK7252","--wifi-peer","sta"};
static const ExpectStr c113_exp[] = {{"[WIFI] device RX filter 0x7fffffde: monitor mode, every frame on air accepted (sniffing, as in EZ pairing)",1,0},{"[WIFI] device RX filter 0x3503848c: back to normal reception",1,0},{"[WIFI] found BSS 'PIXA-077260-DNGUV' 90:91:64:08:54:80 ch 11 (open)",1,0},{"[WIFI] authenticated with 90:91:64:08:54:80",1,0},{"[WIFI] associated with 'PIXA-077260-DNGUV' (AID 1)",1,0},{"[NET] DHCP: we got 192.168.169.101/255.255.255.0 from the device (router 192.168.169.100)",1,0},{"[NET] ARP: device 192.168.169.100 is at 90:91:64:08:54:80",1,0},{"[NET] ping reply from 192.168.169.100 (seq 1)",1,0}};
static const PinWant c113_pins[] = {{0,120},{1,120},{4,60},{7,60},{10,120},{11,120},{12,60},{14,120},{15,120},{16,120},{17,120},{18,120},{19,120}};
static const char *const c114_args[] = {"--only-uart","-chip","BK7252","--wifi-peer","sta"};
static const ExpectStr c114_exp[] = {{"[WIFI] device RX filter 0x7fffffde: monitor mode, every frame on air accepted (sniffing, as in EZ pairing)",1,0},{"[WIFI] device RX filter 0x3503848c: back to normal reception",1,0},{"[WIFI] found BSS 'PIXA-180431-XLPNL' 90:91:64:0b:56:77 ch 11 (open)",1,0},{"[WIFI] authenticated with 90:91:64:0b:56:77",1,0},{"[WIFI] associated with 'PIXA-180431-XLPNL' (AID 1)",1,0},{"[NET] DHCP: we got 192.168.169.101/255.255.255.0 from the device (router 192.168.169.100)",1,0},{"[NET] ARP: device 192.168.169.100 is at 90:91:64:0b:56:77",1,0},{"[NET] ping reply from 192.168.169.100 (seq 1)",1,0}};
static const PinWant c114_pins[] = {{0,120},{1,120},{7,60},{14,120},{15,120},{16,120},{17,120},{18,120},{19,120}};
static const char *const c115_args[] = {"--only-uart","--ble-core","--uart1-hex"};
static const ExpectStr c115_exp[] = {{"SDK Rev: 3.0.46_20220921_d9dce354c70f",1,0},{"chip id=7231a device id=18520001",1,0},{"calibration_main over",1,0},{"rwble_hl_init ok",1,0},{"BLE Rev: B5-3.0.46-P0",1,0},{"gapm_cmp_evt:BLE_STACK_OK",1,0},{"firware ver:3.4.3",1,0},{"Date: Oct 23 2024 14:47:15",1,0},{"MAC address: 00:1c:c2:a4:d9:1c",1,0},{"ble_name: DREOsh17",1,0},{"info checkout: success",1,0},{"license.sn:1769616359658942465-fda17af2af19fd07:001:0000000000b",1,0},{"[UART1/MCU] 55 aa 00 00 00 00 00 00 ff 55 aa 00 01 01 00 00 00 01",1,0},{"[UART1/MCU] 55 aa 00 02 03 00 00 02 01 00 07",1,0},{"wpa S: DISCONNECTED -> SCANNING",1,0}};
static const PinWant c115_pins[] = {{0,120},{1,120},{10,120},{11,120}};
static const char *const c115_tags[] = {"BLE","BLE core","FIQ","TuyaMCU"};
static const char *const c116_args[] = {"--only-uart","--ble-core","--uart1-hex"};
static const ExpectStr c116_exp[] = {{"SDK Rev: 3.0.46_20220921_d9dce354c70f",1,0},{"chip id=7231a device id=18520001",1,0},{"calibration_main over",1,0},{"rwble_hl_init ok",1,0},{"BLE Rev: B5-3.0.46-P0",1,0},{"gapm_cmp_evt:BLE_STACK_OK",1,0},{"firware ver:3.8.7",1,0},{"Date: Oct 13 2025 10:46:14",1,0},{"MAC address: 00:1c:c2:a6:a8:07",1,0},{"ble_name: DREOsh17",1,0},{"info checkout: success",1,0},{"license.sn:1769616359658942465-010f809255100431:001:0000000000b",1,0},{"[UART1/MCU] 55 aa 00 00 00 00 00 00 ff 55 aa 00 01 01 00 00 00 01",1,0},{"[UART1/MCU] 55 aa 00 02 03 00 00 02 01 00 07",1,0},{"wpa S: DISCONNECTED -> SCANNING",1,0}};
static const PinWant c116_pins[] = {{0,120},{1,120},{10,120},{11,120}};
static const char *const c116_tags[] = {"BLE","BLE core","FIQ","TuyaMCU"};
static const char *const c117_args[] = {"--only-uart","-chip","BK7231N"};
static const ExpectStr c117_exp[] = {{"OSK Rev: A-3.0.30",1,0},{"SDK Rev: 3.0.65",1,0},{"chip id=7231c device id=20521023",1,0},{"calibration_main over",1,0},{"rwble_hl_init ok",1,0},{"BLE Rev: B5-3.0.65-P0",1,0},{"gapm_cmp_evt:BLE_STACK_OK",1,0},{"-Welcome to AliOS Things-",1,0},{"Kernel: AOS-R-1.3.4",1,0},{"LinkKit: 2.3.0_FY_1.6.0",1,0},{"APP: WP8621NK_03",1,0},{"VSON product_key[a1c3mFpE63X]",1,0},{"product_secret[HZQ5AC8ME8Vv5klX]",1,0},{"device_name[86:21:02:FC:22:41]",1,0},{"ota init success, ver:WP8621NK_03 type:0",1,0},{"scan factory ap, set led ON",1,0},{"struct_calibration.calibration = 1365",1,0},{"verson = WP8621NK_03",1,0}};
static const PinWant c117_pins[] = {{0,0},{1,0},{7,60},{9,60},{10,120},{11,120},{15,0},{17,0},{22,8},{23,0},{28,60}};
static const char *const c117_tags[] = {"BLE","BLE core","FIQ"};
static const char *const c118_args[] = {"--only-uart","-chip","BK7238","--wifi-peer","ap","--wifi-ssid","DLINK_FastNet","--wifi-pass","F4STN3T@W*f*PL1234567890"};
static const ExpectStr c118_exp[] = {{"**********tuya_upgrade_main need upgrade\? [255]**********",1,0},{"device_id=0x21128000",1,0},{"< TuyaOS V:3.11.11 BS:40.00_PT:2.3_LAN:3.5_CAD:1.0.5_CD:1.0.0 >",1,0},{"< BUILD AT:2025_03_05_20_01_36 BY ci_manage FOR tuyaos-iot AT T1 >",1,0},{"t1_common_user_config_ty sw ver:3.0.22",1,0},{"calibration_main over",1,0},{"kv has ccode PL",1,0},{"mac 80:64:7c:99:dd:7f",1,0},{"bssid 28:87:ba:a0:f5:6d, cap_info 0x1811, BI 100, ssid DLINK_FastNet",1,0},{"writed fci to flash ssid=DLINK_FastNet",1,0},{"[NET] DHCP: device 80:64:7c:99:dd:7f got 192.168.1.100 (gateway/DNS 192.168.1.1)",1,0},{"handshake authenticated; serving encrypted MQTT offline",1,0},{"[NET] MQTT SUBSCRIBE smart/device/in/bf70c4ef9a3806556brng4",1,0}};
static const PinWant c118_pins[] = {{0,120},{1,120},{10,120},{11,120},{14,840},{15,842},{16,840},{17,840}};
static const char *const c119_args[] = {"--only-uart","-chip","BK7238"};
static const ExpectStr c119_exp[] = {{"OSK Rev: F-3.0.40",1,0},{"SDK Rev: 3.0.65",1,0},{"chip id=7238 device id=21128000",1,0},{"calibration_main over",1,0},{"lpf_i & q in flash is:74, 76",1,0},{"xtal in flash is:23",1,0},{"gapm_cmp_evt:BLE_STACK_OK",1,0},{"BLE_5_CREATE_DB",1,0},{"ssid:tuyatest key:fakepassword",1,0},{"mac 44:4a:d6: c:9e:14",1,0},{"PSKC: ssid tuyatest, passphrase fakepassword",1,0},{"wpa S: DISCONNECTED -> SCANNING",1,0},{"[BLE_ADV]   16-bit UUIDs (incomplete)  01 a1                                   ..  uuid=0xA101",1,0},{"sxebObqQRfXmgU  uuid=0xA101",1,0}};
static const PinWant c119_pins[] = {{0,120},{1,120},{6,60},{10,120},{11,120},{14,72},{15,72},{16,72},{17,72},{22,0},{28,8}};
static const char *const c119_tags[] = {"BLE","BLE core","FIQ"};
static const char *const c120_args[] = {"--only-uart","-chip","BK7238"};
static const ExpectStr c120_exp[] = {{"power on",1,0},{"[BLE_ADV] EM 0x0040e480 +0x0a54 (13 bytes): 02 01 06 09 09 57 46 32 34 2d 42 4c 45",1,0},{"[BLE_ADV]   Complete Name",1,0},{"WF24-BLE",1,0}};
static const PinWant c120_pins[] = {{0,120},{1,120},{6,0},{10,120},{11,120},{26,0}};
static const char *const c120_tags[] = {"BLE","BLE core","FIQ"};
static const char *const c121_args[] = {"--only-uart","-chip","BK7238"};
static const ExpectStr c121_exp[] = {{"SDK Rev: 3.0.70 c063598",1,0},{"chip id=7238 device id=21128000",1,0},{"bulid time 20240425",1,0},{"light version 4.16.0(2)",1,0},{"|MID-> AK001-ZJ21412",1,0},{"--------BP5758D rgbwc_ele 8 8 8 30 30",1,0},{"|OS->8.43.0(4)",1,0},{"|FW->35_243_20240425_ZG-BK38-BP101",1,0},{"I(APP:PWRRCD) store index 23",1,0},{"calibration_main over",1,0},{"I(PORT) adv_name_get IOTWF46D",1,0},{"[BLE_ADV]   Service Data               00 5a 4c 07 00 00 00 00 00 00 00 35 ..  .ZL........5....  uuid=0x5A00",1,0}};
static const PinWant c121_pins[] = {{0,120},{1,120},{10,120},{11,120},{24,2},{26,2}};
static const char *const c121_tags[] = {"BLE","BLE core","FIQ"};
static const char *const c122_args[] = {"--only-uart","-chip","BK7238"};
static const ExpectStr c122_exp[] = {{"SDK Rev: 3.0.70 ecb1fb4",1,0},{"chip id=7238 device id=21128000",1,0},{"bulid time 20240423",1,0},{"light version 1.1.2(2)",1,0},{"|MID-> AK001-ZJ21413",1,0},{"|OS->8.42.4(1)",1,0},{"|FW->77_24_20240423_ZG-BK",1,0},{"calibration_main over",1,0},{"I(PORT) adv_name_get IOTWF8A9",1,0},{"[BLE_ADV]   Complete Name",1,0},{"IOTWF8A9",1,0},{"D(BRMT) (2408) task init success",1,0}};
static const PinWant c122_pins[] = {{0,120},{1,8},{6,60},{8,60},{10,120},{11,120},{14,72},{15,72},{16,72},{17,72}};
static const char *const c122_tags[] = {"BLE","BLE core","FIQ"};
static const char *const c123_args[] = {"--only-uart","-chip","BK7238"};
static const ExpectStr c123_exp[] = {{"SDK Rev: 3.0.70 17e8b67",1,0},{"chip id=7238 device id=21128000",1,0},{"bulid time 20250724",1,0},{"light version 0.1.2(8)",1,0},{"|MID-> AK001-ZJ21412",1,0},{"|sdk version -> 2.10.1(6)",1,0},{"|OS->9.7.1(3)",1,0},{"|FW->3D_23_20250724_ZIL-BK38-M5-12",1,0},{"calibration_main over",1,0},{"I(PORT) adv_name_get IOTWF615",1,0},{"[BLE_ADV]   Complete Name",1,0},{"IOTWF615",1,0},{"I(FOTA) read dev version (old :  ) ( now : 3D_23_20250724_ZIL-BK38-M5-12 ) (target :  )",1,0}};
static const PinWant c123_pins[] = {{0,120},{1,120},{6,72},{8,72},{10,120},{11,120},{14,72},{15,72},{16,72},{17,72}};
static const char *const c123_tags[] = {"BLE","BLE core","FIQ"};
static const char *const c124_args[] = {"--only-uart","-chip","BK7238"};
static const ExpectStr c124_exp[] = {{"OSK Rev: F-3.0.44",1,0},{"SDK Rev: 3.0.70",1,0},{"***CK-BK7238-W105SE0X-01-DOOR-HB---Ver:1.0.4,Btim:20250516.09531208.015***",1,0},{"chip id=7238 device id=21128000",1,0},{"sta_mac:d0:27:04:43:42:98",1,0},{"DeviceMac:d02704434298",1,0},{"calibration_main over",1,0},{"wpa S: DISCONNECTED -> SCANNING",1,0}};
static const PinWant c124_pins[] = {{0,120},{1,120},{7,60},{10,120},{11,120},{21,0},{22,60},{23,60}};
static const char *const c125_args[] = {"--only-uart","-chip","BK7238"};
static const ExpectStr c125_exp[] = {{"SDK Rev: 3.0.70 434b394",1,0},{"chip id=7238 device id=21128000",1,0},{"bulid time 20250107",1,0},{"light version 0.1.2(8)",1,0},{"|MID-> AK001-ZJ21412",1,0},{"|sdk version -> 2.9.0(1)",1,0},{"|OS->9.7.1(3)",1,0},{"|FW->3D_22_20250107_ZIL-BK38",1,0},{"calibration_main over",1,0},{"I(PORT) adv_name_get IOTWF829",1,0},{"[BLE_ADV]   Complete Name",1,0},{"IOTWF829",1,0},{"D(BRMT) (2588) task init success",1,0}};
static const PinWant c125_pins[] = {{0,120},{1,120},{6,72},{8,72},{10,120},{11,120},{14,72},{15,72},{16,72},{17,72}};
static const char *const c125_tags[] = {"BLE","BLE core","FIQ"};
static const char *const c126_args[] = {"--only-uart","-key","TUYA","--wifi-peer","ap","--wifi-ssid","HomeNET","--wifi-pass","746025625"};
static const ExpectStr c126_exp[] = {{"< TUYA IOT SDK V:2.0.0 BS:30.06_PT:2.2_LAN:3.3_CAD:1.0.2_CD:1.0.0 >",1,0},{"oem_bk7231s_fan_control_arlec:1.0.5",1,0},{"**********[oem_bk7231s_fan_control_arlec] [1.0.5] compiled at Jul  7 2020 11:27:29**********",1,0},{"update bound state 1",1,0},{"device_init ok  free_mem_size:81808",1,0},{"ty bt update localkey",1,0},{"55 bc ce cb 9b 56 ac 21 ba 29 28 8a 6e 47 2c f0",1,0},{"ble adv && resp changed",1,0},{"gapm_cmp_evt_handler operation = 0x1, status = 0x0",1,0},{"ssid:HomeNET, 1",1,0},{"[NET] DHCP: device 68:57:2d:f9:7c:48 got 192.168.1.100 (gateway/DNS 192.168.1.1)",1,0},{"[NET] DNS m2.tuyaeu.com -> 10.200.0.1 (offline sinkhole)",1,0},{"TLS_PSK_WITH_AES_128_CBC_SHA256",1,0}};
static const PinWant c126_pins[] = {{0,120},{1,60},{6,2},{7,2},{8,60},{9,2},{10,2},{11,120},{12,120},{13,120},{14,60},{26,0}};
static const char *const c127_args[] = {"--only-uart","-key","TUYA"};
static const ExpectStr c127_exp[] = {{"goto first bright up!",1,0},{"**********[oem_bk7231t_light3_laser_nanxin] [1.1.2] compiled at Jan 27 2021 10:29:48**********",1,0},{"< TUYA IOT SDK V:1.0.2 BS:40.00_PT:2.2_LAN:3.3_CAD:1.0.2_CD:1.0.0 >",1,0},{"oem_bk7231t_light3_laser_nanxin:1.1.2",1,0},{"have actived over 15 min, not enter mf_init",1,0},{"mf_init succ",1,0},{"button init",1,0},{"device config data already load! Don't load again!!",1,0},{"Drive init already init ok",1,0},{"STACK INIT OK",1,0},{"CREATE DB SUCCESS",1,0},{"ble adv && resp changed",1,0},{"appm start advertising",1,0}};
static const PinWant c127_pins[] = {{0,120},{1,120},{6,72},{7,0},{8,72},{9,72},{10,120},{11,120},{12,120},{13,120},{14,60},{24,72},{26,72}};
static const PwmWant c127_pwm[] = {{0,26000,13000},{2,26000,26000},{3,26000,26000},{4,26000,26000},{5,26000,-1}};
static const char *const c128_args[] = {"--only-uart","-key","TUYA","--wifi-peer","ap","--wifi-ssid","FASTWEB-casaFW","--wifi-pass","Giuseppe2707&Marta0909"};
static const ExpectStr c128_exp[] = {{"< TUYA IOT SDK V:1.0.2 BS:40.00_PT:2.2_LAN:3.3_CAD:1.0.2_CD:1.0.0 >",1,0},{"oem_bk7231s_garage_door:1.1.2",1,0},{"**********[oem_bk7231s_garage_door] [1.1.2] compiled at Nov 23 2020 20:07:02**********",1,0},{"device_ctrl_init start!",1,0},{"device_ctrl_init end!",1,0},{"device_init ok  free_mem_size:74552",1,0},{"ty bt update localkey",1,0},{"ble adv && resp changed",1,0},{"ssid:FASTWEB-casaFW, 1",1,0},{"[NET] DHCP: device b8:06:0d:ca:44:35 got 192.168.1.100 (gateway/DNS 192.168.1.1)",1,0},{"[NET] DNS m2.tuyaeu.com -> 10.200.0.1 (offline sinkhole)",1,0},{"\"gwId\":\"bf5758e46fc5d63663qc5f\",\"active\":2,\"ablilty\":0,\"encrypt\":true,\"productKey\":\"keymxkctp9ukgcvg\"",1,0}};
static const PinWant c128_pins[] = {{0,120},{1,120},{6,60},{7,2},{10,120},{11,120},{12,120},{13,120},{24,0},{26,44}};
static const char *const c129_args[] = {"--only-uart","-key","TUYA"};
static const ExpectStr c129_exp[] = {{"< TUYA IOT SDK V:1.0.2 BS:40.00_PT:2.2_LAN:3.3_CAD:1.0.2_CD:1.0.0 >",1,0},{"oem_bk7231s_curtain_switch:1.6.0",1,0},{"**********[oem_bk7231s_curtain_switch] [1.6.0] compiled at May 31 2021 16:53:20**********",1,0},{"CH[0]:dpid,[1-2-3-10-8-12-{dryEn(true)-intertime200(ms)-drvtime:200(ms)}]",1,0},{"Sub ch[0]:led[255-  2],rly[  7-  1],bt[  8-  0 - longKey-5000(ms)]",1,0},{"Sub ch[1]:led[255-  2],rly[255-  2],bt[ 10-  0 - longKey-5000(ms)]",1,0},{"Sub ch[2]:led[255-  2],rly[  6-  1],bt[ 24-  0 - longKey-5000(ms)]",1,0},{"curtain_json_init init end!",1,0},{"dev_event_json_init init end!",1,0},{"have actived over 15 min, not enter mf_init",1,0},{"STACK INIT OK",1,0},{"CREATE DB SUCCESS",1,0},{"ble adv && resp changed",1,0},{"appm start advertising",1,0}};
static const PinWant c129_pins[] = {{0,120},{1,0},{6,0},{7,0},{8,60},{10,60},{11,120},{12,120},{13,120},{24,60}};
static const char *const c130_args[] = {"--only-uart","-key","TUYA"};
static const ExpectStr c130_exp[] = {{"< TUYA IOT SDK V:1.0.2 BS:40.00_PT:2.2_LAN:3.3_CAD:1.0.2_CD:1.0.0 >",1,0},{"oem_bk7231s_rnd_switch:1.1.8",1,0},{"**********[oem_bk7231s_rnd_switch] [1.1.8] compiled at Jul  9 2021 19:30:24**********",1,0},{"STACK INIT OK",1,0},{"CREATE DB SUCCESS",1,0},{"ble adv && resp changed",1,0},{"appm start advertising",1,0},{"mf_init succ",1,0},{"pConfig = {netled_lv:1,bt_type:0,bt1_pin:26,mxcl_led_m:0,led1_pin:6,rand_dpid:43,net_trig:2,rl_on1_pin:9,led_dp:40,jv:1.1.1,lock_dp:41,nety_led:1,rl_off1_pin:8,netled_pin:24,total_stat:0,rl_type:1,bt1_lv:0,",1,0}};
static const PinWant c130_pins[] = {{0,120},{1,120},{10,120},{11,120},{12,120},{13,120}};
static const char *const c131_args[] = {"--only-uart","-key","TUYA","--wifi-peer","ap","--wifi-ssid","DLINK_FastNet","--wifi-pass","F4STN3T@W*f*PL1234567890"};
static const ExpectStr c131_exp[] = {{"< TUYA IOT SDK V:1.0.8 BS:40.00_PT:2.2_LAN:3.3_CAD:1.0.2_CD:1.0.0 >",1,0},{"oem_bk7231s_double_switch_rmd:1.1.3",1,0},{"**********[oem_bk7231s_double_switch_rmd] [1.1.3] compiled at May 12 2021 16:12:47**********",1,0},{"STACK INIT OK",1,0},{"CREATE DB SUCCESS",1,0},{"ble adv && resp changed",1,0},{"appm start advertising",1,0},{"[NET] DHCP: device 38:1f:8d:f3:bd:35 got 192.168.1.100 (gateway/DNS 192.168.1.1)",1,0},{"\"gwId\":\"bf9014e5bafa4fa172qfyf\",\"active\":2,\"ablilty\":0,\"encrypt\":true,\"productKey\":\"keyu5sjrcfhjsvd8\"",1,0}};
static const PinWant c131_pins[] = {{0,120},{1,120},{6,60},{9,0},{10,120},{11,120},{12,120},{13,120},{24,0},{26,60},{28,60}};
static const char *const c132_args[] = {"--only-uart","-chip","BK7231N","--ble-core"};
static const ExpectStr c132_exp[] = {{"SetupQRCode: [MT:--VJ0-IC005WSL02810]",1,0},{"Copy/paste the below URL in a browser to see the QR Code:https://project-chip.github.io/connectedhomeip/qrcode.html\?data=MT%3A--VJ",1,0}};
static const char *const c133_args[] = {"--only-uart","-chip","BK7238","--ble-core"};
static const ExpectStr c133_exp[] = {{"prvHeapInit-start addr:0x415098, size:175976",1,0},{"**********tuya_upgrade_main need upgrade\? [255]**********",1,0},{"< BUILD AT:2025_01_02_15_55_27 BY ci_manage FOR tuyaos-iot AT T1 >",1,0},{"name:oem_t1_plug",1,0},{"tuya_tls_rand_init ok!",1,0},{"calibration_main over",1,0},{"!! tkl_wifi_station_fast_connect",1,0},{"found scan rst rssi -34 > -50",1,0}};
static const char *const c134_args[] = {"--only-uart","-chip","BK7238","--ble-core"};
static const ExpectStr c134_exp[] = {{"prvHeapInit-start addr:0x415118, size:175848",1,0},{"**********tuya_upgrade_main need upgrade\? [255]**********",1,0},{"< BUILD AT:2025_01_02_15_55_27 BY ci_manage FOR tuyaos-iot AT T1 >",1,0},{"name:oem_t1_plug",1,0},{"tuya_tls_rand_init ok!",1,0},{"calibration_main over",1,0},{"mf_init succ",1,0},{"scan_start_req_handler",1,0}};
static const char *const c135_args[] = {"--only-uart","-chip","BK7238","--ble-core"};
static const ExpectStr c135_exp[] = {{"prvHeapInit-start addr:0x414e60, size:176544",1,0},{"sdk_info:< TuyaOS V:3.11.11 BS:40.00_PT:2.3_LAN:3.5_CAD:1.0.5_CD:1.0.0 >",1,0},{"wifi soc init. pid:ocz3mqv0v4ntkzva firmwarekey:NULL ver:1.0.6",1,0},{"serial_no:80647ce3bf55",1,0},{"start ap success:SmartLife-BF55",1,0},{"ty bt sdk init success finish",1,0},{"ble central init success",1,0},{"app_key_task start!!!",1,0}};
static const char *const c136_args[] = {"--only-uart","-chip","BK7238","--ble-core"};
static const ExpectStr c136_exp[] = {{"prvHeapInit-start addr:0x415120, size:175840",1,0},{"**********tuya_upgrade_main need upgrade\? [255]**********",1,0},{"< BUILD AT:2025_01_08_15_12_50 BY ci_manage FOR tuyaos-iot AT T1 >",1,0},{"tuya_tls_rand_init ok!",1,0},{"calibration_main over",1,0},{"gw_cntl->gw_wsm.stat:1",1,0},{"mf_init succ",1,0},{"scan_start_req_handler",1,0}};
static const char *const c137_args[] = {"--only-uart","-chip","BK7238","--ble-core"};
static const ExpectStr c137_exp[] = {{"[MEM DBG] heap init-------size:199672 addr:0x40f408---------",1,0},{"< TuyaOS V:3.11.16 BS:40.00_PT:2.3_LAN:3.5_CAD:1.0.5_CD:1.0.0 >",1,0},{",over_cur: is 63000",1,0},{"tuya_tls_rand_init ok!",1,0},{"!! tkl_wifi_station_fast_connect",1,0},{"found scan rst rssi -37 > -50",1,0}};
static const char *const c138_args[] = {"--only-uart","-chip","BK7231N","-key","TUYA","--ble-core"};
static const ExpectStr c138_exp[] = {{"prvHeapInit-start addr:0x410958, size:128680",1,0},{"ble mac:50-8b-b9-6e-c3-ba",1,0},{"ty bt sdk init success finish",1,0},{"< TUYA IOT SDK V:2.3.3 BS:40.00_PT:2.2_LAN:3.4_CAD:1.0.5_CD:1.0.0 >",1,0},{"< BUILD AT:2022_12_22_14_55_08 BY ci_manage FOR ty_iot_sdk AT bk7231n >",1,0},{"gapm_cmp_evt:GAPM_SET_DEV_CONFIG",1,0}};
static const char *const c139_args[] = {"--only-uart","-chip","BK7231N","-key","TUYA","--ble-core"};
static const ExpectStr c139_exp[] = {{"prvHeapInit-start addr:0x411930, size:124624",1,0},{"ble mac:18-de-50-ea-4c-c4",1,0},{"ty bt sdk init success finish",1,0},{"gapm_cmp_evt:BLE_STACK_OK",1,0},{"[PLATFORM NOTICE]STACK INIT OK",1,0},{"[PLATFORM NOTICE]CREATE DB SUCCESS",1,0}};
static const char *const c140_args[] = {"--only-uart","-chip","BK7231N","-key","TUYA","--ble-core"};
static const ExpectStr c140_exp[] = {{"[MEM DBG] heap init-------size:153512 addr:0x40a858---------",1,0},{"tuya_tls_rand_init ok!",1,0},{"mf_init succ",1,0},{"< BUILD AT:2024_12_12_17_45_32 BY ci_manage FOR tuyaos-iot AT bk7231n >",1,0},{"calibration_main over",1,0},{"current product ssid name:tuya_mdev_test2",1,0},{"scan_start_req_handler",1,0}};
static const char *const c141_args[] = {"--only-uart","-chip","BK7231N","-key","TUYA","--ble-core"};
static const ExpectStr c141_exp[] = {{"prvHeapInit-start addr:0x4114a0, size:125792",1,0},{"< TUYA IOT SDK V:2.3.1 BS:40.00_PT:2.2_LAN:3.3_CAD:1.0.3_CD:1.0.0 >",1,0},{"fj_wifi_ble_controller_moto:1.0.2",1,0},{"[ble_appm_start_advertising]",1,0},{"mf_init succ",1,0},{"tuya_hard_init is success",1,0},{"gw_cntl.gw_if.product_key:s2vw3kf1575apfyc, input:s2vw3kf1575apfyc",1,0},{"scan_start_req_handler",1,0}};
static const char *const c142_args[] = {"--only-uart","-chip","BK7231N","-key","TUYA","--ble-core"};
static const ExpectStr c142_exp[] = {{"prvHeapInit-start addr:0x412040, size:122816",1,0},{"gapm_cmp_evt:BLE_STACK_OK",1,0},{"[ble_appm_start_advertising]",1,0},{"calibration_main over",1,0},{"app lighting is enable now",1,0},{"Succeed to malloc space for app_light_cfg.seg_list",1,0},{"lighting effect main task in running",1,0}};
static const char *const c143_args[] = {"--only-uart","-chip","BK7231N","-key","TUYA","--ble-core"};
static const ExpectStr c143_exp[] = {{"prvHeapInit-start addr:0x411550, size:125616",1,0},{"< TUYA IOT SDK V:2.3.1 BS:40.00_PT:2.2_LAN:3.3_CAD:1.0.3_CD:1.0.0 >",1,0},{"gapm_cmp_evt:BLE_STACK_OK",1,0},{"[ble_appm_start_advertising]",1,0},{"mf_init succ",1,0},{"key:infre:-----val:7----len:1",1,0},{"key:infrr:-----val:8----len:1",1,0},{"current product ssid name:tuya_mdev_test1",1,0}};
static const char *const c144_args[] = {"--only-uart","-key","TUYA"};
static const ExpectStr c144_exp[] = {{"< TUYA IOT SDK V:1.0.6 BS:40.00_PT:2.2_LAN:3.3_CAD:1.0.2_CD:1.0.0 >",1,0},{"**********[oem_bk7231s_rnd_zero_dctrl_switch] [1.1.5] compiled at Jun 22 2021",1,0},{"mf_init succ",1,0},{"gw_cntl.gw_if.product_key:key7axydcvmea3x9, input:key7axydcvmea3x9",1,0},{"device_init ok  free_mem_size:67688",1,0},{"appm start advertising",1,0}};
static const char *const c145_args[] = {"--only-uart","-key","TUYA"};
static const ExpectStr c145_exp[] = {{"< TUYA IOT SDK V:1.0.2 BS:40.00_PT:2.2_LAN:3.3_CAD:1.0.2_CD:1.0.0 >",1,0},{"**********[oem_bk7231s_rnd_switch] [1.1.0] compiled at Oct  3 2020 15:09:14",1,0},{"mf_init succ",1,0},{"gw_cntl.gw_if.product_key:keya4gvchmtapm8n, input:keya4gvchmtapm8n",1,0},{"IO - wifi_stat:\tpin-9\ttype low",1,0},{"device_init ok  free_mem_size:66344",1,0},{"appm start advertising",1,0}};
static const char *const c146_args[] = {"--only-uart","-key","TUYA"};
static const ExpectStr c146_exp[] = {{"< TUYA IOT SDK V:1.0.2 BS:40.00_PT:2.2_LAN:3.3_CAD:1.0.2_CD:1.0.0 >",1,0},{"**********[oem_bk7231_2onoff_wetemp] [1.2.8] compiled at Jul 16 2021 14:33:15",1,0},{"ble adv && resp changed",1,0},{"appm start advertising",1,0},{"mf_init succ",1,0},{"scan_start_req_handler",1,0}};
static const char *const c147_args[] = {"--only-uart","-key","TUYA"};
static const ExpectStr c147_exp[] = {{"--------------> PID:xhsx0clcbovz5xre",1,0},{"**********[HYS_03_010_RGB_xhsx0clcbovz5xre] [1.0.2] compiled at Dec 21 2020 16:13:43",1,0},{"appm start advertising",1,0},{"mf_init succ",1,0},{"gw_cntl.gw_if.product_key:xhsx0clcbovz5xre, input:xhsx0clcbovz5xre",1,0},{"frame init ok!",1,0},{"==== tuya_hal_adc_init enter ==== channel:3",1,0}};
static const char *const c148_args[] = {"--only-uart","-key","TUYA"};
static const ExpectStr c148_exp[] = {{"< TUYA IOT SDK V:1.0.2 BS:40.00_PT:2.2_LAN:3.3_CAD:1.0.2_CD:1.0.0 >",1,0},{"**********[oem_bk7231s_dltj] [1.3.5] compiled at Dec 25 2020 10:09:10**********",1,0},{"mf_init succ",1,0},{"gw_cntl.gw_if.product_key:keym9qkuywghyrvs, input:keym9qkuywghyrvs",1,0},{"dltj_run_int",1,0},{"device_init ok  free_mem_size:67376",1,0},{"appm start advertising",1,0}};
static const char *const c149_args[] = {"--only-uart","-key","TUYA"};
static const ExpectStr c149_exp[] = {{"**********[oem_bk7231s_light_ty] [2.9.6] compiled at Oct 29 2020 14:38:00**********",1,0},{"mf_init succ",1,0},{"gw_cntl.gw_if.product_key:keytg5kq8gvkv9dh, input:keytg5kq8gvkv9dh",1,0},{"frame init ok!",1,0},{"ssid:SmartLife-8811\tkey:  channnel: 6",1,0},{"appm start advertising",1,0},{"start AP config auto blink",1,0}};
static const char *const c150_args[] = {"--only-uart","-key","TUYA"};
static const ExpectStr c150_exp[] = {{"prvHeapInit-start addr:0x421b18, size:124136",1,0},{"crc_r[ca] crc_g[00] crc_b[67]",1,0},{"goto first bright up!",1,0},{"ty bt sdk init success finish",1,0},{"mf_init succ",1,0},{"< TUYA IOT SDK V:2.3.2 BS:40.00_PT:2.2_LAN:3.3_CAD:1.0.4_CD:1.0.0 >",1,0},{"< BUILD AT:2022_02_25_14_49_05 BY ci_manage FOR ty_iot_sdk AT bk7231t >",1,0}};
static const char *const c151_args[] = {"--only-uart","-key","TUYA"};
static const ExpectStr c151_exp[] = {{"< TUYA IOT SDK V:1.0.6 BS:40.00_PT:2.2_LAN:3.3_CAD:1.0.2_CD:1.0.0 >",1,0},{"**********[oem_bk7231s_rnd_zero_dctrl_switch] [1.1.5] compiled at Jun 22 2021",1,0},{"appm start advertising",1,0},{"mf_init succ",1,0},{"pConfig = {rl1_lv:1,bt_type:0,bt1_pin:6,mxcl_led_m:0,led1_pin:11,rand_dpid:18",1,0},{"gw_cntl.gw_if.product_key:key7axydcvmea3x9, input:key7axydcvmea3x9",1,0},{"device_init ok  free_mem_size:68040",1,0}};
static const char *const c152_args[] = {"--only-uart","-key","TUYA","--uart1-rx","DeepSleep 10","--deep-sleep-wake","rtc"};
static const ExpectStr c152_exp[] = {{"Warn:CFG:CFG_InitAndLoad: Correct config has been loaded with 0 changes count.",1,0},{"[SLEEP] entered deep sleep (RTC=10s)",1,0},{"[SLEEP] waking from deep sleep (RTC timer); device reset #1",1,0},{"Info:CMD:StartupBeforeConsole",1,0}};
static const char *const c152_tags[] = {"deep sleep"};
static const char *const c153_args[] = {"--only-uart","-key","TUYA","--uart1-rx","SetPinRole 10 Btn\\nPinDeepSleep","--deep-sleep-wake","gpio:10"};
static const ExpectStr c153_exp[] = {{"Warn:CFG:CFG_InitAndLoad: Correct config has been loaded with 0 changes count.",1,0},{"[SLEEP] entered deep sleep (GPIO=[P10:LOW])",1,0},{"[SLEEP] waking from deep sleep (GPIO P10=0); device reset #1",1,0},{"Warn:CFG:CFG_InitAndLoad: Correct config has been loaded with 1 changes count.",1,0}};
static const char *const c153_tags[] = {"deep sleep"};
static const char *const c154_args[] = {"--only-uart","-chip","BK7231N","-key","TUYA","--wifi","--deep-sleep-wake","gpio:16"};
static const ExpectStr c154_exp[] = {{"upload_by_wake_event,cur_wake_type:0",1,0},{"enter sleep mode,sleep_timer is 28800",1,0},{"[SLEEP] entered deep sleep (RTC=28800s, GPIO=[P16:HIGH, P20:HIGH])",1,0},{"[SLEEP] waking from deep sleep (GPIO P16=1); device reset #1",1,0},{"upload_by_wake_event,cur_wake_type:3",1,0}};
static const char *const c154_tags[] = {"deep sleep"};
static const char *const c155_args[] = {"--only-uart","-chip","BK7231N","-key","TUYA","--gpio-input","20:1","--deep-sleep-wake","gpio:16"};
static const ExpectStr c155_exp[] = {{"oem_bk7231n_water_sensor_plus:1.1.9",1,0},{"power_on wake up",1,0},{"enter sleep mode,sleep_timer is 28800",1,0},{"[SLEEP] entered deep sleep (RTC=28800s, GPIO=[P16:HIGH, P20:LOW])",1,0},{"[SLEEP] waking from deep sleep (GPIO P16=1); device reset #1",1,0},{"IO wake up",1,0},{"g_dev_ctl.wake_event is :2",1,0}};
static const char *const c155_tags[] = {"deep sleep"};
static const char *const c156_args[] = {"--only-uart","-chip","BK7231N","-key","TUYA","--gpio-input","20:1","--deep-sleep-wake","rtc"};
static const ExpectStr c156_exp[] = {{"oem_bk7231n_temp_hum_sensor:1.1.17",1,0},{"Wakeup_GetEvent is:0",1,0},{"sleep sec is:3600",1,0},{"[SLEEP] entered deep sleep (RTC=3600s, GPIO=[P16:HIGH, P20:LOW])",1,0},{"[SLEEP] waking from deep sleep (RTC timer); device reset #1",1,0},{"Wakeup_GetEvent is:4",1,0}};
static const char *const c156_tags[] = {"deep sleep"};
static const char *const c157_args[] = {"--only-uart","-chip","BK7238","--wifi","--deep-sleep-wake","rtc"};
static const ExpectStr c157_exp[] = {{"start_type:0",1,0},{"---enter deep sleep :wake up with gpio 0~31 ps: 0x300 0x100",1,0},{"---enter deep sleep :wake up with   rosc  ps :240 s",1,0},{"[SLEEP] entered deep sleep (RTC=240s, GPIO=[P8:LOW, P9:HIGH])",1,0},{"[SLEEP] waking from deep sleep (RTC timer); device reset #1",1,0},{"start_type:4",1,0}};
static const char *const c157_tags[] = {"deep sleep"};
static const char *const c158_args[] = {"--only-uart","-chip","BK7252N","--uart1-rx","video_buffer open\\nvideo_buffer read 4096\\n"};
static const ExpectStr c158_exp[] = {{"0328c id_data:9d",1,0},{"GC0328C init finish",1,0},{"jpeg opened",1,0},{"vbuf opened",1,0},{"[CAMERA] GC0328C streaming 640x480 JPEG (1180 bytes/frame",1,0},{"get frame ret: 0, len:1185",1,0},{"ff,d8,ff,e0,00,10,4a,46,49,46,",1,0}};
static const char *const c158_tags[] = {"camera","EJPEG"};
static const char *const c159_args[] = {"--only-uart","-chip","BK7252","--uart1-rx","video open\\n"};
static const ExpectStr c159_exp[] = {{"video_transfer_main entry",1,0},{"open I2C1",1,0},{"0328c id_data:9d",1,0},{"GC0328C init finish",1,0},{"[CAMERA] GC0328C streaming 640x480 JPEG (1180 bytes/frame",1,0}};
static const char *const c159_tags[] = {"camera","EJPEG"};
static const char *const c160_args[] = {"--only-uart","-chip","BK7252","--wifi-peer","auto"};
static const ExpectStr c160_exp[] = {{"SDK Rev: 3.0.29",1,0},{"SC008-WB00:1.0.4",1,0},{"wifi soc init. pid:keyph8vwhadwsx3a",1,0},{"serial_no:381f8da92640",1,0},{"[WIFI] associated with 'SmartLife-2640' (AID 1)",1,0},{"[NET] DHCP: we got 192.168.175.100/255.255.255.0 from the device (router 192.168.175.1)",1,0},{"[NET] ping reply from 192.168.175.1 (seq 1)",1,0}};
static const char *const c160_tags[] = {"camera"};
static const char *const c161_args[] = {"--only-uart","-chip","BK7252","-key","TUYA"};
static const ExpectStr c161_exp[] = {{"prvHeapInit-start addr:0x420cb0, size:127824",1,0},{"[MEM DBG] heap init-------size:229376 addr:0x908000---------",1,0},{"SDK Rev: 3.0.29",1,0},{"ty bt sdk init success finish",1,0},{"firmware compiled at [Sep 13 2024 14:31:56]",1,0},{"key_addr: 0x397000   block_sz 4096",1,0},{"0xc3 0x3d 0x40 0x5b 0x90 0xfa 0x59 0x81 0x20 0x31 0x54 0xd8 0x80 0xa9 0x8f 0xc",1,0}};
static const char *const c161_tags[] = {"camera"};
static const char *const c162_args[] = {"--only-uart","-chip","BK7252","-key","TUYA"};
static const ExpectStr c162_exp[] = {{"prvHeapInit-start addr:0x420cb8, size:127816",1,0},{"[MEM DBG] heap init-------size:229376 addr:0x908000---------",1,0},{"SDK Rev: 3.0.29",1,0},{"ty bt sdk init success finish",1,0},{"SC028_WB30_ALK:2.2.4",1,0},{"firmware compiled at [Jul 20 2023 14:41:22]",1,0},{"key_addr: 0x397000   block_sz 4096",1,0},{"0x89 0x33 0xfa 0xd1 0x58 0xfb 0x7f 0x3b 0xe7 0x24 0x97 0xd2 0x15 0xb2 0x6f 0xbd",1,0}};
static const char *const c162_tags[] = {"camera"};
static const char *const c163_args[] = {"--only-uart","-chip","BK7238","--ble-core","--wifi-peer","auto"};
static const ExpectStr c163_exp[] = {{"[WIFI] device RX filter 0x7fffffde: monitor mode, every frame on air accepted (sniffing, as in EZ pairing)",1,0},{"[WIFI] device RX filter 0x3503848c: back to normal reception",1,0},{"[WIFI] found BSS 'JIEWU' 12:71:11:71:0b:71 ch 6 (privacy)",1,0},{"[BLE_ADV]   16-bit UUIDs               e1 ff                                   ..  uuid=0xFFE1",1,0},{"[BLE_ADV]   Manufacturer Data          53 50 63 10 54 20 24 00 24 bd           SPc.T $.$.  company=0x5053",1,0},{"[BLE_ADV]   Complete Name              53 50 35 34 38 45",1,0},{"SP548E",1,0}};
static const PinWant c163_pins[] = {{0,2},{1,2},{6,72},{7,72},{8,72},{10,120},{11,120},{14,72},{15,72},{16,72},{17,72},{21,60},{22,60},{24,72},{26,72},{28,60}};
static const PwmWant c163_pwm[] = {{0,1625,0},{1,1625,0},{2,1625,0},{4,1625,0},{5,1625,0}};
static const char *const c163_tags[] = {"BLE","BLE core","FIQ","WiFi MAC"};
static const char *const c164_args[] = {"--only-uart","-chip","BK7238","--ble-core"};
static const ExpectStr c164_exp[] = {{"prvHeapInit-start addr:0x415120, size:175840",1,0},{"**********tuya_upgrade_main need upgrade\? [255]**********",1,0},{"sdk_info:< TuyaOS V:3.8.31 BS:40.00_PT:2.3_LAN:3.5_CAD:1.0.5_CD:1.0.0 >",1,0},{"name:oem_t1_switch:1.1.9",1,0},{"tuya_tls_rand_init ok!",1,0},{"calibration_main over",1,0},{"mf_init succ",1,0},{"sg_net_led.sg_net_led_count:1",1,0},{"mac  0:33:7a:11:43:5f",1,0},{"scan_start_req_handler",1,0}};
static const PinWant c164_pins[] = {{0,120},{1,120},{9,0},{10,120},{11,120},{26,0}};
static const char *const c165_args[] = {"--only-uart","-chip","BK7238","--ble-core","--wifi-peer","auto"};
static const ExpectStr c165_exp[] = {{"[MEM DBG] heap init-------size:199672 addr:0x40f408---------",1,0},{"< TuyaOS V:3.11.16 BS:40.00_PT:2.3_LAN:3.5_CAD:1.0.5_CD:1.0.0 >",1,0},{"oem_t1_cutout_self_switch:1.1.0",1,0},{",over_cur: is 50000",1,0},{"0xe4 0xe5 0xc7 0x35 0x95 0xce 0x2e 0xf3 0x4e 0x11 0xcd 0x54 0x78 0xa9 0xb1 0x22",1,0},{"tuya_tls_rand_init ok!",1,0},{"tuya_ele_hw_init success",1,0},{"dltj rslt ok!",1,0},{"wifi soc init. pid:key7mapd9sdpyyf9 firmwarekey:key7mapd9sdpyyf9 ver:1.1.0",1,0}};
static const PinWant c165_pins[] = {{0,120},{1,120},{6,0},{10,120},{11,120},{15,2},{17,2},{21,60},{26,0}};
static const char *const c166_args[] = {"--only-uart","-chip","BK7238","--ble-core","--uart1-hex","--tuyamcu","--tuyamcu-pid","y1kgvuhjplpctr1o","--tuyamcu-raw"};
static const ExpectStr c166_exp[] = {{"prvHeapInit-start addr:0x416278, size:171400",1,0},{"< TuyaOS V:3.11.11 BS:40.00_PT:2.3_LAN:3.5_CAD:1.0.5_CD:1.0.0 >",1,0},{"t1_common_user_config_ty sw ver:3.0.22",1,0},{"0x6f 0xe9 0xe2 0x30 0xcb 0x7f 0x2b 0xdf 0xbd 0x69 0x87 0x6b 0xe0 0xc5 0x4 0xfc",1,0},{"read baud rate 115200",1,0},{"[UART1/MCU] 55 aa 00 00 00 00 ff 55 aa 00 01 00 00 00 55 aa 00 02 00 00 01",1,0},{"VER:1.0.0,mcu tp :9",1,0},{"gw_cntl->gw_if.product_key:y1kgvuhjplpctr1o, input:y1kgvuhjplpctr1o",1,0},{"enter success_proc",1,0},{"serial_no:20f1b21a10c4",1,0},{"scan_start_req_handler",1,0}};
static const PinWant c166_pins[] = {{0,120},{1,120},{10,120},{11,120},{14,840},{15,842},{16,840},{17,840}};
static const char *const c166_tags[] = {"TuyaMCU"};
static const char *const c167_args[] = {"--only-uart","-chip","BK7238","--ble-core","--adc","7:264"};
static const ExpectStr c167_exp[] = {{"[MEM DBG] heap init-------size:199688 addr:0x40f3f8---------",1,0},{"< TuyaOS V:3.11.16 BS:40.00_PT:2.3_LAN:3.5_CAD:1.0.5_CD:1.0.0 >",1,0},{"oem_t1_cutout_self_switch:1.0.3",1,0},{",temp_mea: is 20",1,0},{",temp_fun_en: is 1",1,0},{"0x11 0x14 0x4f 0x86 0xa2 0x37 0xaf 0xbe 0x8c 0xa9 0x55 0xf2 0xd1 0xec 0x74 0xf1",1,0},{"cal_bias!",1,0},{"cal dpll!",1,0},{"adc ntc init ok!",1,0},{"tuya_ele_hw_init success",1,0},{"temp test ok!",1,0},{"scan_start_req_handler",1,0},{"adc_trend:[13]:264->[23]:164",1,0}};
static const char *const c167_unexp[] = {"adc ddev_open error","adc_read err"};
static const PinWant c167_pins[] = {{0,120},{1,120},{6,0},{8,0},{10,120},{11,120},{17,60},{21,0},{28,0}};
static const char *const c167_tags[] = {"SARADC"};
static const char *const c168_args[] = {"--only-uart","-chip","BK7238","--ble-core","--uart1-hex","--tuyamcu","--tuyamcu-pid","rknwi0ctbbghzgla","--tuyamcu-raw"};
static const ExpectStr c168_exp[] = {{"prvHeapInit-start addr:0x416278, size:171400",1,0},{"< TuyaOS V:3.11.11 BS:40.00_PT:2.3_LAN:3.5_CAD:1.0.5_CD:1.0.0 >",1,0},{"t1_common_user_config_ty sw ver:3.0.22",1,0},{"0xf5 0x61 0xb 0x20 0x72 0xd7 0xea 0x8b 0xb4 0x9 0x6c 0xa1 0x51 0xcd 0x41 0x42",1,0},{"read baud rate 115200",1,0},{"[UART1/MCU] 55 aa 00 00 00 00 ff 55 aa 00 01 00 00 00 55 aa 00 02 00 00 01",1,0},{"reset wifi link params to factory state, nc_tp:10 md:0 mthd:0 start mode:4",1,0},{"gw_cntl->gw_if.product_key:rknwi0ctbbghzgla, input:rknwi0ctbbghzgla",1,0},{"enter reset factory",1,0},{"enter success_proc",1,0},{"serial_no:3c0b59a46608",1,0},{"scan_start_req_handler",1,0}};
static const PinWant c168_pins[] = {{0,120},{1,120},{10,120},{11,120},{14,840},{15,842},{16,840},{17,840}};
static const char *const c168_tags[] = {"TuyaMCU"};
static const char *const c169_args[] = {"--only-uart","-chip","BK7238","--ble-core"};
static const ExpectStr c169_exp[] = {{"prvHeapInit-start addr:0x415120, size:175840",1,0},{"sdk_info:< TuyaOS V:3.8.31 BS:40.00_PT:2.3_LAN:3.5_CAD:1.0.5_CD:1.0.0 >",1,0},{"name:oem_t1_switch:1.1.9",1,0},{"tuya_tls_rand_init ok!",1,0},{"calibration_main over",1,0},{"mf_init succ",1,0},{"dctrl channel_num:4",1,0},{"sg_net_led.sg_net_led_count:1",1,0},{"mac  0:33:7a:a3:22:bf",1,0},{"scan_start_req_handler",1,0}};
static const PinWant c169_pins[] = {{0,120},{1,120},{9,0},{10,120},{11,120},{22,0},{23,0},{24,0},{26,0}};
static const char *const c170_args[] = {"--only-uart","-chip","BK7231N","-key","TUYA","--ble-core"};
static const ExpectStr c170_exp[] = {{"prvHeapInit-start addr:0x410958, size:128680",1,0},{"< TUYA IOT SDK V:2.3.3 BS:40.00_PT:2.2_LAN:3.4_CAD:1.0.5_CD:1.0.0 >",1,0},{"oem_bk7231n_dctrl_switch:1.3.5",1,0},{"ble mac:50-8b-b9-9c-32-0f",1,0},{"gapm_cmp_evt:BLE_STACK_OK",1,0},{"[ble_appm_start_advertising]",1,0},{"0xdb 0xe5 0xf5 0xde 0x2a 0x78 0xbe 0x7 0xa4 0x59 0xa1 0xc9 0xdb 0xbf 0x6b 0x95",1,0},{"mf_init succ",1,0},{"pConfig = {rl1_lv:1,netled_lv:0,bt_type:0,bt1_pin:14,rl3_pin:9,rl3_lv:1,rand_dpid:18",1,0},{"channel init success",1,0},{"scan_start_req_handler",1,0}};
static const PinWant c170_pins[] = {{0,120},{1,120},{10,120},{11,120},{12,120},{13,120},{21,60}};
static const char *const c171_args[] = {"--only-uart","-chip","BK7231N","-key","TUYA","--ble-core"};
static const ExpectStr c171_exp[] = {{"prvHeapInit-start addr:0x4114b0, size:125776",1,0},{"ble mac:b8-06-0d-26-f4-74",1,0},{"gapm_cmp_evt:BLE_STACK_OK",1,0},{"[BLE_ADV]   Service Data               01 a2 00                                ...  uuid=0xA201",1,0},{"0x6e 0x23 0x57 0x9a 0x92 0xfa 0x37 0xcf 0x37 0xf0 0xd4 0xf0 0xf7 0xa5 0xc 0x72",1,0},{"oem_bk7231n_curtain_onoff:1.0.6",1,0},{"power on cnt = 3 satisfy, unactive now!",1,0},{"[WDT] watchdog expired; device reset #1",1,0},{"power on cnt = 1",1,0},{"[RLY] - pin:24,\t io_drive:2",1,0},{"[RLY] - pin:26,\t io_drive:2",1,0},{"TOTAL KEY - pin:11,\t io_drive:1,\t long_press:5000",1,0},{"WIFI LED - pin:10,\t io_drive:1",1,0},{"serial_no:b8060d26f473",1,0}};
static const PinWant c171_pins[] = {{0,120},{1,120},{7,60},{8,60},{10,2},{11,60},{12,120},{13,120},{21,60},{24,0},{26,0}};
static const char *const c172_args[] = {"--only-uart","-key","TUYA","--wifi"};
static const ExpectStr c172_exp[] = {{"prvHeapInit-start addr:0x41fa88, size:132472",1,0},{"< TUYA IOT SDK V:1.0.2 BS:40.00_PT:2.2_LAN:3.3_CAD:1.0.2_CD:1.0.0 >",1,0},{"**********[oem_bk7231s_rnd_zero_dctrl_switch] [1.0.7] compiled at Dec 11 2020 18:22:59**********",1,0},{"0xcb 0x4e 0x3e 0xa4 0x0 0x30 0x9d 0xab 0x65 0x6d 0x8d 0xbf 0xe4 0xb9 0x3f 0x35",1,0},{"appm start advertising",1,0},{"mf_init succ",1,0},{"pConfig = {rl1_lv:1,netled_lv:1,bt_type:0,bt1_pin:10,mxcl_led_m:0,led1_pin:26,rand_dpid:18",1,0},{"serial_no:10d5614a30c5",1,0},{"gw_cntl.gw_if.product_key:key7axydcvmea3x9, input:key7axydcvmea3x9",1,0},{"IO - relay[0]:\tpin-9\ttype high",1,0},{"IO - backlit:\tpin-11\ttype high",1,0},{"device_init ok  free_mem_size:70520",1,0},{"ssid:SmartLife-30C5\tkey:  channnel: 6",1,0},{"wifi stat is 2",1,0}};
static const PinWant c172_pins[] = {{0,120},{1,0},{9,0},{10,60},{11,0},{12,120},{13,120},{26,0}};
static const char *const c173_args[] = {"--only-uart","-chip","BK7238","--ble-core","--wifi"};
static const ExpectStr c173_exp[] = {{"prvHeapInit-start addr:0x415098, size:175976",1,0},{"sdk_info:< TuyaOS V:3.8.31 BS:40.00_PT:2.3_LAN:3.5_CAD:1.0.5_CD:1.0.0 >",1,0},{"name:oem_t1_plug:1.0.2",1,0},{"tuya_tls_rand_init ok!",1,0},{"calibration_main over",1,0},{"mf_init succ",1,0},{"tdd_gpio_button_register succ",1,0},{"mac bc:35:1e:15:cd:31",1,0},{"wifi soc init. pid:keykmm3rjyqy5r8p firmwarekey:keykmm3rjyqy5r8p ver:1.0.2",1,0},{"enter success_proc",1,0},{"serial_no:bc351e15cd31",1,0},{"tdl_button_create succ",1,0}};
static const PinWant c173_pins[] = {{0,120},{1,2},{6,60},{8,60},{9,2},{10,120},{11,2},{24,0},{26,60}};
static const char *const c174_args[] = {"--only-uart","-chip","BK7238","--ble-core","--wifi"};
static const ExpectStr c174_exp[] = {{"prvHeapInit-start addr:0x415098, size:175976",1,0},{"sdk_info:< TuyaOS V:3.8.31 BS:40.00_PT:2.3_LAN:3.5_CAD:1.0.5_CD:1.0.0 >",1,0},{"name:oem_t1_plug:1.0.2",1,0},{"init protected data length 505 wr_cnt 9",1,0},{"tuya_tls_rand_init ok!",1,0},{"temp in flash is:249",1,0},{"mf_init succ",1,0},{"mac bc:35:1e:3f:c3:f9",1,0},{"wifi soc init. pid:keykmm3rjyqy5r8p firmwarekey:keykmm3rjyqy5r8p ver:1.0.2",1,0},{"enter success_proc",1,0},{"serial_no:bc351e3fc3f9",1,0},{"tdl_button_create succ",1,0}};
static const PinWant c174_pins[] = {{0,120},{1,2},{6,60},{8,60},{9,2},{10,120},{11,2},{24,0},{26,60}};
static const char *const c175_args[] = {"--only-uart","-chip","BK7238","--ble-core","--wifi"};
static const ExpectStr c175_exp[] = {{"prvHeapInit-start addr:0x415120, size:175840",1,0},{"sdk_info:< TuyaOS V:3.8.31 BS:40.00_PT:2.3_LAN:3.5_CAD:1.0.5_CD:1.0.0 >",1,0},{"name:oem_t1_switch:1.1.9",1,0},{"init protected data length 506 wr_cnt 22",1,0},{"tuya_tls_rand_init ok!",1,0},{"mf_init succ",1,0},{"mac  0:33:7a:ca:50:c1",1,0},{"wifi soc init. pid:keyfxuj3d5wg53wx firmwarekey:keyfxuj3d5wg53wx ver:1.1.9",1,0},{"serial_no:00337aca50c1",1,0},{"ssid:SmartLife-50C1, key:, channel: 6",1,0},{"ble mac:00-33-7a-ca-50-c2",1,0},{"ty bt sdk init success finish",1,0},{"[BLE_ADV]   Service Data               50 fd 43 04 00 10 6b 65 79 66 78 75 ..  P.C...keyfxuj3d5wg53wx  uuid=0xFD50",1,0},{"ble adv updated",1,0}};
static const PinWant c175_pins[] = {{0,120},{1,120},{6,60},{10,120},{11,120},{15,60},{26,0}};
static const char *const c176_args[] = {"--only-uart","-chip","BK7238","--ble-core","--wifi","--uart1-hex"};
static const ExpectStr c176_exp[] = {{"prvHeapInit-start addr:0x415098, size:175976",1,0},{"sdk_info:< TuyaOS V:3.8.31 BS:40.00_PT:2.3_LAN:3.5_CAD:1.0.5_CD:1.0.0 >",1,0},{"name:oem_t1_plug:1.0.2",1,0},{"init protected data length 632 wr_cnt 20",1,0},{"tuya_tls_rand_init ok!",1,0},{"mf_init succ",1,0},{"mac bc:35:1e:b4:d8:ea",1,0},{"wifi soc init. pid:keykmm3rjyqy5r8p firmwarekey:keykmm3rjyqy5r8p ver:1.0.2",1,0},{"tdl_button_create succ",1,0},{"ble mac:bc-35-1e-b4-d8-eb",1,0},{"ty bt sdk init success finish",1,0},{"[BLE_ADV]   Service Data               50 fd 43 04 00 10 6b 65 79 6b 6d 6d ..  P.C...keykmm3rjyqy5r8p  uuid=0xFD50",1,0},{"ble adv updated",1,0},{"[UART1/MCU] 58 aa 58 aa",1,0}};
static const PinWant c176_pins[] = {{0,120},{1,120},{6,2},{9,2},{10,120},{11,120},{21,2},{22,60},{24,2},{26,2}};
static const char *const c177_args[] = {"--only-uart","-chip","BK7238","--ble-core","--wifi","--uart1-hex"};
static const ExpectStr c177_exp[] = {{"prvHeapInit-start addr:0x4150d8, size:175912",1,0},{"sdk_info:< TuyaOS V:3.8.31 BS:40.00_PT:2.3_LAN:3.5_CAD:1.0.5_CD:1.0.0 >",1,0},{"name:oem_t1_switch:1.1.4",1,0},{"init protected data length 516 wr_cnt 9",1,0},{"tuya_tls_rand_init ok!",1,0},{"mf_init succ",1,0},{"mac 80:64:7c:6f:19:d7",1,0},{"wifi soc init. pid:keyfxuj3d5wg53wx firmwarekey:keyfxuj3d5wg53wx ver:1.1.4",1,0},{"serial_no:80647c6f19d7",1,0},{"[UART1/MCU] a8 1d 55 00 00 e5 a8 1c 5a 5a 5a 2d a8 1d 00 00 00 3a a8 1d 55 00 00 e5 58 1d",1,0},{"[UART1/MCU] a8 18 26 00 00 19 58 18",1,0},{"[UART1/MCU] 58 aa 58 aa",1,0}};
static const char *const c177_unexp[] = {"Failed to open user write protect","Failed to set CF1 to voltage zero cross"};
static const PinWant c177_pins[] = {{0,120},{1,2},{8,0},{9,60},{10,120},{11,120},{24,60}};
static const char *const c178_args[] = {"--only-uart","-chip","BK7238","--ble-core","--wifi","--tuyamcu","--tuyamcu-raw","--tuyamcu-pid","pxrmxy5zzzhfklxh"};
static const ExpectStr c178_exp[] = {{"prvHeapInit-start addr:0x416278, size:171400",1,0},{"< TuyaOS V:3.11.11 BS:40.00_PT:2.3_LAN:3.5_CAD:1.0.5_CD:1.0.0 >",1,0},{"t1_common_user_config_ty sw ver:3.0.22",1,0},{"0x95 0xaa 0xea 0x10 0xab 0x53 0xe4 0x5c 0x96 0xb2 0x4b 0x78 0x1e 0xf5 0x43 0xfc",1,0},{"gw_cntl->gw_if.product_key:pxrmxy5zzzhfklxh, input:pxrmxy5zzzhfklxh",1,0},{"gw_cntl->gw_if.firmware_key:key5s5pkkwnfwgvr, input:key5s5pkkwnfwgvr",1,0},{"enter success_proc",1,0},{"serial_no:80647c9cfe5f",1,0},{"ssid:SmartLife-FE5F, key:, channel: 6",1,0},{"ble mac:80-64-7c-9c-fe-60",1,0},{"ty bt sdk init success finish",1,0},{"[BLE_ADV]   Service Data               50 fd 43 04 00 10 70 78 72 6d 78 79 ..  P.C...pxrmxy5zzzhfklxh  uuid=0xFD50",1,0}};
static const PinWant c178_pins[] = {{0,120},{1,120},{10,120},{11,120},{14,840},{15,842},{16,840},{17,840}};
static const char *const c179_args[] = {"--only-uart","-chip","BK7238","--ble-core","--wifi","--tuyamcu","--tuyamcu-raw","--tuyamcu-pid","wifech3utowiyknu"};
static const ExpectStr c179_exp[] = {{"prvHeapInit-start addr:0x416278, size:171400",1,0},{"< TuyaOS V:3.11.11 BS:40.00_PT:2.3_LAN:3.5_CAD:1.0.5_CD:1.0.0 >",1,0},{"t1_common_user_config_ty sw ver:3.0.22",1,0},{"0x3e 0x16 0xe1 0x9c 0x25 0xd7 0x2a 0x42 0xa 0xa5 0xbc 0x53 0x65 0xf0 0x7a 0x86",1,0},{"gw_cntl->gw_if.product_key:wifech3utowiyknu, input:wifech3utowiyknu",1,0},{"gw_cntl->gw_if.firmware_key:key5s5pkkwnfwgvr, input:key5s5pkkwnfwgvr",1,0},{"enter success_proc",1,0},{"serial_no:00337a3e9943",1,0},{"ssid:SmartLife-9943, key:, channel: 6",1,0},{"ble mac:00-33-7a-3e-99-44",1,0},{"ty bt sdk init success finish",1,0},{"[BLE_ADV]   Service Data               50 fd 43 04 00 10 77 69 66 65 63 68 ..  P.C...wifech3utowiyknu  uuid=0xFD50",1,0}};
static const PinWant c179_pins[] = {{0,120},{1,120},{10,120},{11,120},{14,840},{15,842},{16,840},{17,840}};
static const char *const c180_args[] = {"--only-uart","-chip","BK7238","--ble-core","--wifi","--tuyamcu","--tuyamcu-raw","--tuyamcu-pid","z3jngbyubvwgfrcv"};
static const ExpectStr c180_exp[] = {{"prvHeapInit-start addr:0x416278, size:171400",1,0},{"< TuyaOS V:3.11.11 BS:40.00_PT:2.3_LAN:3.5_CAD:1.0.5_CD:1.0.0 >",1,0},{"t1_common_user_config_ty sw ver:3.0.22",1,0},{"0xbc 0x89 0x26 0xa2 0x8a 0xaa 0x7c 0x4 0x74 0xfc 0xa8 0x93 0x65 0x25 0xac 0x63",1,0},{"gw_cntl->gw_if.product_key:z3jngbyubvwgfrcv, input:z3jngbyubvwgfrcv",1,0},{"gw_cntl->gw_if.firmware_key:key5s5pkkwnfwgvr, input:key5s5pkkwnfwgvr",1,0},{"enter success_proc",1,0},{"serial_no:bc351e6efc5c",1,0},{"ssid:SmartLife-FC5C, key:, channel: 6",1,0},{"ble mac:bc-35-1e-6e-fc-5d",1,0},{"ty bt sdk init success finish",1,0},{"[BLE_ADV]   Service Data               50 fd 43 04 00 10 7a 33 6a 6e 67 62 ..  P.C...z3jngbyubvwgfrcv  uuid=0xFD50",1,0}};
static const PinWant c180_pins[] = {{0,120},{1,120},{10,120},{11,120},{14,840},{15,842},{16,840},{17,840}};
static const char *const c181_args[] = {"--only-uart","-chip","BK7238","--ble-core","--wifi","--tuyamcu","--tuyamcu-raw","--tuyamcu-pid","xeagimantb7d7apb"};
static const ExpectStr c181_exp[] = {{"prvHeapInit-start addr:0x416278, size:171400",1,0},{"< TuyaOS V:3.11.11 BS:40.00_PT:2.3_LAN:3.5_CAD:1.0.5_CD:1.0.0 >",1,0},{"t1_common_user_config_ty sw ver:3.0.22",1,0},{"0xad 0x33 0x22 0x1b 0x31 0x32 0xc5 0x30 0xa4 0xe6 0x42 0x80 0xe3 0xe5 0xc9 0x44",1,0},{"gw_cntl->gw_if.product_key:xeagimantb7d7apb, input:xeagimantb7d7apb",1,0},{"gw_cntl->gw_if.firmware_key:key5s5pkkwnfwgvr, input:key5s5pkkwnfwgvr",1,0},{"enter success_proc",1,0},{"serial_no:e4aee44ea0fc",1,0},{"ssid:SmartLife-A0FC, key:, channel: 6",1,0},{"ble mac:e4-ae-e4-4e-a0-fd",1,0},{"ty bt sdk init success finish",1,0},{"[BLE_ADV]   Service Data               50 fd 43 04 00 10 78 65 61 67 69 6d ..  P.C...xeagimantb7d7apb  uuid=0xFD50",1,0}};
static const PinWant c181_pins[] = {{0,120},{1,120},{10,120},{11,120},{14,840},{15,842},{16,840},{17,840}};
static const char *const c182_args[] = {"--only-uart","-chip","BK7238","--ble-core","--wifi","--adc","7:258"};
static const ExpectStr c182_exp[] = {{"[MEM DBG] heap init-------size:193344 addr:0x410cc0---------",1,0},{"temp_code:86 - adc_code:129 - adc_trend:[13]:258->[23]:158",1,0},{"COMPILE_PLATFORM_TEMPLATE V1.6.9 BK7238 Build @Oct 27 2025 17:34:29",1,0},{"I[33]qne_kernel:QNE kernel init ok",1,0},{"0xa8 0x3 0xa 0xfd 0xc6 0x16 0x7b 0xda 0x61 0xe8 0xb 0xf3 0x88 0xef 0x92 0x6f",1,0},{"W[99]tuya_temphum:u32AdcValue      330",1,0},{"tuya_tls_rand_init ok!",1,0},{"mf_init APP_BIN_NAME[COMPILE_PLATFORM_TEMPLATE] USER_SW_VER[1.6.9]",1,0},{"mac e4:ae:e4:1b:92:c9",1,0},{"I[321]tuya_temphum:vTyWifi_TempHumNetChange",1,0},{"ble mac:e4-ae-e4-1b-92-ca",1,0}};
static const char *const c182_unexp[] = {"adc ddev_open error:1"};
static const PinWant c182_pins[] = {{0,120},{1,60},{10,120},{11,120},{17,2},{21,0},{22,60},{24,2},{26,8}};
static const char *const c183_args[] = {"--only-uart","-chip","BK7238","--ble-core","--wifi"};
static const ExpectStr c183_exp[] = {{"[WIFI] device RX filter 0x7fffffde: monitor mode",1,0},{"www.sparkleiot.com",1,0},{"ready",1,0}};
static const PinWant c183_pins[] = {{0,120},{1,120},{10,120},{11,120}};
static const char *const c184_args[] = {"--only-uart","-chip","BK7238","--ble-core","--wifi"};
static const ExpectStr c184_exp[] = {{"prvHeapInit-start addr:0x414e60, size:176544",1,0},{"< TuyaOS V:3.11.11 BS:40.00_PT:2.3_LAN:3.5_CAD:1.0.5_CD:1.0.0 >",1,0},{"name:tuyaos_curtain_switch_wifi:1.0.6",1,0},{"0x32 0x6e 0x0 0xdc 0x93 0x70 0x1e 0x6e 0x33 0x11 0x6b 0xb6 0x7e 0xde 0x9d 0x88",1,0},{"gw_cntl->gw_if.product_key:ocz3mqv0v4ntkzva, input:ocz3mqv0v4ntkzva",1,0},{"serial_no:80647ce3c90b",1,0},{"ssid:SmartLife-C90B, key:, channel: 6",1,0},{"ble mac:80-64-7c-e3-c9-0c",1,0},{"ty bt sdk init success finish",1,0},{"[BLE_ADV]   Service Data               50 fd 43 04 00 10 6f 63 7a 33 6d 71 ..  P.C...ocz3mqv0v4ntkzva  uuid=0xFD50",1,0},{"------ clear local config start ------",1,0}};
static const PinWant c184_pins[] = {{0,120},{1,120},{10,120},{11,120},{14,840},{15,842},{16,840},{17,840}};
static const char *const c185_args[] = {"--only-uart","-chip","BK7238","--ble-core","--wifi"};
static const ExpectStr c185_exp[] = {{"prvHeapInit-start addr:0x415120, size:175840",1,0},{"< TuyaOS V:3.8.31 BS:40.00_PT:2.3_LAN:3.5_CAD:1.0.5_CD:1.0.0 >",1,0},{"name:oem_t1_switch:1.1.9",1,0},{"init protected data length 505 wr_cnt 8",1,0},{"tuya_tls_rand_init ok!",1,0},{"mf_init succ",1,0},{"sg_net_led.is_mux:2",1,0},{"sg_net_led.sg_net_led_count:2",1,0},{"mac 80:64:7c:c9:ff:10",1,0},{"wifi soc init. pid:keyfxuj3d5wg53wx firmwarekey:keyfxuj3d5wg53wx ver:1.1.9",1,0},{"serial_no:80647cc9ff10",1,0}};
static const PinWant c185_pins[] = {{0,120},{1,120},{8,0},{9,0},{10,120},{11,120},{20,0},{22,0},{23,0}};
static const char *const c186_args[] = {"--only-uart","-chip","BK7238","--ble-core","--wifi"};
static const ExpectStr c186_exp[] = {{"[MEM DBG] heap init-------size:199688 addr:0x40f3f8---------",1,0},{"< TuyaOS V:3.11.16 BS:40.00_PT:2.3_LAN:3.5_CAD:1.0.5_CD:1.0.0 >",1,0},{"oem_t1_cutout_self_switch:1.0.3",1,0},{"IoT config ov_cur: 50000",1,0},{"0x74 0xd3 0xd1 0x81 0x21 0xe3 0xd3 0xbf 0x73 0x4c 0x48 0xb 0xbf 0x37 0xb8 0x2",1,0},{"tuya_tls_rand_init ok!",1,0},{"mf_init succ",1,0},{"[switch] wifi led init ok",1,0},{"tuya_ele_hw_init success",1,0},{"dltj rslt ok!",1,0},{"mac bc:35:1e:79:93:28",1,0}};
static const PinWant c186_pins[] = {{0,120},{1,120},{6,0},{10,120},{11,120},{15,2},{17,2},{21,60},{26,0}};
static const char *const c187_args[] = {"--only-uart","-chip","BK7238","--ble-core","--wifi"};
static const ExpectStr c187_exp[] = {{"prvHeapInit-start addr:0x415120, size:175840",1,0},{"< TuyaOS V:3.8.31 BS:40.00_PT:2.3_LAN:3.5_CAD:1.0.5_CD:1.0.0 >",1,0},{"name:oem_t1_switch:1.1.9",1,0},{"init protected data length 670 wr_cnt 33",1,0},{"tuya_tls_rand_init ok!",1,0},{"mf_init succ",1,0},{"sg_net_led.sg_net_led_count:1",1,0},{"mac 80:64:7c:fa:34:66",1,0},{"serial_no:80647cfa3466",1,0},{"ssid:SmartLife-3466, key:, channel: 6",1,0},{"--->net state change to:13",1,0}};
static const PinWant c187_pins[] = {{0,120},{1,120},{6,0},{8,60},{9,0},{10,120},{11,120},{24,2},{26,60}};
static const char *const c188_args[] = {"--only-uart","-chip","BK7238","--ble-core","--wifi","--tuyamcu","--tuyamcu-raw","--tuyamcu-pid","b8vqmdjmpswbap1q","--uart1-hex"};
static const ExpectStr c188_exp[] = {{"prvHeapInit-start addr:0x416278, size:171400",1,0},{"< TuyaOS V:3.11.11 BS:40.00_PT:2.3_LAN:3.5_CAD:1.0.5_CD:1.0.0 >",1,0},{"t1_common_user_config_ty sw ver:3.0.22",1,0},{"0x38 0xb1 0x61 0x32 0xb7 0x7c 0x63 0x85 0xf7 0xe2 0x0 0x53 0x67 0xee 0xb0 0x24",1,0},{"read baud rate 9600",1,0},{"VER:1.0.0,mcu tp :9",1,0},{"gw_cntl->gw_if.product_key:b8vqmdjmpswbap1q, input:b8vqmdjmpswbap1q",1,0},{"enter success_proc",1,0},{"serial_no:bc351e663469",1,0},{"mac bc:35:1e:66:34:69",1,0}};
static const PinWant c188_pins[] = {{0,120},{1,120},{10,120},{11,120},{14,840},{15,842},{16,840},{17,840}};
static const char *const c189_args[] = {"--only-uart","-chip","BK7238","--ble-core","--wifi","--tuyamcu","--tuyamcu-raw","--tuyamcu-pid","lwt5eraqqvwh3pfw","--uart1-hex"};
static const ExpectStr c189_exp[] = {{"prvHeapInit-start addr:0x416278, size:171400",1,0},{"< TuyaOS V:3.11.11 BS:40.00_PT:2.3_LAN:3.5_CAD:1.0.5_CD:1.0.0 >",1,0},{"t1_common_user_config_ty sw ver:3.0.22",1,0},{"0x23 0x59 0xf3 0x1 0x20 0xa8 0xd6 0x84 0x6b 0x71 0x6f 0x3e 0x39 0xe6 0xfb 0x34",1,0},{"read baud rate 9600",1,0},{"VER:1.0.0,mcu tp :9",1,0},{"gw_cntl->gw_if.product_key:lwt5eraqqvwh3pfw, input:lwt5eraqqvwh3pfw",1,0},{"enter success_proc",1,0},{"serial_no:80647cd8a040",1,0},{"mac 80:64:7c:d8:a0:40",1,0}};
static const PinWant c189_pins[] = {{0,120},{1,120},{10,120},{11,120},{14,840},{15,842},{16,840},{17,840}};
static const char *const c190_args[] = {"--only-uart","-chip","BK7238","--ble-core","--wifi"};
static const ExpectStr c190_exp[] = {{"[MEM DBG] heap init-------size:199672 addr:0x40f408---------",1,0},{"< TuyaOS V:3.11.16 BS:40.00_PT:2.3_LAN:3.5_CAD:1.0.5_CD:1.0.0 >",1,0},{"oem_t1_cutout_self_switch:1.1.0",1,0},{"IoT config ov_cur: 63000",1,0},{"0x36 0x9b 0xa1 0x34 0x5 0x67 0x4d 0x42 0xc4 0xd9 0x45 0xa7 0x5b 0x45 0x5 0x89",1,0},{"tuya_tls_rand_init ok!",1,0},{"mf_init succ",1,0},{"[switch] wifi led init ok",1,0},{"tuya_ele_hw_init success",1,0},{"current product ssid name:tuya_mdev_test1",1,0},{"mac  0:33:7a:68:84:e5",1,0}};
static const PinWant c190_pins[] = {{0,120},{1,120},{6,0},{8,0},{10,120},{11,120},{15,2},{17,60},{21,2}};
static const char *const c191_args[] = {"--only-uart","-chip","BK7238","--ble-core","--wifi"};
static const ExpectStr c191_exp[] = {{"[MEM DBG] heap init-------size:202792 addr:0x40e7d8---------",1,0},{"0x70 0xd0 0x25 0x72 0xa0 0x44 0xaf 0x3d 0xc 0x7d 0xd9 0xb0 0x85 0x32 0x16 0xf4",1,0},{"name:WIFI_THS_T1U:1.0.3",1,0},{"wifi soc init. pid:0apiv8a0c4gnk0km firmwarekey:NULL ver:1.0.3",1,0},{"gw_cntl->gw_if.product_key:0apiv8a0c4gnk0km, input:0apiv8a0c4gnk0km",1,0},{"Software timer for tdl_sensor_hub has started.",1,0},{"Status <last command>: 0.",1,0},{"---------- sensor data callback ----------",1,0},{"Temp: 26.0",1,0},{"Humi: 50.0",1,0}};
static const char *const c191_unexp[] = {"i2c wait ack timeout"};
static const PinWant c191_pins[] = {{0,120},{1,60},{10,120},{11,120}};
static const char *const c192_args[] = {"--only-uart","-chip","BK7238","--ble-core","--wifi"};
static const ExpectStr c192_exp[] = {{"prvHeapInit-start addr:0x415120, size:175840",1,0},{"name:oem_t1_switch:1.1.9",1,0},{"tuya_tls_rand_init ok!",1,0},{"mf_init succ",1,0},{"dctrl channel_num:4",1,0},{"mac 80:64:7c:e4:f3:ce",1,0},{"ssid:SmartLife-F3CE, key:, channel: 6",1,0},{"[BLE_ADV]   Service Data               50 fd 43 04 00 10 6b 65 79 66 78 75 ..  P.C...keyfxuj3d5wg53wx  uuid=0xFD50",1,0}};
static const PinWant c192_pins[] = {{0,120},{1,120},{10,60},{11,2}};

static const char *const c193_args[] = {"--only-uart","-chip","BK7238","--ble-core","--wifi","--tuyamcu","--tuyamcu-raw","--tuyamcu-pid","z3jngbyubvwgfrcv"};
static const ExpectStr c193_exp[] = {
  {"prvHeapInit-start addr:0x416278, size:171400",1,0},
  {"[Flash]id:0x0",1,0},
  {"don't config this flash, choose default config",1,0},
  {"bk_timer_init exit",1,0},
  {"sctrl_sta_ps_init",1,0},
  {"**********tuya_upgrade_main need upgrade? [255]**********",1,0},
  {"cset:0 0 0 0",1,0},
  {"tkl_ethernetif_init",1,0},
  {"[FUNC]rwnxl_init",1,0},
  {"IP Rev: 90d35d1",1,0}
};
static const PinWant c193_pins[] = {{0,120},{1,120},{10,120},{11,120},{14,840},{15,842},{16,840},{17,840}};

static const char *const c194_args[] = {"--only-uart","-chip","BK7238","--ble-core","--wifi"};
static const ExpectStr c194_exp[] = {
  {"prvHeapInit-start addr:0x4150d8, size:175912",1,0},
  {"[Flash]id:0x0",1,0},
  {"don't config this flash, choose default config",1,0},
  {"bk_timer_init exit",1,0},
  {"spi_init exit",1,0},
  {"sctrl_sta_ps_init",1,0},
  {"**********tuya_upgrade_main need upgrade? [255]**********",1,0},
  {"cset:0 0 0 0",1,0},
  {"[FUNC]rwnxl_init",1,0},
  {"tkl_ethernetif_init",1,0}
};
static const PinWant c194_pins[] = {{0,120},{1,120},{10,120},{11,120}};

static const char *const c195_args[] = {"--only-uart","-chip","BK7238","--ble-core","--wifi"};
static const ExpectStr c195_exp[] = {
  {"prvHeapInit-start addr:0x4150d8, size:175912",1,0},
  {"[Flash]id:0x0",1,0},
  {"don't config this flash, choose default config",1,0},
  {"bk_timer_init exit",1,0},
  {"spi_init exit",1,0},
  {"sctrl_sta_ps_init",1,0},
  {"**********tuya_upgrade_main need upgrade? [255]**********",1,0},
  {"cset:0 0 0 0",1,0},
  {"[FUNC]rwnxl_init",1,0},
  {"tkl_ethernetif_init",1,0}
};
static const PinWant c195_pins[] = {{0,120},{1,120},{10,120},{11,120}};

static const char *const c196_args[] = {"--only-uart","-key","0000000000000000376e7eff01000b63","-chip","BK7231N","--ble-core","--wifi"};
static const ExpectStr c196_exp[] = {
  {"[WIFI] device RX filter 0x7fffffde: monitor mode, every frame on air accepted (sniffing, as in EZ pairing)",1,0},
  {"uuid=0x180A",1,0}
};
static const PinWant c196_pins[] = {{0,120},{1,120},{10,120},{11,120},{12,120},{13,120},{24,2},{26,8}};

static const char *const c197_args[] = {"--only-uart","-key","0000000000000000b8bff72e01001b63","-chip","BK7231N","--ble-core","--wifi"};
static const ExpectStr c197_exp[] = {
  {"LibreTiny v1.0.2+sha.f7c28ee on cb2l",1,0},
  {"[I][logger:299]: Log initialized",1,0},
  {"[C][ota:477]: There have been 6 suspected unsuccessful boot attempts.",1,0},
  {"[D][lt.preferences:104]: Saving 1 preferences to flash...",1,0},
  {"[D][lt.preferences:132]: Saving 1 preferences to flash",1,0},
  {"[I][app:029]: Running through setup()...",1,0},
  {"[C][wifi:038]: Setting up WiFi...",1,0},
  {"[V][wifi_lt:039]: Enabling AP.",1,0}
};
static const PinWant c197_pins[] = {{0,120},{1,120},{10,120},{11,120},{12,120},{13,120}};

static const char *const c198_args[] = {"--only-uart","-key","00000000000000007cb5072101000b63","-chip","BK7231N","--ble-core","--wifi"};
static const ExpectStr c198_exp[] = {
  {"bk_misc_init_start_type 0 0",1,0},
  {"prvHeapInit-start addr:0x411948, size:124600",1,0},
  {"[Flash]id:0x0",1,0},
  {"don't config this flash, choose default config",1,0},
  {"sctrl_sta_ps_init",1,0},
  {"cset:0 0 0 0",1,0},
  {"[01-01 18:12:15 TUYA Info][lr:0xa5f11] mqc app init ...",1,0},
  {"[01-01 18:12:15 TUYA Debug][lr:0xaeea7] Thread:sys_timer Exec Start. Set to Running Stat",1,0},
  {"[01-01 18:12:15 TUYA Info][lr:0xaf01b] thread_create name:sys_timer,stackDepth:4096,totalstackDepth:4096,priority:5",1,0},
  {"[01-01 18:12:15 TUYA Info][lr:0xaf01b] thread_create name:cmmod,stackDepth:4096,totalstackDepth:8192,priority:4",1,0}
};
static const PinWant c198_pins[] = {{0,120},{1,120},{10,120},{11,120},{12,120},{13,120},{21,60}};

static const char *const c199_args[] = {"--only-uart","-key","00000000000000003fa7a2df01001b63","-chip","BK7231N","--ble-core","--wifi"};
static const ExpectStr c199_exp[] = {
  {"[WIFI] device RX filter 0x7fffffde: monitor mode, every frame on air accepted (sniffing, as in EZ pairing)",1,0},
  {"Cannot publish message to the topic",1,0}
};
static const PinWant c199_pins[] = {{0,120},{1,120},{6,60},{8,60},{9,2},{10,120},{11,120},{12,120},{13,120},{16,60},{26,72}};

static const char *const c200_args[] = {"--only-uart","-key","TUYA","--ble-core","--wifi"};
static const ExpectStr c200_exp[] = {
  {"prvHeapInit-start addr:0x41fa88, size:132472",1,0},
  {"[01-01 18:12:15 TUYA Info][mqc_app.c:175] mqc app init ...",1,0},
  {"[01-01 18:12:15 TUYA Info][sf_mqc_cb.c:42] register mqc app callback",1,0},
  {"[01-01 18:12:15 TUYA Debug][mqc_app.c:118] mq_pro:5 mqc_handler_cnt:1",1,0},
  {"[01-01 18:12:15 TUYA Debug][mqc_app.c:118] mq_pro:31 mqc_handler_cnt:2",1,0},
  {"[01-01 18:12:15 TUYA Debug][uni_thread.c:215] Thread:sys_timer Exec Start. Set to Running Status",1,0},
  {"[01-01 18:12:15 TUYA Debug][log_seq.c:732] read from uf. max:1 first:0 last:0",1,0},
  {"[01-01 18:12:15 TUYA Debug][svc_online_log.c:288] svc online log init success",1,0},
  {"[01-01 18:12:15 TUYA Err][tuya_ws_db.c:314] kvs_read fails gw_bi -1",1,0},
  {"[01-01 18:12:15 TUYA Err][ws_db_gw.c:111] gw base read fails -935",1,0}
};
static const PinWant c200_pins[] = {{0,120},{1,120},{10,120},{11,120},{12,120},{13,120}};

static const char *const c201_args[] = {"--only-uart","-key","TUYA","--ble-core","--wifi"};
static const ExpectStr c201_exp[] = {
  {"prvHeapInit-start addr:0x41fa88, size:132472",1,0},
  {"[01-01 18:12:15 TUYA Info][mqc_app.c:175] mqc app init ...",1,0},
  {"[01-01 18:12:15 TUYA Info][sf_mqc_cb.c:42] register mqc app callback",1,0},
  {"[01-01 18:12:15 TUYA Debug][mqc_app.c:118] mq_pro:5 mqc_handler_cnt:1",1,0},
  {"[01-01 18:12:15 TUYA Debug][mqc_app.c:118] mq_pro:31 mqc_handler_cnt:2",1,0},
  {"[01-01 18:12:15 TUYA Debug][uni_thread.c:215] Thread:sys_timer Exec Start. Set to Running Status",1,0},
  {"[01-01 18:12:15 TUYA Debug][log_seq.c:732] read from uf. max:2 first:0 last:1",1,0},
  {"[01-01 18:12:15 TUYA Debug][svc_online_log.c:288] svc online log init success",1,0},
  {"[01-01 18:12:15 TUYA Err][tuya_ws_db.c:314] kvs_read fails gw_bi -1",1,0},
  {"[01-01 18:12:15 TUYA Err][ws_db_gw.c:111] gw base read fails -935",1,0}
};
static const PinWant c201_pins[] = {{0,120},{1,120},{10,120},{11,120},{12,120},{13,120}};

static const char *const c202_args[] = {"--only-uart","-key","TUYA","--ble-core","--wifi"};
static const ExpectStr c202_exp[] = {
  {"prvHeapInit-start addr:0x41fa88, size:132472",1,0},
  {"[01-01 18:12:15 TUYA Info][mqc_app.c:175] mqc app init ...",1,0},
  {"[01-01 18:12:15 TUYA Debug][uni_thread.c:215] Thread:sys_timer Exec Start. Set to Running Status",1,0},
  {"[01-01 18:12:15 TUYA Info][sf_mqc_cb.c:42] register mqc app callback",1,0},
  {"[01-01 18:12:15 TUYA Debug][mqc_app.c:118] mq_pro:5 mqc_handler_cnt:1",1,0},
  {"[01-01 18:12:15 TUYA Debug][mqc_app.c:118] mq_pro:31 mqc_handler_cnt:2",1,0},
  {"[01-01 18:12:15 TUYA Debug][log_seq.c:732] read from uf. max:0 first:0 last:0",1,0},
  {"[01-01 18:12:15 TUYA Debug][svc_online_log.c:288] svc online log init success",1,0},
  {"[01-01 18:12:15 TUYA Err][tuya_ws_db.c:314] kvs_read fails gw_bi -1",1,0},
  {"[01-01 18:12:15 TUYA Err][ws_db_gw.c:111] gw base read fails -935",1,0}
};
static const PinWant c202_pins[] = {{0,120},{1,120},{10,120},{11,120},{12,120},{13,120}};

static const char *const c203_args[] = {"--only-uart","-key","TUYA","--ble-core","--wifi"};
static const ExpectStr c203_exp[] = {
  {"prvHeapInit-start addr:0x41e650, size:137648",1,0},
  {"[01-01 18:12:15 TUYA Debug][uni_thread.c:215] Thread:sys_timer Exec Start. Set to Running Status",1,0},
  {"[01-01 18:12:15 TUYA Debug][online_log_serv.c:303] read from uf. max:0 first:0 last:0",1,0},
  {"[01-01 18:12:15 TUYA Debug][online_log_serv.c:540] log serv init success",1,0},
  {"[01-01 18:12:15 TUYA Debug][tuya_device.c:192] < TUYA IOT SDK V:2.0.0 BS:30.06_PT:2.2_LAN:3.3_CAD:1.0.2_CD:1.0.0 >",1,0},
  {"< tuya_iot_lib BUILD AT:2018_12_05_17_03_30 BY tuya_iot_team AT 8710_2M >",1,0},
  {"IOT DEFS < WIFI_GW:1 DEBUG:1 KV_FILE:0 SHUTDOWN_MODE:0 LITTLE_END:1 TLS_MODE:2 ENABLE_LOCAL_LINKAGE:0 ENABLE_CLOUD_OPERATION:0 ENABLE_SUBDEVICE:0 ENABLE_ENGINEER_TO_NORMAL:0 OPERATING_SYSTEM:2 ENABLE_SYS_RPC:0 TY_SECURITY_CHIP:0 RELIABLE_TRANSFER:RELIABLE_TRANSFER ENABLE_LAN_ENCRYPTION:1 ENABLE_SIGMESH:0 >",1,0},
  {"[01-01 18:12:15 TUYA Debug][tuya_device.c:193] oem_bk7231s_rnd_switch:1.0.7",1,0},
  {"[01-01 18:12:15 TUYA Notice][tuya_device.c:194] firmware compiled at Jun 10 2020 15:59:57",1,0}
};
static const PinWant c203_pins[] = {{0,120},{1,120},{10,120},{11,120},{12,120},{13,120}};

static const char *const c204_args[] = {"--only-uart","-key","TUYA","--ble-core","--wifi"};
static const ExpectStr c204_exp[] = {
  {"prvHeapInit-start addr:0x4203d0, size:130096",1,0},
  {"[01-01 18:12:15 TUYA Info][mqc_app.c:175] mqc app init ...",1,0},
  {"[01-01 18:12:15 TUYA Debug][uni_thread.c:215] Thread:sys_timer Exec Start. Set to Running Status",1,0},
  {"[01-01 18:12:15 TUYA Info][sf_mqc_cb.c:42] register mqc app callback",1,0},
  {"[01-01 18:12:15 TUYA Debug][mqc_app.c:118] mq_pro:5 mqc_handler_cnt:1",1,0},
  {"[01-01 18:12:15 TUYA Debug][mqc_app.c:118] mq_pro:31 mqc_handler_cnt:2",1,0},
  {"[01-01 18:12:15 TUYA Debug][log_seq.c:732] read from uf. max:0 first:0 last:0",1,0},
  {"[01-01 18:12:15 TUYA Debug][svc_online_log.c:288] svc online log init success",1,0},
  {"[01-01 18:12:15 TUYA Err][tuya_ws_db.c:314] kvs_read fails gw_bi -1",1,0},
  {"[01-01 18:12:15 TUYA Err][ws_db_gw.c:111] gw base read fails -935",1,0}
};
static const PinWant c204_pins[] = {{0,120},{1,120},{10,120},{11,120},{12,120},{13,120}};

static const char *const c205_args[] = {"--only-uart","-key","TUYA","--ble-core","--wifi"};
static const ExpectStr c205_exp[] = {
  {"prvHeapInit-start addr:0x4203f8, size:130056",1,0},
  {"[01-01 18:12:15 TUYA Info][mqc_app.c:175] mqc app init ...",1,0},
  {"[01-01 18:12:15 TUYA Info][sf_mqc_cb.c:42] register mqc app callback",1,0},
  {"[01-01 18:12:15 TUYA Debug][mqc_app.c:118] mq_pro:5 mqc_handler_cnt:1",1,0},
  {"[01-01 18:12:15 TUYA Debug][mqc_app.c:118] mq_pro:31 mqc_handler_cnt:2",1,0},
  {"[01-01 18:12:15 TUYA Debug][uni_thread.c:215] Thread:sys_timer Exec Start. Set to Running Status",1,0},
  {"[01-01 18:12:15 TUYA Debug][log_seq.c:732] read from uf. max:0 first:0 last:0",1,0},
  {"[01-01 18:12:15 TUYA Debug][svc_online_log.c:288] svc online log init success",1,0},
  {"[01-01 18:12:15 TUYA Err][tuya_ws_db.c:314] kvs_read fails gw_bi -1",1,0},
  {"[01-01 18:12:15 TUYA Err][ws_db_gw.c:111] gw base read fails -935",1,0}
};
static const PinWant c205_pins[] = {{0,120},{1,120},{10,120},{11,120},{12,120},{13,120}};

static const char *const c206_args[] = {"--only-uart","-key","TUYA","--ble-core","--wifi"};
static const ExpectStr c206_exp[] = {
  {"prvHeapInit-start addr:0x41f1d8, size:134696",1,0},
  {"[01-01 18:12:15 TUYA Debug][uni_thread.c:215] Thread:sys_timer Exec Start. Set to Running Status",1,0},
  {"[01-01 18:12:15 TUYA Debug][online_log_serv.c:303] read from uf. max:0 first:0 last:0",1,0},
  {"[01-01 18:12:15 TUYA Debug][online_log_serv.c:540] log serv init success",1,0},
  {"[01-01 18:12:15 TUYA Notice][light_system.c:1425] go to pre device!",1,0}
};
static const PinWant c206_pins[] = {{0,120},{1,120},{10,120},{11,120},{12,120},{13,120}};

static const char *const c207_args[] = {"--only-uart","-key","TUYA","--ble-core","--wifi"};
static const ExpectStr c207_exp[] = {
  {"prvHeapInit-start addr:0x41f380, size:134272",1,0},
  {"[01-01 18:12:15 TUYA Debug][uni_thread.c:215] Thread:sys_timer Exec Start. Set to Running Status",1,0},
  {"[01-01 18:12:15 TUYA Debug][online_log_serv.c:303] read from uf. max:0 first:0 last:0",1,0},
  {"[01-01 18:12:15 TUYA Debug][online_log_serv.c:540] log serv init success",1,0},
  {"[01-01 18:12:15 TUYA Notice][light_system.c:1667] go to pre device!",1,0}
};
static const PinWant c207_pins[] = {{0,120},{1,120},{10,120},{11,120},{12,120},{13,120}};

static const char *const c208_args[] = {"--only-uart","-key","TUYA","--ble-core","--wifi"};
static const ExpectStr c208_exp[] = {
  {"prvHeapInit-start addr:0x41f7b8, size:133192",1,0},
  {"[01-01 18:12:15 TUYA Info][mqc_app.c:175] mqc app init ...",1,0},
  {"[01-01 18:12:15 TUYA Debug][uni_thread.c:215] Thread:sys_timer Exec Start. Set to Running Status",1,0},
  {"[01-01 18:12:15 TUYA Info][sf_mqc_cb.c:42] register mqc app callback",1,0},
  {"[01-01 18:12:15 TUYA Debug][mqc_app.c:118] mq_pro:5 mqc_handler_cnt:1",1,0},
  {"[01-01 18:12:15 TUYA Debug][mqc_app.c:118] mq_pro:31 mqc_handler_cnt:2",1,0},
  {"[01-01 18:12:15 TUYA Debug][log_seq.c:732] read from uf. max:1 first:0 last:0",1,0},
  {"[01-01 18:12:15 TUYA Debug][svc_online_log.c:288] svc online log init success",1,0},
  {"[01-01 18:12:15 TUYA Err][tuya_ws_db.c:314] kvs_read fails gw_bi -1",1,0},
  {"[01-01 18:12:15 TUYA Err][ws_db_gw.c:111] gw base read fails -935",1,0}
};
static const PinWant c208_pins[] = {{0,120},{1,120},{10,120},{11,120},{12,120},{13,120}};

static const char *const c209_args[] = {"--only-uart","-key","TUYA","--ble-core","--wifi"};
static const ExpectStr c209_exp[] = {
  {"prvHeapInit-start addr:0x41ef98, size:135272",1,0},
  {"[01-01 18:12:15 TUYA Debug][uni_thread.c:215] Thread:sys_timer Exec Start. Set to Running Status",1,0},
  {"[01-01 18:12:15 TUYA Debug][online_log_serv.c:303] read from uf. max:1 first:0 last:0",1,0},
  {"[01-01 18:12:15 TUYA Debug][online_log_serv.c:540] log serv init success",1,0},
  {"[01-01 18:12:15 TUYA Notice][light_system.c:1034] go to pre device!",1,0}
};
static const PinWant c209_pins[] = {{0,120},{1,120},{10,120},{11,120},{12,120},{13,120}};

static const char *const c210_args[] = {"--only-uart","-key","TUYA","--ble-core","--wifi"};
static const ExpectStr c210_exp[] = {
  {"prvHeapInit-start addr:0x426e98, size:102760",1,0},
  {"[01-01 18:12:15 TUYA Debug][uni_thread.c:215] Thread:sys_timer Exec Start. Set to Running Status",1,0},
  {"[01-01 18:12:15 TUYA Debug][online_log_serv.c:303] read from uf. max:0 first:0 last:0",1,0},
  {"[01-01 18:12:15 TUYA Debug][online_log_serv.c:540] log serv init success",1,0},
  {"[01-01 18:12:15 TUYA Notice][light_system.c:1370] go to pre device!",1,0}
};
static const PinWant c210_pins[] = {{0,120},{1,120},{10,120},{11,120},{12,120},{13,120}};

static const char *const c211_args[] = {"--only-uart","-key","TUYA","--ble-core","--wifi"};
static const ExpectStr c211_exp[] = {
  {"prvHeapInit-start addr:0x41f700, size:133376",1,0},
  {"[01-01 18:12:15 TUYA Info][mqc_app.c:175] mqc app init ...",1,0},
  {"[01-01 18:12:15 TUYA Info][sf_mqc_cb.c:42] register mqc app callback",1,0},
  {"[01-01 18:12:15 TUYA Debug][mqc_app.c:118] mq_pro:5 mqc_handler_cnt:1",1,0},
  {"[01-01 18:12:15 TUYA Debug][mqc_app.c:118] mq_pro:31 mqc_handler_cnt:2",1,0},
  {"[01-01 18:12:15 TUYA Debug][uni_thread.c:215] Thread:sys_timer Exec Start. Set to Running Status",1,0},
  {"[01-01 18:12:15 TUYA Debug][log_seq.c:732] read from uf. max:0 first:0 last:0",1,0},
  {"[01-01 18:12:15 TUYA Debug][svc_online_log.c:288] svc online log init success",1,0},
  {"[01-01 18:12:15 TUYA Err][tuya_ws_db.c:314] kvs_read fails gw_bi -1",1,0},
  {"[01-01 18:12:15 TUYA Err][ws_db_gw.c:111] gw base read fails -935",1,0}
};
static const PinWant c211_pins[] = {{0,120},{1,120},{10,120},{11,120},{12,120},{13,120}};

static const char *const c212_args[] = {"--only-uart","-key","TUYA","--ble-core","--wifi"};
static const ExpectStr c212_exp[] = {
  {"prvHeapInit-start addr:0x421ff8, size:122888",1,0},
  {"[01-01 18:12:15 TUYA Info][mqc_app.c:175] mqc app init ...",1,0},
  {"[01-01 18:12:15 TUYA Debug][uni_thread.c:215] Thread:sys_timer Exec Start. Set to Running Status",1,0},
  {"[01-01 18:12:15 TUYA Info][sf_mqc_cb.c:42] register mqc app callback",1,0},
  {"[01-01 18:12:15 TUYA Debug][mqc_app.c:118] mq_pro:5 mqc_handler_cnt:1",1,0},
  {"[01-01 18:12:15 TUYA Debug][mqc_app.c:118] mq_pro:31 mqc_handler_cnt:2",1,0},
  {"[01-01 18:12:15 TUYA Debug][log_seq.c:732] read from uf. max:1 first:0 last:0",1,0},
  {"[01-01 18:12:15 TUYA Debug][svc_online_log.c:288] svc online log init success",1,0},
  {"[01-01 18:12:15 TUYA Err][tuya_ws_db.c:314] kvs_read fails gw_bi -1",1,0},
  {"[01-01 18:12:15 TUYA Err][ws_db_gw.c:111] gw base read fails -935",1,0}
};
static const PinWant c212_pins[] = {{0,120},{1,120},{10,120},{11,120},{12,120},{13,120}};

static const char *const c213_args[] = {"--only-uart","-key","TUYA","-chip","BK7231N","--ble-core","--wifi"};
static const ExpectStr c213_exp[] = {
  {"bk_misc_init_start_type 0 0",1,0},
  {"prvHeapInit-start addr:0x410958, size:128680",1,0},
  {"[Flash]id:0x0",1,0},
  {"choose default config",1,0},
  {"[01-01 18:12:15 TUYA I][lr:0xa081d] mqc app init ...",1,0},
  {"[01-01 18:12:15 TUYA I][lr:0xab803] thread_create name:sys_timer,stackDepth:4096,totalstackDepth:4096,priority:5",1,0},
  {"[01-01 18:12:15 TUYA D][lr:0xab68f] Thread:sys_timer Exec Start. Set to Running Stat",1,0},
  {"[01-01 18:12:15 TUYA I][lr:0xab803] thread_create name:cmmod,stackDepth:4096,totalstackDepth:8192,priority:4",1,0},
  {"[01-01 18:12:15 TUYA D][lr:0xa0737] mq_pro:5 cnt:1",1,0},
  {"[01-01 18:12:15 TUYA D][lr:0xa0737] mq_pro:31 cnt:2",1,0}
};
static const PinWant c213_pins[] = {{0,120},{1,120},{10,120},{11,120},{12,120},{13,120},{21,60}};

static const char *const c214_args[] = {"--only-uart","-key","TUYA","-chip","BK7231N","--ble-core","--wifi"};
static const ExpectStr c214_exp[] = {
  {"bk_misc_init_start_type 0 0",1,0},
  {"prvHeapInit-start addr:0x4111b0, size:126544",1,0},
  {"[Flash]id:0x0",1,0},
  {"choose default config",1,0},
  {"[01-01 18:12:15 TUYA I][lr:0xa584d] mqc app init ...",1,0},
  {"[01-01 18:12:15 TUYA I][lr:0xb087b] thread_create name:sys_timer,stackDepth:4096,totalstackDepth:4096,priority:5",1,0},
  {"[01-01 18:12:15 TUYA D][lr:0xb0707] Thread:sys_timer Exec Start. Set to Running Stat",1,0},
  {"[01-01 18:12:15 TUYA I][lr:0xb087b] thread_create name:cmmod,stackDepth:4096,totalstackDepth:8192,priority:4",1,0},
  {"[01-01 18:12:15 TUYA D][lr:0xa5767] mq_pro:5 cnt:1",1,0},
  {"[01-01 18:12:15 TUYA D][lr:0xa5767] mq_pro:31 cnt:2",1,0}
};
static const PinWant c214_pins[] = {{0,120},{1,120},{10,120},{11,120},{12,120},{13,120},{21,60}};

static const char *const c215_args[] = {"--only-uart","-key","TUYA","-chip","BK7231N","--ble-core","--wifi"};
static const ExpectStr c215_exp[] = {
  {"bk_misc_init_start_type 0 0",1,0},
  {"prvHeapInit-start addr:0x4112e8, size:126232",1,0},
  {"[Flash]id:0x0",1,0},
  {"don't config this flash, choose default config",1,0},
  {"sctrl_sta_ps_init",1,0},
  {"cset:0 0 0 0",1,0},
  {"[01-01 18:12:15 TUYA I][lr:0xbb561] mqc app init ...",1,0},
  {"[01-01 18:12:15 TUYA I][lr:0xb8b33] thread_create name:sys_timer,stackDepth:4096,totalstackDepth:4096,priority:5",1,0},
  {"[01-01 18:12:15 TUYA D][lr:0xb89bf] Thread:sys_timer Exec Start. Set to Running Stat",1,0},
  {"[01-01 18:12:15 TUYA I][lr:0xb8b33] thread_create name:cmmod,stackDepth:4096,totalstackDepth:8192,priority:4",1,0}
};
static const PinWant c215_pins[] = {{0,120},{1,120},{10,120},{11,120},{12,120},{13,120},{21,60}};

static const char *const c216_args[] = {"--only-uart","-key","TUYA","-chip","BK7231N","--ble-core","--wifi"};
static const ExpectStr c216_exp[] = {
  {"bk_misc_init_start_type 0 0",1,0},
  {"enter driver_init !",1,0},
  {"[MEM DBG] heap init-------size:148744 addr:0x40baf8---------",1,0},
  {"[Flash]id:0x0",1,0},
  {"choose default config",1,0},
  {"out driver_init !",1,0},
  {"tkl_ethernetif_init",1,0},
  {"tkl_ethernetif_output: invalid vif: 255!",1,0}
};
static const PinWant c216_pins[] = {{0,120},{1,120},{10,120},{11,120},{12,120},{13,120},{21,60}};

static const char *const c217_args[] = {"--only-uart","-key","TUYA","-chip","BK7231N","--ble-core","--wifi"};
static const ExpectStr c217_exp[] = {
  {"bk_misc_init_start_type 0 0",1,0},
  {"prvHeapInit-start addr:0x411048, size:126904",1,0},
  {"[Flash]id:0x0",1,0},
  {"don't config this flash, choose default config",1,0},
  {"sctrl_sta_ps_init",1,0},
  {"cset:0 0 0 0",1,0},
  {"[01-01 18:12:15 TUYA I][lr:0xb7e0d] mqc app init ...",1,0},
  {"[01-01 18:12:15 TUYA I][lr:0xb53cf] thread_create name:sys_timer,stackDepth:4096,totalstackDepth:4096,priority:5",1,0},
  {"[01-01 18:12:15 TUYA D][lr:0xb525b] Thread:sys_timer Exec Start. Set to Running Stat",1,0},
  {"[01-01 18:12:15 TUYA I][lr:0xb53cf] thread_create name:cmmod,stackDepth:4096,totalstackDepth:8192,priority:4",1,0}
};
static const PinWant c217_pins[] = {{0,120},{1,120},{10,120},{11,120},{12,120},{13,120},{21,60}};

static const char *const c218_args[] = {"--only-uart","-key","TUYA","-chip","BK7231N","--ble-core","--wifi"};
static const ExpectStr c218_exp[] = {
  {"bk_misc_init_start_type 0 0",1,0},
  {"prvHeapInit-start addr:0x410958, size:128680",1,0},
  {"[Flash]id:0x0",1,0},
  {"choose default config",1,0},
  {"[01-01 18:12:15 TUYA I][lr:0xa081d] mqc app init ...",1,0},
  {"[01-01 18:12:15 TUYA I][lr:0xab803] thread_create name:sys_timer,stackDepth:4096,totalstackDepth:4096,priority:5",1,0},
  {"[01-01 18:12:15 TUYA D][lr:0xab68f] Thread:sys_timer Exec Start. Set to Running Stat",1,0},
  {"[01-01 18:12:15 TUYA I][lr:0xab803] thread_create name:cmmod,stackDepth:4096,totalstackDepth:8192,priority:4",1,0},
  {"[01-01 18:12:15 TUYA D][lr:0xa0737] mq_pro:5 cnt:1",1,0},
  {"[01-01 18:12:15 TUYA D][lr:0xa0737] mq_pro:31 cnt:2",1,0}
};
static const PinWant c218_pins[] = {{0,120},{1,120},{10,120},{11,120},{12,120},{13,120},{21,60}};

static const char *const c219_args[] = {"--only-uart","-key","TUYA","-chip","BK7231N","--ble-core","--wifi"};
static const ExpectStr c219_exp[] = {
  {"bk_misc_init_start_type 0 0",1,0},
  {"prvHeapInit-start addr:0x411a30, size:124368",1,0},
  {"[Flash]id:0x0",1,0},
  {"don't config this flash, choose default config",1,0},
  {"sctrl_sta_ps_init",1,0},
  {"cset:0 0 0 0",1,0},
  {"[01-01 18:12:15 TUYA Info][lr:0xa79d1] mqc app init ...",1,0},
  {"[01-01 18:12:15 TUYA Info][lr:0xb0b1f] thread_create name:sys_timer,stackDepth:4096,totalstackDepth:4096,priority:5",1,0},
  {"[01-01 18:12:15 TUYA Debug][lr:0xb09ab] Thread:sys_timer Exec Start. Set to Running Stat",1,0},
  {"[01-01 18:12:15 TUYA Info][lr:0xb0b1f] thread_create name:cmmod,stackDepth:4096,totalstackDepth:8192,priority:4",1,0}
};
static const PinWant c219_pins[] = {{0,120},{1,120},{10,120},{11,120},{12,120},{13,120},{21,60}};

static const char *const c220_args[] = {"--only-uart","-key","TUYA","-chip","BK7231N","--ble-core","--wifi"};
static const ExpectStr c220_exp[] = {
  {"bk_misc_init_start_type 0 0",1,0},
  {"prvHeapInit-start addr:0x412008, size:122872",1,0},
  {"[Flash]id:0x0",1,0},
  {"don't config this flash, choose default config",1,0},
  {"sctrl_sta_ps_init",1,0},
  {"cset:0 0 0 0",1,0},
  {"[01-01 18:12:15 TUYA Info][lr:0xbcb09] mqc app init ...",1,0},
  {"[01-01 18:12:15 TUYA Info][lr:0x9ef0b] thread_create name:sys_timer,stackDepth:4096,totalstackDepth:4096,priority:5",1,0},
  {"[01-01 18:12:15 TUYA Debug][lr:0x9ed97] Thread:sys_timer Exec Start. Set to Running Stat",1,0},
  {"[01-01 18:12:15 TUYA Info][lr:0x9ef0b] thread_create name:cmmod,stackDepth:4096,totalstackDepth:8192,priority:4",1,0}
};
static const PinWant c220_pins[] = {{0,120},{1,120},{10,120},{11,120},{12,120},{13,120},{21,60}};

static const char *const c221_args[] = {"--only-uart","-key","TUYA","-chip","BK7231N","--ble-core","--wifi"};
static const ExpectStr c221_exp[] = {
  {"bk_misc_init_start_type 0 0",1,0},
  {"prvHeapInit-start addr:0x411988, size:124536",1,0},
  {"[Flash]id:0x0",1,0},
  {"don't config this flash, choose default config",1,0},
  {"sctrl_sta_ps_init",1,0},
  {"cset:0 0 0 0",1,0},
  {"[01-01 18:12:15 TUYA Info][lr:0xa5e25] mqc app init ...",1,0},
  {"[01-01 18:12:15 TUYA Info][lr:0xaef6b] thread_create name:sys_timer,stackDepth:4096,totalstackDepth:4096,priority:5",1,0},
  {"[01-01 18:12:15 TUYA Info][lr:0xaef6b] thread_create name:cmmod,stackDepth:4096,totalstackDepth:8192,priority:4",1,0},
  {"[01-01 18:12:15 TUYA Debug][lr:0xa5dd3] mq_pro:5 cnt:1",1,0}
};
static const PinWant c221_pins[] = {{0,120},{1,120},{10,120},{11,120},{12,120},{13,120},{21,60}};

static const char *const c222_args[] = {"--only-uart","-key","TUYA","-chip","BK7231N","--ble-core","--wifi"};
static const ExpectStr c222_exp[] = {
  {"bk_misc_init_start_type 0 0",1,0},
  {"enter driver_init !",1,0},
  {"[MEM DBG] heap init-------size:155648 addr:0x40a000---------",1,0},
  {"[Flash]id:0x0",1,0},
  {"choose default config",1,0},
  {"out driver_init !",1,0},
  {"tkl_ethernetif_init",1,0},
  {"tkl_ethernetif_output: invalid vif: 255!",1,0}
};
static const PinWant c222_pins[] = {{0,120},{1,120},{10,120},{11,120},{12,120},{13,120},{21,60}};

static const char *const c223_args[] = {"--only-uart","-key","TUYA","-chip","BK7231N","--ble-core","--wifi"};
static const ExpectStr c223_exp[] = {
  {"bk_misc_init_start_type 0 0",1,0},
  {"prvHeapInit-start addr:0x411988, size:124536",1,0},
  {"[Flash]id:0x0",1,0},
  {"don't config this flash, choose default config",1,0},
  {"sctrl_sta_ps_init",1,0},
  {"cset:0 0 0 0",1,0},
  {"[01-01 18:12:15 TUYA Info][lr:0xa5e25] mqc app init ...",1,0},
  {"[01-01 18:12:15 TUYA Info][lr:0xaef6b] thread_create name:sys_timer,stackDepth:4096,totalstackDepth:4096,priority:5",1,0},
  {"[01-01 18:12:15 TUYA Info][lr:0xaef6b] thread_create name:cmmod,stackDepth:4096,totalstackDepth:8192,priority:4",1,0},
  {"[01-01 18:12:15 TUYA Debug][lr:0xa5dd3] mq_pro:5 cnt:1",1,0}
};
static const PinWant c223_pins[] = {{0,120},{1,120},{10,120},{11,120},{12,120},{13,120},{21,60}};

static const char *const c224_args[] = {"--only-uart","-key","TUYA","-chip","BK7231N","--ble-core","--wifi"};
static const ExpectStr c224_exp[] = {
  {"bk_misc_init_start_type 0 0",1,0},
  {"prvHeapInit-start addr:0x4126b0, size:121168",1,0},
  {"[Flash]id:0x0",1,0},
  {"choose default config",1,0},
  {"irda_init",1,0},
  {"[01-01 18:12:15 TUYA I][lr:0xa01a1] mqc app init ...",1,0},
  {"[01-01 18:12:15 TUYA I][lr:0xab0c3] thread_create name:sys_timer,stackDepth:4096,totalstackDepth:4096,priority:5",1,0},
  {"[01-01 18:12:15 TUYA D][lr:0xaaf4f] Thread:sys_timer Exec Start. Set to Running Stat",1,0},
  {"[01-01 18:12:15 TUYA I][lr:0xab0c3] thread_create name:cmmod,stackDepth:4096,totalstackDepth:8192,priority:4",1,0},
  {"[01-01 18:12:15 TUYA D][lr:0xa00bb] mq_pro:5 cnt:1",1,0}
};
static const PinWant c224_pins[] = {{0,120},{1,120},{10,120},{11,120},{12,120},{13,120},{21,60}};

static const char *const c225_args[] = {"--only-uart","-key","TUYA","-chip","BK7231N","--ble-core","--wifi"};
static const ExpectStr c225_exp[] = {
  {"bk_misc_init_start_type 0 0",1,0},
  {"[MEM DBG] heap init-------size:157776 addr:0x4097b0---------",1,0},
  {"[Flash]id:0x0",1,0},
  {"choose default config",1,0},
  {"tkl_ethernetif_init",1,0},
  {"tkl_ethernetif_output: invalid vif: 255!",1,0}
};
static const PinWant c225_pins[] = {{0,120},{1,120},{10,120},{11,120},{12,120},{13,120},{21,60}};

static const char *const c226_args[] = {"--only-uart","-key","TUYA","-chip","BK7231N","--ble-core","--wifi"};
static const ExpectStr c226_exp[] = {
  {"bk_misc_init_start_type 0 0",1,0},
  {"prvHeapInit-start addr:0x410a88, size:128376",1,0},
  {"[Flash]id:0x0",1,0},
  {"choose default config",1,0},
  {"[01-01 18:12:15 TUYA I][lr:0x9d87d] mqc app init ...",1,0},
  {"[01-01 18:12:15 TUYA I][lr:0xa884b] thread_create name:sys_timer,stackDepth:4096,totalstackDepth:4096,priority:5",1,0},
  {"[01-01 18:12:15 TUYA D][lr:0xa86d7] Thread:sys_timer Exec Start. Set to Running Stat",1,0},
  {"[01-01 18:12:15 TUYA I][lr:0xa884b] thread_create name:cmmod,stackDepth:4096,totalstackDepth:8192,priority:4",1,0},
  {"[01-01 18:12:15 TUYA D][lr:0x9d797] mq_pro:5 cnt:1",1,0},
  {"[01-01 18:12:15 TUYA D][lr:0x9d797] mq_pro:31 cnt:2",1,0}
};
static const PinWant c226_pins[] = {{0,120},{1,120},{10,120},{11,120},{12,120},{13,120},{21,60}};

static const char *const c227_args[] = {"--only-uart","-key","TUYA","-chip","BK7231N","--ble-core","--wifi"};
static const ExpectStr c227_exp[] = {
  {"bk_misc_init_start_type 0 0",1,0},
  {"prvHeapInit-start addr:0x411fa0, size:122976",1,0},
  {"[Flash]id:0x0",1,0},
  {"don't config this flash, choose default config",1,0},
  {"sctrl_sta_ps_init",1,0},
  {"cset:0 0 0 0",1,0},
  {"[01-01 18:12:15 TUYA Info][lr:0x9fc21] mqc app init ...",1,0},
  {"[01-01 18:12:15 TUYA Info][lr:0xb2e6b] thread_create name:sys_timer,stackDepth:4096,totalstackDepth:4096,priority:5",1,0},
  {"[01-01 18:12:15 TUYA Info][lr:0xb2e6b] thread_create name:cmmod,stackDepth:4096,totalstackDepth:8192,priority:4",1,0},
  {"[01-01 18:12:15 TUYA Debug][lr:0x9fbcf] mq_pro:5 cnt:1",1,0}
};
static const PinWant c227_pins[] = {{0,120},{1,120},{10,120},{11,120},{12,120},{13,120},{21,60}};

static const char *const c228_args[] = {"--only-uart","-key","TUYA","-chip","BK7231N","--ble-core","--wifi"};
static const ExpectStr c228_exp[] = {
  {"bk_misc_init_start_type 0 0",1,0},
  {"prvHeapInit-start addr:0x411fa0, size:122976",1,0},
  {"[Flash]id:0x0",1,0},
  {"don't config this flash, choose default config",1,0},
  {"sctrl_sta_ps_init",1,0},
  {"cset:0 0 0 0",1,0},
  {"[01-01 18:12:15 TUYA Info][lr:0x9fc21] mqc app init ...",1,0},
  {"[01-01 18:12:15 TUYA Info][lr:0xb2e6b] thread_create name:sys_timer,stackDepth:4096,totalstackDepth:4096,priority:5",1,0},
  {"[01-01 18:12:15 TUYA Info][lr:0xb2e6b] thread_create name:cmmod,stackDepth:4096,totalstackDepth:8192,priority:4",1,0},
  {"[01-01 18:12:15 TUYA Debug][lr:0x9fbcf] mq_pro:5 cnt:1",1,0}
};
static const PinWant c228_pins[] = {{0,120},{1,120},{10,120},{11,120},{12,120},{13,120},{21,60}};

static const char *const c229_args[] = {"--only-uart","-key","TUYA","-chip","BK7231N","--ble-core","--wifi"};
static const ExpectStr c229_exp[] = {
  {"bk_misc_init_start_type 0 0",1,0},
  {"enter driver_init !",1,0},
  {"[MEM DBG] heap init-------size:155624 addr:0x40a018---------",1,0},
  {"[Flash]id:0x0",1,0},
  {"choose default config",1,0},
  {"out driver_init !",1,0},
  {"tkl_ethernetif_init",1,0},
  {"tkl_ethernetif_output: invalid vif: 255!",1,0}
};
static const PinWant c229_pins[] = {{0,120},{1,120},{10,120},{11,120},{12,120},{13,120},{21,60}};

static const char *const c230_args[] = {"--only-uart","-key","TUYA","-chip","BK7231N","--ble-core","--wifi"};
static const ExpectStr c230_exp[] = {
  {"bk_misc_init_start_type 0 0",1,0},
  {"enter driver_init !",1,0},
  {"[MEM DBG] heap init-------size:155648 addr:0x40a000---------",1,0},
  {"[Flash]id:0x0",1,0},
  {"choose default config",1,0},
  {"out driver_init !",1,0},
  {"tkl_ethernetif_init",1,0},
  {"tkl_ethernetif_output: invalid vif: 255!",1,0}
};
static const PinWant c230_pins[] = {{0,120},{1,120},{10,120},{11,120},{12,120},{13,120},{21,60}};

static const char *const c231_args[] = {"--only-uart","-key","TUYA","-chip","BK7231N","--ble-core","--wifi"};
static const ExpectStr c231_exp[] = {
  {"bk_misc_init_start_type 0 0",1,0},
  {"prvHeapInit-start addr:0x411a00, size:124416",1,0},
  {"[Flash]id:0x0",1,0},
  {"choose default config",1,0}
};
static const PinWant c231_pins[] = {{0,120},{1,120},{10,120},{11,120},{12,120},{13,120},{21,60}};

static const char *const c232_args[] = {"--only-uart","-key","TUYA","-chip","BK7231N","--ble-core","--wifi"};
static const ExpectStr c232_exp[] = {
  {"bk_misc_init_start_type 0 0",1,0},
  {"prvHeapInit-start addr:0x4111b8, size:126536",1,0},
  {"[Flash]id:0x0",1,0},
  {"choose default config",1,0},
  {"[01-01 18:12:15 TUYA I][lr:0xa5919] mqc app init ...",1,0},
  {"[01-01 18:12:15 TUYA I][lr:0xb0947] thread_create name:sys_timer,stackDepth:4096,totalstackDepth:4096,priority:5",1,0},
  {"[01-01 18:12:15 TUYA I][lr:0xb0947] thread_create name:cmmod,stackDepth:4096,totalstackDepth:8192,priority:4",1,0},
  {"[01-01 18:12:15 TUYA D][lr:0xb07d3] Thread:sys_timer Exec Start. Set to Running Stat",1,0},
  {"[01-01 18:12:15 TUYA D][lr:0xa5833] mq_pro:5 cnt:1",1,0},
  {"[01-01 18:12:15 TUYA D][lr:0xa5833] mq_pro:31 cnt:2",1,0}
};
static const PinWant c232_pins[] = {{0,120},{1,120},{10,120},{11,120},{12,120},{13,120},{21,60}};

static const char *const c233_args[] = {"--only-uart","-chip","BK7238","--wifi"};
static const ExpectStr c233_exp[] = {
  {"[WIFI] device RX filter 0x7fffffde: monitor mode",1,0}
};

static const char *const c234_args[] = {"--only-uart","-chip","BK7238","--wifi"};
static const ExpectStr c234_exp[] = {
  {"prvHeapInit-start addr",1,0},
  {"[Flash]id:0x0",1,0},
  {"bk_timer_init exit",1,0},
  {"sctrl_sta_ps_init",1,0}
};

static const char *const c235_args[] = {"--only-uart","-chip","BK7238","--wifi","--tuyamcu"};
static const ExpectStr c235_exp[] = {
  {"prvHeapInit-start addr",1,0},
  {"bk_timer_init exit",1,0},
  {"sctrl_sta_ps_init",1,0},
  {"tuya_upgrade_main need upgrade",1,0}
};

static const char *const c236_args[] = {"--only-uart","-chip","BK7238","--wifi","--tuyamcu"};
static const ExpectStr c236_exp[] = {
  {"prvHeapInit-start addr",1,0},
  {"bk_timer_init exit",1,0},
  {"sctrl_sta_ps_init",1,0},
  {"tuya_upgrade_main need upgrade",1,0}
};

static const char *const c237_args[] = {"--only-uart","-chip","BK7238","--wifi","--tuyamcu"};
static const ExpectStr c237_exp[] = {
  {"prvHeapInit-start addr",1,0},
  {"bk_timer_init exit",1,0},
  {"sctrl_sta_ps_init",1,0}
};

static const char *const c238_args[] = {"--only-uart","-chip","BK7238","--wifi"};
static const ExpectStr c238_exp[] = {
  {"prvHeapInit-start addr",1,0},
  {"bk_timer_init exit",1,0},
  {"sctrl_sta_ps_init",1,0}
};

static const char *const c239_args[] = {"--only-uart","-chip","BK7238","--wifi"};
static const ExpectStr c239_exp[] = {
  {"prvHeapInit-start addr",1,0},
  {"bk_timer_init exit",1,0},
  {"sctrl_sta_ps_init",1,0}
};

static const char *const c240_args[] = {"--only-uart","-chip","BK7238","--wifi"};
static const ExpectStr c240_exp[] = {
  {"prvHeapInit-start addr",1,0},
  {"bk_timer_init exit",1,0},
  {"sctrl_sta_ps_init",1,0}
};

static const char *const c241_args[] = {"--only-uart","-chip","BK7238","--wifi"};
static const ExpectStr c241_exp[] = {
  {"prvHeapInit-start addr",1,0},
  {"bk_timer_init exit",1,0},
  {"sctrl_sta_ps_init",1,0}
};

static const char *const c242_args[] = {"--only-uart","-chip","BK7231U","--wifi"};
static const ExpectStr c242_exp[] = {
  {"[WIFI] device RX filter 0x7fffffde: monitor mode",1,0},
  {"==app_start==",1,0},
  {"==user_main==",1,0},
  {"ssid:7231u-test key:12345678",1,0}
};

static const char *const c243_args[] = {"--only-uart","-chip","BK7252","--wifi"};
static const ExpectStr c243_exp[] = {
  {"Thread Operating System",1,0},
  {"3.1.0 build Apr  1 2024",1,0},
  {"[FUNC]rwnxl_init",1,0},
  {"[FUNC]calibration_main",1,0}
};

static const char *const c244_args[] = {"--only-uart","-chip","BK7231N","--wifi","--ble-core"};
static const ExpectStr c244_exp[] = {
  {"[WIFI] device RX filter 0x7fffffde: monitor mode",1,0},
  {"SetupQRCode: [MT:--VJ0-IC005WSL02810]",1,0},
  {"Copy/paste the below URL in a browser to see the QR Code:https://project-chip.github.io/connectedhomeip/qrcode.html",1,0}
};

static const char *const c245_args[] = {"--only-uart","-chip","BK7231N","-key","00000000000000002e38810101000120","--wifi","--ble-core"};
static const ExpectStr c245_exp[] = {
  {"[WIFI] device RX filter 0x7fffffde: monitor mode",1,0}
};

static const char *const c246_args[] = {"--only-uart","-chip","BK7231N","-key","0000000000000000376e7eff01000803","--wifi","--ble-core"};
static const ExpectStr c246_exp[] = {
  {"[WIFI] device RX filter 0x7fffffde: monitor mode",1,0},
  {"[BLE_ADV] EM 0x00910000",1,0},
  {"SBDV-00117",1,0}
};

static const char *const c247_args[] = {"--only-uart","-chip","BK7231N","-key","0000000000000000ffffaf7f01000007","--wifi","--ble-core"};
static const ExpectStr c247_exp[] = {
  {"[DEVINFO]app partition signature not found",1,0},
  {"[WIFI] device RX filter 0x7fffffde: monitor mode",1,0},
  {"[STORAGE_PW]failed to read key:WIFI_SSID",1,0}
};

static const char *const c248_args[] = {"--only-uart","-chip","BK7231N","-key","0000000000000000f9610db101001803","--wifi","--ble-core"};
static const ExpectStr c248_exp[] = {
  {"[WIFI] device RX filter 0x7fffffde: monitor mode",1,0},
  {"[POWER_CTRL]NO DATA",1,0}
};

static const char *const c249_args[] = {"--only-uart","-chip","BK7231N","-key","0000000000000000b8bff72e01001803","--wifi","--ble-core"};
static const ExpectStr c249_exp[] = {
  {"[DEVINFO]app partition signature not found",1,0},
  {"[WIFI] device RX filter 0x7fffffde: monitor mode",1,0}
};

static const char *const c250_args[] = {"--only-uart","-chip","BK7231","--wifi"};
static const ExpectStr c250_exp[] = {
  {"bk_misc_init_start_type 0 0",1,0},
  {"prvHeapInit-start addr",1,0},
  {"[Flash]init over",1,0}
};

static const char *const c251_args[] = {"--only-uart","-chip","BK7231","--wifi"};
static const ExpectStr c251_exp[] = {
  {"bk_misc_init_start_type 0 0",1,0},
  {"prvHeapInit-start addr",1,0},
  {"[Flash]init over",1,0}
};

static const char *const c252_args[] = {"--only-uart","-chip","BK7231","--wifi"};
static const ExpectStr c252_exp[] = {
  {"bk_misc_init_start_type 0 0",1,0},
  {"prvHeapInit-start addr",1,0},
  {"[Flash]init over",1,0}
};

static const char *const c253_args[] = {"--only-uart","-chip","BK7231","--wifi"};
static const ExpectStr c253_exp[] = {
  {"bk_misc_init_start_type 0 0",1,0},
  {"prvHeapInit-start addr",1,0},
  {"[Flash]init over",1,0}
};

static const char *const c254_args[] = {"--only-uart","-chip","BK7231","--wifi"};
static const ExpectStr c254_exp[] = {
  {"bk_misc_init_start_type 0 0",1,0},
  {"prvHeapInit-start addr",1,0},
  {"[Flash]init over",1,0}
};

static const char *const c255_args[] = {"--only-uart","-chip","BK7231N","--wifi","--ble-core"};
static const ExpectStr c255_exp[] = {
  {"bk_misc_init_start_type 0 0",1,0},
  {"prvHeapInit-start addr",1,0},
  {"[Flash]init over",1,0}
};

static const char *const c256_args[] = {"--only-uart","-chip","BK7231N","--wifi","--ble-core"};
static const ExpectStr c256_exp[] = {
  {"bk_misc_init_start_type 0 0",1,0},
  {"prvHeapInit-start addr",1,0},
  {"[Flash]init over",1,0}
};

static const char *const c257_args[] = {"--only-uart","-chip","BK7231N","--wifi","--ble-core"};
static const ExpectStr c257_exp[] = {
  {"bk_misc_init_start_type 0 0",1,0},
  {"prvHeapInit-start addr",1,0},
  {"[Flash]init over",1,0}
};

static const char *const c258_args[] = {"--only-uart","-chip","BK7231N","--wifi","--ble-core"};
static const ExpectStr c258_exp[] = {
  {"bk_misc_init_start_type 0 0",1,0},
  {"[Flash]init over",1,0},
  {"sctrl_sta_ps_init",1,0}
};

static const char *const c259_args[] = {"--only-uart","-chip","BK7252N","--wifi"};
static const ExpectStr c259_exp[] = {
  {"[WIFI] device RX filter 0x7fffffde: monitor mode",1,0},
  {"beken_onchip",1,0},
  {"FAL partition table",1,0}
};

static const char *const c260_args[] = {"--only-uart","-chip","BK7252N","--wifi"};
static const ExpectStr c260_exp[] = {
  {"[WIFI] device RX filter 0x7fffffde: monitor mode",1,0},
  {"beken_onchip",1,0},
  {"FAL partition table",1,0}
};

static const char *const c261_args[] = {"--only-uart","-chip","BK7252N","--wifi"};
static const ExpectStr c261_exp[] = {
  {"[WIFI] device RX filter 0x7fffffde: monitor mode",1,0},
  {"beken_onchip",1,0},
  {"FAL partition table",1,0}
};

static const char *const c262_args[] = {"--only-uart","-chip","BK7252","--wifi"};
static const ExpectStr c262_exp[] = {
  {"Thread Operating System",1,0},
  {"3.1.0 build Apr  1 2024",1,0},
  {"[FUNC]rwnxl_init",1,0}
};

static const char *const c263_args[] = {"--only-uart","-chip","BK7252","--wifi"};
static const ExpectStr c263_exp[] = {
  {"Thread Operating System",1,0},
  {"3.1.0 build Aug  7 2024",1,0},
  {"test_flash_read_for_mac",1,0}
};

static const char *const c264_args[] = {"--only-uart","-chip","BK7252","--wifi"};
static const ExpectStr c264_exp[] = {
  {"[WIFI] device RX filter 0x7fffffde: monitor mode",1,0},
  {"[WIFI] device RX filter 0x3503848c: back to normal reception",1,0}
};

static const char *const c265_args[] = {"--only-uart","-chip","BK7252","--wifi"};
static const ExpectStr c265_exp[] = {
  {"[WIFI] device RX filter 0x7fffffde: monitor mode",1,0},
  {"[CAMERA] GC0328C streaming 320x240 JPEG",1,0}
};

static const char *const c266_args[] = {"--only-uart","-chip","BK7238","--wifi"};
static const ExpectStr c266_exp[] = {
  {"power on",1,0},
  {"[WIFI] device RX filter 0x7fffffde: monitor mode",1,0},
  {"[BLE_ADV] EM 0x0040e480",1,0},
  {"WF24-BLE",1,0}
};

static const char *const c267_args[] = {"--only-uart","-chip","BK7238","--wifi"};
static const ExpectStr c267_exp[] = {
  {"bk_misc_init_start_type 0 0",1,0},
  {"prvHeapInit-start addr",1,0},
  {"[Flash]init over",1,0}
};

static const char *const c268_args[] = {"--only-uart","-chip","BK7238","--wifi"};
static const ExpectStr c268_exp[] = {
  {"[MEM DBG] heap init",1,0},
  {"bk_timer_init exit",1,0},
  {"sctrl_sta_ps_init",1,0}
};

static const char *const c269_args[] = {"--only-uart","-chip","BK7238","--wifi"};
static const ExpectStr c269_exp[] = {
  {"[MEM DBG] heap init",1,0},
  {"bk_timer_init exit",1,0},
  {"sctrl_sta_ps_init",1,0}
};

static const char *const c270_args[] = {"--only-uart","-chip","BK7238","--wifi"};
static const ExpectStr c270_exp[] = {
  {"bk_misc_init_start_type 0 0",1,0},
  {"prvHeapInit-start addr",1,0},
  {"[Flash]init over",1,0}
};

static const char *const c271_args[] = {"--only-uart","-chip","BK7238","--wifi"};
static const ExpectStr c271_exp[] = {
  {"bk_misc_init_start_type 0 0",1,0},
  {"prvHeapInit-start addr",1,0},
  {"[Flash]init over",1,0}
};

static const char *const c272_args[] = {"--only-uart","-chip","BK7238","--wifi"};
static const ExpectStr c272_exp[] = {
  {"bk_misc_init_start_type 0 0",1,0},
  {"prvHeapInit-start addr",1,0},
  {"[Flash]init over",1,0}
};

static const char *const c273_args[] = {"--only-uart","-chip","BK7238","--wifi"};
static const ExpectStr c273_exp[] = {
  {"[MEM DBG] heap init",1,0},
  {"bk_timer_init exit",1,0},
  {"sctrl_sta_ps_init",1,0}
};

static const char *const c274_args[] = {"--only-uart","-chip","BK7238","--wifi"};
static const ExpectStr c274_exp[] = {
  {"[MEM DBG] heap init",1,0},
  {"bk_timer_init exit",1,0},
  {"sctrl_sta_ps_init",1,0}
};

TestCase g_cases_arr[] = {
  {"CLI: invalid -key is rejected","firmwares/OpenBK7231T_QIO_1.18.300.bin",c0_args,3,60,"",0,10,c0_exp,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A bad -key value must be rejected up front by parse_key(), with a message naming the accepted forms (a known key name, 32 hex chars, or base64 of 16 bytes), rather than failing deep inside emulation."},
  {"CLI: invalid -chip is rejected","firmwares/OpenBK7231T_QIO_1.18.300.bin",c1_args,3,60,"",0,10,c1_exp,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "An unknown -chip name must be rejected and the known chip identities listed, so a typo fails fast instead of emulating with the wrong SCTRL id registers."},
  {"CLI: --ble-core is rejected on a chip without a BLE 5.x core","firmwares/OpenBK7252_QIO_1.18.300.bin",c2_args,4,60,"",0,10,c2_exp,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   ""},
  {"CLI: encrypted image without a key warns","firmwares/OpenBK7231T_QIO_1.18.300.bin",c3_args,1,60,"",0,10,c3_exp,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "Running an encrypted image with no key must warn that the decrypted slice does not look like ARM code and suggest -key TUYA, instead of silently emulating garbage."},
  {"CLI: malformed --net-forward is rejected","firmwares/OpenBK7231T_QIO_1.18.300.bin",c4_args,5,60,"",0,10,c4_exp,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A port forward that is not LPORT:DPORT must be refused with a one-line message before anything boots."},
  {"CLI: malformed --adc is rejected","firmwares/OpenBK7231T_QIO_1.18.300.bin",c5_args,5,60,"",0,10,c5_exp,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A malformed --adc specification must be rejected up front by parse_adc_specs() with the expected CH:VALUE syntax, rather than starting emulation with unparsed ADC overrides."},
  {"OpenBK7231T Wi-Fi AP: virtual station joins, DHCP, ping, HTTP command runs, panel on localhost","firmwares/OpenBK7231T_QIO_1.18.300.bin",c6_args,9,900,"",0,10,c6_exp,12,0,0,0,0,0,0,0,0,0,0,c6_probes,1,0,0,1000,
   "The emulated Wi-Fi path end to end: the RivieraWaves MAC is modelled, a virtual station joins OpenBeken's open AP, takes a DHCP lease from it, pings it and sends an HTTP GET to its command API - the command runs in the firmware and its reply comes back over the air. Then the harness opens the device's web panel through a real port forward on 127.0.0.1, the way a browser does, and checks the page is the real panel."},
  {"OpenBK7238 Wi-Fi AP: virtual station joins, DHCP, ping, HTTP command runs","firmwares/OpenBK7238_QIO_1.18.300.bin",c7_args,7,1200,"",0,10,c7_exp,6,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "The same emulated Wi-Fi path on BK7238 silicon, with the MAC model unchanged: join, DHCP from the device, ping and an HTTP command executed by the firmware."},
  {"OpenBK7231T Wi-Fi station: joins virtual AP, DHCP, HTTP in, MQTT out captured","firmwares/OpenBK7231T_QIO_1.18.300_obkConfig_staOpen_BekenSim_netTraffic.bin",c8_args,9,1200,"",0,10,c8_exp,8,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "OpenBeken configured for an open network joins the virtual access point, gets its address from the virtual network, answers an HTTP command coming in, and its outgoing MQTT connection is resolved to a sinkhole and decoded instead of leaving the machine."},
  {"OpenBK7231T Wi-Fi station: joins a WPA2-protected network and uses it","firmwares/OpenBK7231T_QIO_1.18.300_obkConfig_staWPA2_BekenSec.bin",c9_args,9,1200,"",0,10,c9_exp,7,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "The emulated access point could only ever be an open network, so a device that had stored a protected one could see it and never join. Here it offers a protected network and the device joins it properly: both ends already hold the passphrase, the four-way handshake is how each proves that, and the link stays shut until it completes. The address, the clock and the outgoing request all happen on the far side of it."},
  {"OpenBK7231T Wi-Fi station: outgoing NTP, HTTP GET and MQTT answered offline, pcap holds them","firmwares/OpenBK7231T_QIO_1.18.300_obkConfig_staOpen_BekenSim_netTraffic.bin",c10_args,13,1200,"",0,10,c10_exp,16,0,0,0,0,0,0,c10_pcap,8,c10_ippcap,5,0,0,0,0,1000,
   "Everything the device starts itself, answered without a real network: its NTP query gets the host clock, its repeating HTTP GET to a cloud-style name gets a 200 from the offline responder each time, and its MQTT client completes CONNECT, SUBSCRIBE and PUBLISH against the offline broker until the firmware reports the session up. The broker then pushes a command back to the device on a topic the firmware subscribed to itself, and the firmware runs it - the device driven over MQTT, not merely listened to. The 802.11 capture file written alongside is parsed back and must hold the same exchange in both directions."},
  {"Snapshot: resumed MathDemo run reproduces the original run","firmwares/OpenBK7231T_QIO_1.18.300_mathDemo.bin",c11_args,3,300,"snapshot_roundtrip",1500000,20,c11_exp,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "Saves a snapshot mid-boot and resumes from it in a second run. The emulator is deterministic, so the resumed run must print the same firmware lines as the original after the snapshot point, with the instruction counter within a small tolerance (it is estimated per translated block, which a cold translation cache can split differently). Anything the snapshot forgets shows up as a difference."},
  {"Snapshot: resumed OpenBK7231T Wi-Fi AP run reproduces the original run","firmwares/OpenBK7231T_QIO_1.18.300.bin",c12_args,5,900,"snapshot_roundtrip",37000000,8,c12_exp,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "The same round trip with the Wi-Fi MAC modelled and OpenBeken's AP up - the path the web-panel launcher takes. The resumed device must associate, lease and answer ping exactly as the original run did."},
  {"OpenBK7231T_QIO_1.18.300 Boot to MQTT and 1s timers","firmwares/OpenBK7231T_QIO_1.18.300.bin",c13_args,3,360,"",0,10,c13_exp,8,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "Full OpenBeken boot on the flagship BK7231T image, then ten Main_OnEverySecond ticks - proving the timer keeps firing, not just starts. Also proves the tick interrupt, the FreeRTOS timer daemon and MQTT registration are alive; with no WiFi in the emulator, MQTT stays disconnected (bWifi 0). Runs on to ~5 device-seconds, where the no-SSID path opens the fallback access point: the HAL logs its IP config and the MAC-derived SSID (OpenBK7231T_xxxx), then goes silent inside bk_wlan_start() - the exact edge of what the emulator models of the WiFi stack."},
  {"MathDemo Boot and Float Verification","firmwares/OpenBK7231T_QIO_1.18.300_mathDemo.bin",c14_args,3,120,"",0,10,c14_exp,6,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A special OpenBeken build that runs integer and floating-point math at boot and logs the results. Verifies the CPU's software-float paths and that FreeRTOS software timers fire (quicktick); with no config in flash it takes the default-config path."},
  {"UART1 Command Console: echo runs, unknown command rejected","firmwares/OpenBK7231T_QIO_1.18.300_mathDemo_obkStartupCommand_uartConsole.bin",c15_args,5,300,"",0,10,c15_exp,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "Bidirectional UART1 - the receive counterpart of the uartSendHex case. The injected config sets OBK flag 31 (OBK_FLAG_CMD_ACCEPT_UART_COMMANDS), starting a command console on UART1. --uart1-rx feeds two newline-separated lines into the receive FIFO once the console's RX callback is up: 'echo UartRxConsoleOK' then 'nosuchcmd_uarttest'. OBK runs the first (echoing the argument) and rejects the second as unknown (Error:CMD:cmd ... NOT found) - proof the full RX path works and a real command parser, not a byte reflector, is running."},
  {"UART1 Command Console: setChannel arg 12+28 evaluated to 40","firmwares/OpenBK7231T_QIO_1.18.300_mathDemo_obkStartupCommand_uartConsole.bin",c16_args,5,300,"",0,10,c16_exp,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "Proves OBK evaluates a command argument as an expression, over UART1. --uart1-rx injects 'setChannel 5 12+28'; OBK's tokenizer resolves 12+28 to 40 before the setChannel handler runs, so channel 5 is set to 40 - not rejected as non-numeric, nor truncated to 12. The 'CHANNEL_Set channel 5 has changed to 40' log line carries the resolved value."},
  {"UART1 Command Console: backlog runs two commands from one line","firmwares/OpenBK7231T_QIO_1.18.300_mathDemo_obkStartupCommand_uartConsole.bin",c17_args,5,300,"",0,10,c17_exp,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "Closes the multi-command loop over UART1. A plain console line is one command (OBK does not split on ';'), so multiple commands need the backlog prefix. --uart1-rx injects 'backlog echo BacklogOne; echo BacklogTwo'; backlog splits on ';' and runs both echoes, so both arguments come back - proof the console parses and dispatches several commands from one received line."},
  {"UART1 Command Console: SetPinRole P23 ADC reads --adc P23:2150 into channel 8","firmwares/OpenBK7231T_QIO_1.18.300_mathDemo_obkStartupCommand_uartConsole.bin",c18_args,7,300,"",0,10,c18_exp,3,0,0,0,0,0,0,0,0,0,0,0,0,c18_tags,1,1000,
   "End-to-end SARADC input test on BK7231T: --adc P23:2150 sets SARADC channel 3 (P23) to raw sample 2150, and 'backlog SetPinChannel 23 8; SetPinRole 23 ADC' over the UART1 console makes PIN_OnEverySecond() read HAL_ADC_Read(23) and publish 2150 to channel 8."},
  {"MathDemo Startup Command: echo","firmwares/OpenBK7231T_QIO_1.18.300_mathDemo_obkStartupCommand_echo.bin",c19_args,3,180,"",0,10,c19_exp,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "The mathDemo image with a hand-crafted OpenBeken config injected at flash 0x1e1000, whose startup command is 'echo Test...'. Proves the emulated flash controller serves a full 32-byte page per read and that the config's signed-char Tiny_CRC8 is accepted, then the echo command runs."},
  {"MathDemo Startup Command: uartSendHex on UART1","firmwares/OpenBK7231T_QIO_1.18.300_mathDemo_obkStartupCommand_uartSendHex.bin",c20_args,4,180,"",0,10,c20_exp,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "Same config-injection trick, but the startup command drives OpenBeken's uartSendHex to place arbitrary bytes on UART1 (the MCU link). Exercises config load -> command registration -> HAL UART write -> the emulator's UART1 hex capture."},
  {"MathDemo Startup Command: TuyaMCU full link, data points and channels","firmwares/OpenBK7231T_QIO_1.18.300_mathDemo_obkStartupCommand_tuyaMCU.bin",c21_args,23,420,"",0,10,c21_exp,37,0,0,0,0,0,0,0,0,0,0,0,0,c21_tags,3,1000,
   "The deepest TuyaMCU case here. The driver opens UART1 itself and talks first, unprompted: the first per-second tick emits a heartbeat (55 AA 00 00 00 00 FF). A simulated MCU answers, so OpenBeken completes the whole handshake and then reports one data point of every wire type - bool, value, string, enum, bitmap. The startup command binds those data points to channels, so the case checks both directions: a reported DP must land on its channel, and a channel moved by a change handler must go back out as a 0x06 SET_DP frame. The MCU then speaks unprompted, including a frame with a deliberately wrong checksum and one behind four junk bytes, to check that bad input is discarded and the framer resynchronises instead of desyncing."},
  {"Berry Startup Command: print(\"Hello \" + str(5+2*2))","firmwares/OpenBK7231T_QIO_1.18.302_berry_obkStartupCommand_berryHello.bin",c22_args,3,180,"",0,10,c22_exp,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "Pulls OpenBeken's dedicated 1.18.302 Berry build and runs a one-line Berry script from the startup command: berry print(\"Hello \" + str(5+2*2)). Exercises the embedded VM end to end - arithmetic precedence (=9), str(), string concatenation and print() - which surfaces as Info:BERRY:Hello 9."},
  {"MathDemo Startup Command: SetPinRole drives relay pin P9","firmwares/OpenBK7231T_QIO_1.18.300_mathDemo_obkStartupCommand_setPinRoleP9.bin",c23_args,3,180,"",0,10,c23_exp,2,0,0,c23_pins,2,0,0,0,0,0,0,0,0,0,0,1000,
   "Startup command 'SetPinChannel 9 1; SetPinRole 9 Rel; SetChannel 1 0; SetChannel 1 1'. Orders OpenBeken to make pin 9 a relay output bound to channel 1, then switches that channel on. The only case where the expected GPIO register word is known independently - from the vendor SDK's own gpio_config()/gpio_output() - so it verifies the emulator's pin capture rather than merely displaying it."},
  {"MathDemo Startup Command: SetPinRole drives relay pin P7","firmwares/OpenBK7231T_QIO_1.18.300_mathDemo_obkStartupCommand_setPinRoleP7.bin",c24_args,3,180,"",0,10,c24_exp,2,0,0,c24_pins,2,0,0,0,0,0,0,0,0,0,0,1000,
   "The P9 case repeated on a different pin (7) and a different channel (2), with an assertion that pin 9 stays untouched. Two pins moving independently under command is what rules out a fixed write landing on a captured index by coincidence."},
  {"MathDemo Startup Command: AlwaysHigh and AlwaysLow pins","firmwares/OpenBK7231T_QIO_1.18.300_mathDemo_obkStartupCommand_alwaysHighLow.bin",c25_args,3,180,"",0,10,c25_exp,2,0,0,c25_pins,3,0,0,0,0,0,0,0,0,0,0,1000,
   "Startup command 'SetPinRole 14 AlwaysHigh; SetPinRole 15 AlwaysLow'. The two fixed-output roles drive a pin high and low with no channel involved, on plain GPIOs that have no second function. Asserting the low pin at 0x00 - a written value, not an absent one - is what separates 'driving low' from 'never configured'."},
  {"MathDemo Startup Command: PWM 1400 Hz on pin 9","firmwares/OpenBK7231T_QIO_1.18.300_mathDemo_obkStartupCommand_setPinRolePWM1400.bin",c26_args,3,180,"",0,10,c26_exp,2,0,0,c26_pins,1,c26_pwm,1,0,0,0,0,0,0,0,0,1000,
   "Startup command 'PWMFrequency 1400; SetPinChannel 9 1; SetPinRole 9 PWM; SetChannel 1 50'. Asks for a deliberately uncommon 1400 Hz so the expected period (26 MHz / 1400 = 18571) is a value nothing else produces, then checks the captured PWM registers decode back to that frequency and to 50% duty - verifying the PWM decode, not just the capture."},
  {"MathDemo Startup Command: PWM 2500 Hz on pin 7","firmwares/OpenBK7231T_QIO_1.18.300_mathDemo_obkStartupCommand_setPinRolePWM2500.bin",c27_args,3,180,"",0,10,c27_exp,2,0,0,c27_pins,1,c27_pwm,1,0,0,0,0,0,0,0,0,1000,
   "The 1400 Hz case with every variable moved: pin 7 instead of 9, PWM index 1 instead of 3, channel 3, 2500 Hz and 25% duty. Because PWM1's period register sits at a different offset under the stride-12 layout than it would under stride-8, this case pins down the register stride itself rather than just re-confirming one frequency."},
  {"MathDemo Startup Command: PWM 10 kHz on pins 24 and 26","firmwares/OpenBK7231T_QIO_1.18.300_mathDemo_obkStartupCommand_setPinRolePWM10000.bin",c28_args,3,180,"",0,10,c28_exp,3,0,0,c28_pins,2,c28_pwm,2,0,0,0,0,0,0,0,0,1000,
   "Two PWM channels driven at once on the highest pads (P24/P26 = PWM4/PWM5) at 10 kHz, with different duties on each (10% and 75%). Exercises the top of the captured register window and the first multi-channel PWM_CTL value, where an off-by-one in the channel stride would push the registers out of range entirely."},
  {"OpenBK7231N Startup Command: PWM 3700 Hz on pin 8 (pwm_new)","firmwares/OpenBK7231N_QIO_1.18.300_obkStartupCommand_setPinRolePWM3700.bin",c29_args,3,240,"",0,10,c29_exp,2,0,0,c29_pins,1,c29_pwm,1,0,0,0,0,0,0,0,0,1000,
   "The synthetic ground-truth trick on the BK7231N image: OpenBeken is told to do 3700 Hz at 40% duty on P8, which its HAL programs through the pwm_new block. Both values were predicted before the first run - period 0x1B73 and a 40% toggle at 0x0AFA - and the duty is reconstructed by replaying edge toggles, making this the sharpest single check the N-family decoder has."},
  {"OpenBK7231N Startup Command: dual PWM 25%/70% on pins 7 and 9","firmwares/OpenBK7231N_QIO_1.18.300_obkStartupCommand_setPinRolePWMdual.bin",c30_args,3,240,"",0,10,c30_exp,3,0,0,c30_pins,2,c30_pwm,2,0,0,0,0,0,0,0,0,1000,
   "Two pwm_new channels commanded to different duties (25% and 70%) at 4500 Hz, both on the sub-channel-1 half of their groups - the slots every stock N dump leaves at 0%, so the toggle-replay duty decode for them had only ever been checked against zero. Distinct values on distinct groups also rule out cross-channel register mix-ups."},
  {"OpenBK7231U_QIO_1.18.300 Boot to 1s timers","firmwares/OpenBK7231U_QIO_1.18.300.bin",c31_args,3,180,"",0,10,c31_exp,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "OpenBeken on the BK7231U variant from a plaintext (no-key) image, booting through to the per-second timer - confirms the shared BK7231 model and the no-decrypt path."},
  {"OpenBK7238_QIO_1.18.300 Boot to 1s timers","firmwares/OpenBK7238_QIO_1.18.300.bin",c32_args,3,180,"",0,10,c32_exp,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "OpenBeken on the newer BK7238 silicon (-chip BK7238). Boots through the SPI flash mirror and the crypto/XVR peripherals to the per-second timer, checking BK7238 chip-identity gating."},
  {"OpenBK7231N_QIO_1.18.300 Boot to 1s timers","firmwares/OpenBK7231N_QIO_1.18.300.bin",c33_args,3,180,"",0,10,c33_exp,6,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "OpenBeken on BK7231N. This variant blocks in the per-second temperature read waiting on the SARADC interrupt (ICU bit 11), so reaching the 1s timer proves the emulated SARADC FIFO + interrupt model works."},
  {"OpenBK7231M_QIO_1.18.300 Boot to 1s timers (no key)","firmwares/OpenBK7231M_QIO_1.18.300.bin",c34_args,1,180,"",0,10,c34_exp,6,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "OpenBeken on BK7231M from a plaintext image (no key). Confirms the shared model and no-key path reach the per-second timer."},
  {"OpenBK7252_QIO_1.18.300 Boot to 1s timers","firmwares/OpenBK7252_QIO_1.18.300.bin",c35_args,3,480,"",0,10,c35_exp,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "OpenBeken on BK7252 (BK7221U silicon, -chip BK7252). One of the heavier boots; reaches the per-second timer."},
  {"OpenBK7252N_QIO_1.18.300 Boot to 1s timers","firmwares/OpenBK7252N_QIO_1.18.300.bin",c36_args,3,480,"",0,10,c36_exp,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "OpenBeken on BK7252N (-chip BK7252N). The heaviest boot in the suite; reaches the per-second timer."},
  {"BK7238 Sonoff 4MB Dump Boots (SPI mirror + XVR)","firmwares/Sonoff_S61s_EUPlug_WBBK_01P_V1.3.bin",c37_args,3,240,"",0,10,c37_exp,6,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A real 4MB Sonoff flash dump on BK7238. Exercises the SPI flash mirror and the XVR peripheral through the vendor SDK boot to calibration / normal-mode markers."},
  {"BK7231Q Tuya MOES Relay Boot","firmwares/BK7231Q_Tuya_MOES_Relay_WA2_1.1.3.bin",c38_args,7,120,"",0,10,c38_exp,7,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A Tuya BK7231Q relay dump that initializes its GPIO/datapoint map, returns from TCP/IP stack initialization and actually schedules the Wi-Fi supplicant and core tasks before waiting normally for external manufacturing/provisioning input."},
  {"BL2028N Dreo fan clears BLE gate and serves its pairing AP","firmwares/BL2028N_Dreo_DR-HTF004S_Fan_PAI-053.bin",c39_args,6,180,"",0,10,c39_exp,12,0,0,0,0,0,0,c39_pcap,6,0,0,0,0,c39_tags,3,1000,
   "A BL2028N (BK7231N-class) Dreo fan dump that used to stop at the BLE-controller handoff. With the BLE core model enabled, the stock firmware completes HCI setup, publishes its DREO BLE name, starts its open provisioning AP, leases an address to the virtual station and answers its ping."},
  {"Tuya TMWF02 TuyaMCU Boots past crypto accel","firmwares/BK7231T_Tuya_TMWF02_Fan_Switch_TuyaMCU_1.1.71.bin",c40_args,5,300,"",0,10,c40_exp,7,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A stock Tuya fan-switch dump. Boots past the 0x810000 crypto-accelerator stall into the BLE host stack and network-config advertising."},
  {"Woox Tuya Original Firmware Boot","firmwares/BK7231T_QIO_Woox_R5111_2023-14-10-23-46-06.bin",c41_args,3,300,"",0,10,c41_exp,9,c41_unexp,1,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "Original Tuya (non-OpenBeken) light firmware. Verifies the fix that serves the un-striped protected-key region at 0x1ee000: instead of inventing a key from blank flash, the firmware now retrieves the stored key. With nothing on air it must not reach production test, which is the control for the case below."},
  {"Woox Tuya stock firmware: factory test network on air switches it to production test","firmwares/BK7231T_QIO_Woox_R5111_2023-14-10-23-46-06.bin",c42_args,9,900,"",0,10,c42_exp,4,0,0,0,0,0,0,c42_pcap,4,0,0,0,0,0,0,1000,
   "Stock Tuya firmware, not OpenBeken, changing course because of what the emulator put on air. Every Tuya device scans at boot for the factory test network baked into it; when the virtual access point advertises that name, this one stops its normal start-up and enters manufacturing test. It never joins the network - seeing it advertised is the whole trigger. The same firmware with nothing on air does not do this, which is the case above."},
  {"BK7238 Tuya RGB controller: raises its SmartLife pairing AP, serves DHCP, answers ping","firmwares/BK7238_Tuya_RGBController_TY-02-3CH_PWM_T1-3S_1.0.14.bin",c43_args,8,1200,"",0,10,c43_exp,7,0,0,0,0,0,0,c43_pcap,6,0,0,0,0,0,0,1000,
   "The pairing mode where the device makes its own network for a phone to join. This stock dump raises it unprompted, so the emulator joins as a client, is given an address by the device, and gets an answer to a ping. The device then announces itself in Tuya's LAN protocol; this firmware's version of it is sealed with a key only that device holds, so the announcement is identified by its framing and its contents are left alone."},
  {"BK7231N Tuya 3-phase meter (paired): joins its stored network and reaches for the Tuya cloud","firmwares/BK7231N_Tuya_PC321_3PhaseMeter_TuyaMCU_2.0.2.bin",c44_args,12,1200,"",0,10,c44_exp,14,0,0,0,0,0,0,c44_pcap,7,0,0,0,0,0,0,1000,
   "A stock Tuya device captured after it had been set up, so its flash still holds the owner's network name. It asks for that network, the emulator plays it, and the device joins, takes an address, announces its own cloud identity on the LAN in a form every Tuya device encrypts the same way, and opens TLS to its cloud twice - once at a fixed address and once by name. Nothing leaves the machine: the connections are accepted and decoded locally. The identity it announces matches the product key given on the command line, which is what confirms the decryption rather than assuming it. The cloud connection itself is answered far enough to learn how it is secured: Tuya devices authenticate with a shared secret rather than a certificate, and this one accepts the answer and then stops, because the dump holds no such secret to present."},
  {"BK7231N Tuya bulb (paired): joins its stored network and reaches the Tuya cloud","firmwares/BK7231N_Tuya_Feconn_MR16_RGBCT_5ch_PWM_1000Hz_1.3.21.bin",c45_args,6,1200,"",0,10,c45_exp,10,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "The bulb above once the emulator stops it stalling: it turns out to be a paired device that joins the network name still in its flash and calls its cloud. It is the second device here to confirm how Tuya secures that link, with a different SDK and a longer cipher list but still no certificate and no server name, and it stops at the same point for the same reason, having no key of its own to present."},
  {"BK7231N Tuya thermostat: EZ-mode sniffing reported, then falls back to AP pairing","firmwares/BK7231N_Tuya_BEOK_TOL47WIFI_Thermostat_TuyaMCU_TuyaOS_3.11.12.bin",c46_args,9,1800,"",0,10,c46_exp,6,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "The pairing mode that needs real hardware behaviour: the device opens its receiver to every frame on the channel and reads the credentials out of the pattern a phone sends. The emulated MAC reports that the firmware did this, which is what tells a sniffing device apart from a scanning one. The same run then shows it give up, close the receiver back down and fall back to making its own pairing network."},
  {"BK7231N Tuya alarm (paired): a second device's cloud identity decodes with the same shared key","firmwares/BK7231N_Tuya_A03CB3S_Alarm_TuyaMCU_2.1.17.bin",c47_args,10,1200,"",0,10,c47_exp,7,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A different paired device with a different stored network and a different identity, included to show the announcement is readable because every Tuya device encrypts it with the same key, not because of anything special about the meter. What it announces matches the product key passed on the command line, which rules out rubbish that merely parses."},
  {"BK7231N Tuya TempHum Sensor accepts its MCU identity and starts BLE/Wi-Fi","firmwares/BK7231N_Tuya_TempHum_LCD_Sensor_TuyaMCU_2025-26-9.bin",c48_args,11,300,"",0,10,c48_exp,12,c48_unexp,2,0,0,0,0,0,0,0,0,0,0,c48_tags,3,1000,
   "The stock Tuya temp/humidity sensor that formerly looped forever on product-info queries to its absent companion MCU. The simulated MCU supplies the exact product identity from the dump; firmware validates it, initializes the device and BLE service, then advances to its working-mode query while Wi-Fi MAC activity is present."},
  {"BK7231N Tuya plug auto-models BLE and enters factory mode","firmwares/BK7231N_Tuya_Plug_TSL-PS-F4U-01-W_1.1.17.bin",c49_args,12,120,"",0,10,c49_exp,21,c49_unexp,2,0,0,0,0,c49_pcap,4,0,0,0,0,c49_tags,3,1000,
   "A stock five-channel Tuya plug on SDK 2.3.3. Selecting its real BK7231N silicon now provides BLE 5.1 hardware automatically, without BLE/XVR workaround flags; the firmware completes HCI, database and advertising setup, initializes every relay plus metering, sees the emulated factory network and takes its production-mode branch."},
  {"BK7231N Tuya zmai90 RN8209C Energy Meter Boots into its app","firmwares/BK7231N_Tuya_zmai90_RN8209C_EnergyMeter.bin",c50_args,5,300,"",0,10,c50_exp,8,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A stock Tuya energy meter (zmai90, RN8209C metering chip). A pre-pair (unactivated) dump: it reads its protected key but then parks in the mf_test thread, so it never polls the meter over UART1. Proves the boot + key read; the empty UART1 is the dump's provisioning state."},
  {"BK7238 Tuya RGB Controller TY-02-3CH (stock, unencrypted) boots its light stack","firmwares/BK7238_Tuya_RGBController_TY-02-3CH_PWM_T1-3S_1.0.14.bin",c51_args,3,420,"",0,10,c51_exp,10,0,0,c51_pins,3,0,0,0,0,0,0,0,0,c51_tags,1,1000,
   "The first stock-firmware dump on BK7238 - every other image on this silicon here is OpenBeken, so the chip identity had only ever been exercised by firmware we build ourselves. This one reads the SCTRL device-id register back and prints it, so the device confirms the identity -chip BK7238 hands it. It is also the only stock Tuya dump that is plaintext rather than TUYA-encrypted. It boots through RF calibration into the Tuya light stack - colour gamma tables, dimmer curve, startup state - reports its own product id and starts BLE, with no --xvr-selfclear needed."},
  {"BK7238 Tuya RGB Controller with --ble-core: BLE host completes HCI, adv applied","firmwares/BK7238_Tuya_RGBController_TY-02-3CH_PWM_T1-3S_1.0.14.bin",c52_args,4,420,"",0,10,c52_exp,5,0,0,0,0,0,0,0,0,0,0,0,0,c52_tags,3,1000,
   "The same BK7238 dump with the RivieraWaves BLE core modelled (--ble-core). Without it the controller arms deep sleep and never wakes, so the NimBLE host times out on every HCI command and loops on assert. With the core's slot clock, sleep/wake, clock correction and slot-clock interrupt modelled - and FIQ delivery added to the emulator - the controller runs its real sleep/wake loop, acknowledges every interrupt, answers the host, and the Tuya BLE service reports the advertisement applied. No radio: nothing goes on air."},
  {"BK7231M VeSync WHOGPLUG Plug (VeSync SDK, BLE 5.1 core) advertises and scans","firmwares/BK7231M_VeSync_GreenSun_WHOGPLUG_Plug_1.0.01.bin",c53_args,2,420,"",0,10,c53_exp,11,c53_unexp,3,0,0,0,0,0,0,0,0,0,0,c53_tags,3,1000,
   "A VeSync (Etekcity) EU smart plug on BK7231M - the first firmware in the suite that is neither OpenBeken nor Tuya. It runs VeSync SDK v1.2.1 on the Beken SDK and needs the N-family BLE 5.1 core, which --ble-core now models alongside BK7238's 5.2: the slot clock at its 5.1 offset, the BLE block's version register the link layer asserts on, and the hardware AES engine the host needs for its random numbers. It also needed two pwm_new behaviours, since the plug captures its metering chip's pulses with PWM: the counter-read request self-clears, and interrupt status is write-1-to-clear. BLE comes up, the advertisement (Etekcity manufacturer data carrying the BLE MAC) is decoded straight out of exchange memory, and network config starts a Wi-Fi scan - the edge of what the emulator models."},
  {"BK7231N Tuya Globe Ceiling Fan (BLE-gated, SDK 2.1.17) --ble-core: Tuya beacon, TuyaMCU to Wi-Fi state","firmwares/BK7231N_Tuya_GlobeNashville_CeilingFan_TuyaMCU_2.1.17.bin",c54_args,9,420,"",0,10,c54_exp,13,c54_unexp,2,0,0,0,0,0,0,0,0,0,0,c54_tags,3,1000,
   "A paired Tuya ceiling fan from the 'BLE-gated' class - about 70% of the BK7231N dumps in FlashDumps - whose firmware brings BLE up at start-up and used to stall on the unmodelled BLE core. With --ble-core it runs real BLE init (random numbers from the modelled AES engine, the stack up, the full advertising state machine) and stages Tuya's pairing beacon: service UUID 0xA201 carrying the device's own product key, decoded from exchange memory. With the simulated MCU answering, it accepts its product record and goes one step further than the --xvr-selfclear dumps, reporting its Wi-Fi state to the MCU."},
  {"BK7231T Tuya Breaker/Leakage Switch (TuyaMCU 1.1.80) accepts raw product","firmwares/BK7231T_Tuya_Breaker_LeakageSwitch_TuyaMCU_1.1.80.bin",c55_args,8,420,"",0,10,c55_exp,12,0,0,0,0,0,0,0,0,0,0,0,0,c55_tags,2,1000,
   "A stock Tuya breaker / leakage switch, BK7231T, SDK 1.1.80. It pins down where the TuyaMCU product-info wire form changes: the other 1.1.x dump here (TMWF02, 1.1.71) accepts the JSON product record, while this one rejects it on length (prod len = 36) and needs the raw 16-byte id - so the switch happens inside the 1.1.x series, not at the 2.x boundary. It is also the first stock dump that brings its entire BLE stack up unaided (STACK INIT OK, GATT database, advertising) with no --xvr-selfclear, then completes the product handshake and moves on to the working-mode query."},
  {"BK7231N Tuya Ettroit ETWF4301 (paired) sends TuyaMCU heartbeats","firmwares/BK7231N_Tuya_Ettroit_ETWF4301_Thermostat_TuyaMCU_3.1.28.bin",c56_args,8,420,"",0,10,c56_exp,9,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A stock Tuya ETWF4301 thermostat (SDK 3.1.28) and the first non-OpenBeken image found that actually drives its MCU. Because this dump was taken after pairing, the SDK skips manufacturing test, reaches normal operation, and sends TuyaMCU heartbeats unprompted - the same 55 AA 00 00 00 00 FF frame OpenBeken emits. Also guards the physical flash-addressing model: both KV copies (current 0x1ed000, mirror 0x1cf000) must read valid with matching counts."},
  {"BK7231N Tuya BEOK Thermostat (TuyaOS 3.11.12) sends TuyaMCU heartbeats","firmwares/BK7231N_Tuya_BEOK_TOL47WIFI_Thermostat_TuyaMCU_TuyaOS_3.11.12.bin",c57_args,8,420,"",0,10,c57_exp,10,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A paired BEOK TOL47WIFI-WP-WF thermostat running TuyaOS 3.11.12 - a newer SDK line than the Ettroit case, and one that never prints \"have actived\" yet still skips manufacturing test. It opens the MCU link at 9600 baud and streams TuyaMCU heartbeats. Selected by the mirror-KV signal: paired units have written both KV copies, factory dumps leave the mirror erased."},
  {"BK7231N Tuya PJ1103C Dual-Clamp Power Meter sends TuyaMCU heartbeats","firmwares/BK7231N_Tuya_PJ1103C_DualClampPowerMeter_TuyaMCU_TuyaOS_3.11.12.bin",c58_args,8,420,"",0,10,c58_exp,10,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A paired PJ1103C dual-clamp power meter (TuyaOS 3.11.12). The MCU owns the current clamps and all metering while the Beken is only the radio, so this covers a different hardware class from the two thermostat cases. Its KV pair carries a third distinct rotation count (16), showing the physical flash-addressing model is not tied to one store state."},
  {"MathDemo Startup Command: startDriver BL0942 drives the meter UART","firmwares/OpenBK7231T_QIO_1.18.300_mathDemo_obkStartupCommand_BL0942.bin",c59_args,4,240,"",0,10,c59_exp,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "Runs OpenBeken's BL0942 energy-meter driver from an injected startup command. The driver opens UART1 at 4800 baud and speaks the BL0942 register protocol - two init writes (0xA8|addr) followed by a repeating full-packet read (0x58 0xAA) - which looks nothing like TuyaMCU framing. Proves the UART1 capture path is protocol-agnostic, not tuned to 55 AA."},
  {"BK7231N Tuya NAS-PS10 Presence Sensor sends TuyaMCU heartbeats","firmwares/BK7231N_Tuya_NAS-PS10_PresenceSensor_TuyaMCU_TuyaOS_3.8.18.bin",c60_args,8,420,"",0,10,c60_exp,10,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A paired presence/radar sensor on TuyaOS 3.8.18 - a third SDK line and hardware class. It is the flash-write guard: the device persists a 4K sector during start-up, and while page-program and sector-erase opcodes were ignored it failed its own read-back verify and rebooted in a loop. With writes implemented it reaches normal operation and drives its MCU."},
  {"BK7231N Tuya Afyeev GD4301 EV Charger accepts product, advances to working-mode","firmwares/BK7231N_Tuya_Afyeev_GD4301_EVCharger_TuyaMCU_3.1.17.bin",c61_args,8,420,"",0,10,c61_exp,6,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "An extra TuyaOS 3.x TuyaMCU dump for breadth (from FlashDumps): an EV charger, a device class the suite did not previously cover, on SDK 3.1.17. With --tuyamcu-raw and its licensed id (dsmsam7xpb3ht7rl) the simulated MCU answers in the raw form this SDK line wants, so the device accepts the product record (stored key matches our input), updates its product id, and advances to the working-mode query (0x02) and Wi-Fi link setup."},
  {"BK7231N Tuya 10-Gang Wall Switch accepts product, advances to working-mode","firmwares/BK7231N_Tuya_10Gang_WallSwitch_TuyaMCU_3.1.28.bin",c62_args,8,420,"",0,10,c62_exp,7,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "Another TuyaOS 3.x TuyaMCU dump for device-class breadth (from FlashDumps): a 10-gang wall switch on SDK 3.1.28, a multi-relay panel distinct from the meters, sensors and charger already covered. With --tuyamcu-raw and its licensed id (ni2ztksbndubd9rf) it accepts the raw product record (stored key matches our input), updates its product id, and advances to the working-mode query (0x02) and Wi-Fi link setup."},
  {"BK7231N Tuya ATORCH AT4P Meter (SDK 2.1.17) boots past BLE, accepts product","firmwares/BK7231N_Tuya_ATORCH_AT4P_EnergyMeter_TuyaMCU_2.1.17.bin",c63_args,9,420,"",0,10,c63_exp,5,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "The 2.x SDK generation, reachable only with --xvr-selfclear. This ATORCH energy meter (stock Tuya 2.1.17) wedges twice in RF/BLE init on the XVR busy-bit spins at 0x9000F8 and 0x900000 (both 'set bit 31, wait for it to clear'). --xvr-selfclear models those bits as self-clearing, walking it past both spins to the TuyaMCU link, where it runs the raw-form handshake with its licensed id (tjtigg991kvoiiqi) and advances to the working-mode query (0x02). The flag is opt-in because other dumps use 0x9000F8 the opposite way."},
  {"BK7231N Tuya PC321 3-Phase Meter (SDK 2.0.2) boots past BLE, accepts product","firmwares/BK7231N_Tuya_PC321_3PhaseMeter_TuyaMCU_2.0.2.bin",c64_args,9,420,"",0,10,c64_exp,5,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A second 2.x dump, on SDK 2.0.2 (vs ATORCH's 2.1.17): the PC321 3-phase power meter (from FlashDumps). Like ATORCH it wedges on the XVR busy spins (0x9000F8, 0x900000); --xvr-selfclear walks it past both, through BLE init to the TuyaMCU link, where its raw-form product record (licensed id gqmmtjclqb7reg5p) is accepted and it advances to the working-mode query (0x02). Adds a distinct SDK sub-version and device class."},
  {"BK7231N Tuya A03CB3S Alarm (SDK 2.1.17) boots past BLE, accepts product","firmwares/BK7231N_Tuya_A03CB3S_Alarm_TuyaMCU_2.1.17.bin",c65_args,9,420,"",0,10,c65_exp,5,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "Device-class breadth on the flag-reachable 2.x line: a security alarm/siren (A03CB3S, stock Tuya 2.1.17, from FlashDumps). Like ATORCH/PC321 it wedges on the XVR busy spins; --xvr-selfclear walks it past both to the TuyaMCU link, where its raw-form product record (licensed id ztoh9ka787lzjkpy) is accepted and it advances to the working-mode query (0x02)."},
  {"BK7231T Tuya Geeni BW223 Filament Bulb (single-colour CW PWM) boots","firmwares/BK7231T_Tuya_Geeni_BW223_FilamentBulb_CW_PWM_3000Hz_1.1.1.bin",c66_args,3,300,"",0,10,c66_exp,4,0,0,c66_pins,2,c66_pwm,2,0,0,0,0,0,0,0,0,1000,
   "A real white-only filament bulb on stock Tuya firmware. Its stored config declares pwmhz:3000, so the emulator's decoded 3000.2 Hz is checked against the device's own claim rather than against our arithmetic. Drives P24/P26 (PWM4/PWM5), the high channels no other real-firmware test covers. Its config maps c_pin:26/w_pin:24, so PWM5 is cold and PWM4 warm; the captured duties (100% and 0%) show the bulb at full cold white."},
  {"BK7231T Tuya A60 RGBCW Bulb (5-channel PWM) boots","firmwares/BK7231T_Tuya_A60_SmartBulb_RGBCWW_5ch_PWM_1000Hz.bin",c67_args,3,300,"",0,10,c67_exp,3,0,0,c67_pins,5,c67_pwm,5,0,0,0,0,0,0,0,0,1000,
   "A five-channel RGBCW bulb driving PWM0/1/2/4/5 at once, all at 1000 Hz as its stored config declares. It is the first real firmware to exercise PWM1. PWM2's duty is left unasserted on purpose: the bulb ramps it during light init, so it reads 100% when the harness stops and 54.5% in longer runs - a function of boot progress, not an invariant."},
  {"BK7231T Tuya Ledvance WW/CW Bulb (PWM3) boots","firmwares/BK7231T_Tuya_Ledvance_WW_CW_Bulb_PWM_2000Hz_1.0.8.bin",c68_args,3,300,"",0,10,c68_exp,3,0,0,c68_pins,2,c68_pwm,2,0,0,0,0,0,0,0,0,1000,
   "The only real firmware in the suite driving PWM3, which completes real-device coverage of all six PWM channels. Its period register sits at the same +0xAC offset the synthetic 1400 Hz case pins down, so a shipped product confirms that offset independently of how we drive OpenBeken. 2000 Hz, matching its stored pwmhz."},
  {"BK7231T Tuya ROOMLUX A60 RGBCW Bulb (600 Hz PWM) boots","firmwares/BK7231T_Tuya_ROOMLUX_B15515_A60_RGBCW_PWM_600Hz_1.1.2.bin",c69_args,3,300,"",0,10,c69_exp,3,0,0,c69_pins,5,c69_pwm,5,0,0,0,0,0,0,0,0,1000,
   "Chosen for its period arithmetic: 26 MHz / 600 truncates to 43333, which decodes back to 600.0046 Hz rather than 600, so the case pins the integer truncation instead of a rounded frequency. Also the first five-channel PWM_CTL cross-check - 0x110111 names exactly the channels decoded from the period registers."},
  {"BK7231T Tuya TreatLife RGB LED Strip (PWM3/4/5) boots","firmwares/BK7231T_Tuya_TreatLife_RGB_LEDStrip_PWM_1000Hz_1.0.5.bin",c70_args,3,300,"",0,10,c70_exp,3,0,0,c70_pins,3,c70_pwm,3,0,0,0,0,0,0,0,0,1000,
   "A strip controller driving r/g/b through P9/P24/P26 = PWM3/4/5 - the only real device on the upper half of the channel map, where every bulb clusters on P6/P7/P8. 1000 Hz, matching its stored pwmhz. Its oem build (strip_ty 1.0.5, Apr 2020) is the oldest light firmware in the suite."},
  {"BK7231N Tuya Feconn MR16 RGBCT Bulb (pwm_new ch3) boots into its app","firmwares/BK7231N_Tuya_Feconn_MR16_RGBCT_5ch_PWM_1000Hz_1.3.21.bin",c71_args,4,300,"",0,10,c71_exp,5,0,0,c71_pins,5,c71_pwm,5,0,0,0,0,0,0,0,0,1000,
   "Completes N-family channel coverage: its cool channel sits on P9 = pwm_new ch3, the one channel the other three N lights leave idle. Group 1's CTRL enables only sub-channel 1 here, so it also proves the decoder honours per-sub enable bits. IOT SDK 2.3.3 - the oldest N-family light firmware in the suite."},
  {"BK7231N Tuya Gleco Bulb (5ch pwm_new, 5 kHz) boots","firmwares/BK7231N_Tuya_Gleco_Bulb_5ch_PWM_5000Hz_CB2L_1.5.21.bin",c72_args,3,300,"",0,10,c72_exp,2,0,0,c72_pins,5,c72_pwm,5,0,0,0,0,0,0,0,0,1000,
   "Third N-family light at a third frequency: 5 kHz, period 5200 matching its stored pwmhz:5000. Together with Arlec (1 kHz) and the Plafon (16 kHz), the pwm_new decode is corroborated across a 16x frequency span, with channel maps running in both directions."},
  {"BK7231N Tuya LED RGB Plafon (5ch pwm_new, 16 kHz) boots","firmwares/BK7231N_Tuya_ledRGBPlafon_5ch_PWM_16000Hz_TuyaOS_3.3.43.bin",c73_args,3,300,"",0,10,c73_exp,2,0,0,c73_pins,5,c73_pwm,5,0,0,0,0,0,0,0,0,1000,
   "Five BK7231N pwm_new channels at once at 16 kHz - the highest PWM frequency in the suite and the only case exercising all three pwm_new register groups simultaneously. Period 1625 = 26 MHz / 16000 exactly, matching its stored pwmhz:16000."},
  {"BK7231N Tuya Arlec RGB Strip (PWM light, TuyaOS 3.3.44) boots","firmwares/BK7231N_Tuya_Arlec_Razer_RGBStrip_PWM_TuyaOS_3.3.44.bin",c74_args,3,300,"",0,10,c74_exp,6,0,0,c74_pins,2,c74_pwm,2,0,0,0,0,0,0,0,0,1000,
   "A paired RGB LED strip driven by hardware PWM straight from the Beken - cool on P6, warm on P8 at 1000 Hz - rather than by an external MCU, so unlike the other stock-Tuya cases it produces no UART1 traffic by design. Adds a fourth Tuya SDK generation (TuyaOS 3.3.44) and reaches normal operation after reading its protected key."},
  {"BK7238 Tuya Onforu 100W CCT Floodlight (pwm_new 2-toggle CW, 256B flash page write) boots","firmwares/BK7238_Tuya_Onforu_BDB100WF3_Floodlight_CW_PWM_1000Hz_1.0.43.bin",c75_args,4,420,"",0,10,c75_exp,13,c75_unexp,3,c75_pins,2,c75_pwm,2,0,0,0,0,0,0,c75_tags,3,1000,
   "The first BK7238 light in the suite that drives hardware PWM (TuyaOS 3.11.24, T1-U-HL 1.0.43, plaintext). Its stored Tuya config maps warm to P6 (PWM0) and cold to P26 (PWM5) at 1000 Hz, and PWM0 uses two non-zero edge toggles (T1=0x171B, T2=0x4E75 -> 54.5% duty) - the first real firmware exercising two-edge toggle replay in pwm_new_channels(). Also guards BK7238's 256-byte flash page-write FIFO at 0x00803024 (REG_FLASH_PW_CONF), without which 4K sector writes fail verification and reboot, and asserts [BLE_ADV] exchange-memory extraction on the BLE 5.2 core (UUID 0xFD50 carrying keytfesxxsqewnqy)."},
  {"BK7238 Tuya S06Pro IR + TempHum (TuyaMCU + BLE 5.2 + Wi-Fi AP) handshakes and reports AP state","firmwares/BK7238_Tuya_S06Pro_IR_TempHum_TuyaMCU_T1-3S_1.0.1.bin",c76_args,11,420,"",0,10,c76_exp,21,0,0,c76_pins,6,0,0,0,0,0,0,0,0,c76_tags,3,1000,
   "The first BK7238 TuyaMCU firmware in the suite, and a new device class: an S06Pro IR blaster + temperature/humidity station (t1_common_user_config_ir_ty 1.0.1, TuyaOS 3.11.16, T1-3S, plaintext). Exercises all three emulated peripheral/radio models on BK7238 in one run - UART1 TuyaMCU (0x00/0x01/0x02 handshake with licensed id jbe3snv4tki8oo9c), the 802.11 MAC + virtual station joining its SmartLife-C8F2 pairing AP (DHCP, ping, Tuya LAN 3.5 broadcast), and the BLE 5.2 core staging its UUID 0xFD50 beacon - and shows the firmware report Wi-Fi state 0x01 (AP config mode, 55 aa 00 03 00 01 01 04) back to the MCU once both radios are up."},
  {"BK7238 Tuya IHSENO HAZ-IR-V3 IR Blaster (paired): joins stored network, BLE 5.2 adv, TLS-PSK cloud handshake","firmwares/BK7238_Tuya_IHSENO_HAZ_IR_V3_T1-3S_0.0.5.bin",c77_args,10,420,"",0,10,c77_exp,27,c77_unexp,2,c77_pins,7,0,0,c77_pcap,8,c77_ippcap,2,0,0,c77_tags,3,1000,
   "The first paired BK7238 dump that joins its stored network ('DOM-1') as a Wi-Fi station and reaches for the Tuya cloud (oem_t1_irbox_mol_ty:0.0.5 on TuyaOS 3.8.31, T1-3S, plaintext). Because this unit is already bound ('already bind', psk30_key stored in KV), it connects straight to its MQTT-over-TLS broker (m3-eu.lifeaiot.com:8883). The simulator recovers the dump's own PSK3 internally, verifies the encrypted TLS Finished, and serves its real MQTT CONNECT, SUBSCRIBE and first QoS1 PUBLISH from the offline broker. Also covers fast-connect fallback + FCI flash persistence, bound BLE 5.2 advertising (0x4B flag byte), and the post-connect IR/button/Wi-Fi-LED GPIO state."},
  {"BK7231T Tuya BN-LINK WB2S metering plug boots its relay application","firmwares/BK7231T_Tuya_BN-LINK_Plug_WB2S_1.0.3.bin",c78_args,3,180,"",0,10,c78_exp,9,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A newly added stock BN-LINK WB2S metering-plug dump on Tuya SDK 2.0.0. It recovers its protected configuration, validates the licensed product key, initializes the dltj energy-meter service and relay channel, and brings the BLE database up successfully."},
  {"BK7231T Tuya one-gang WB3S-IPEX switch loads its hardware map","firmwares/BK7231T_Tuya_1G_Switch_WB3S-IPEX_1.0.4.bin",c79_args,3,180,"",0,10,c79_exp,10,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A newly added stock one-gang wall-switch dump on Tuya SDK 1.0.1. Its recovered board configuration is consumed by the application and maps the relay, physical button and Wi-Fi status LED to the expected pins before BLE advertising starts."},
  {"BK7231N Tuya LSC GLS A60 CBLC5 joins and authenticates cloud MQTT","firmwares/BK7231N_Tuya_LSC_GLS_A60_CBLC5_1.6.0.bin",c80_args,7,180,"",0,10,c80_exp,28,c80_unexp,2,0,0,0,0,0,0,0,0,0,0,c80_tags,3,1000,
   "A paired LSC RGB+CCT bulb on TuyaOS 3.3.44 that previously spun after HCI callback registration. Its real BK7231N selection now brings BLE up automatically, joins the stored network and exercises Tuya's older mode-2 identity-as-PSK bootstrap. The offline peer authenticates the encrypted Finished before returning server Finished, CONNACK and SUBACK, and captures the bulb's first encrypted PUBLISH without logging its credential."},
  {"BK7231N Tuya CB3S IR module reaches production-monitor endpoint","firmwares/BK7231N_Tuya_CB3S_Factory_Module.bin",c81_args,11,90,"",0,10,c81_exp,17,c81_unexp,3,0,0,0,0,c81_pcap,6,0,0,0,0,c81_tags,4,1000,
   "A factory CB3S dump carrying Tuya's IR-box application. Its real BK7231N identity selects BLE hardware automatically, with no BLE/XVR workaround flags. The stock image completes GAPM and advertising, initializes its IR services, starts its SoftAP, enters the MAC's monitor mode and reaches the same wifi_stat:1 endpoint observed on hardware. Its capture proves the production beacon/probe exchange and the firmware's periodic raw authentication frame carrying the vendor payload 'filter'."},
  {"BK7231N Tuya 72-087 CB3S switch finds factory network and enters product test","firmwares/BK7231N_Tuya_72-087.bin",c82_args,11,90,"",0,10,c82_exp,14,c82_unexp,1,0,0,0,0,c82_pcap,4,0,0,0,0,c82_tags,3,1000,
   "A factory-state CB3S one-channel switch that previously stopped at its scan request. The emulated air now supplies the exact Tuya manufacturing SSID; the stock firmware measures its RSSI and takes the real product-test branch."},
  {"BK7238 Tuya GBDQA protection switch starts metering, BLE and cloud TLS","firmwares/BK7238_GBDQA_63V_16A.bin",c83_args,6,240,"",0,10,c83_exp,28,c83_unexp,2,0,0,0,0,0,0,0,0,0,0,c83_tags,3,1000,
   "A paired BK7238 GBDQA protection switch that previously stopped at app_init. With its BLE 5.2 controller and network peer active, it synchronizes the host stack, starts the bistable relay and metering application, rejoins its stored network, authenticates with the dump's recovered PSK3, and completes MQTT CONNECT, SUBSCRIBE and its first QoS1 PUBLISH against the offline cloud broker."},
  {"BK7238 Tuya T1-U PCIe card serves its BLE/Wi-Fi pairing interfaces","firmwares/BK7238_Tuya_T1U_PCIE_RemoteBoot.bin",c84_args,8,180,"",0,10,c84_exp,24,0,0,0,0,0,0,c84_pcap,6,0,0,0,0,c84_tags,3,1000,
   "A three-channel BK7238 PCIe remote-boot card that previously stopped after loading its board map. It now completes BLE 5.2 startup, publishes its Tuya service, raises the open SmartLife pairing AP, leases an address to the virtual station and answers its ping."},
  {"BK7252N Xiongmai camera restores config and enters deep sleep","firmwares/BK7252N_Camera_7252N.bin",c85_args,3,60,"",0,10,c85_exp,15,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A newly added full-flash BK7252N camera image running RT-Thread and Xiongmai's camera stack. It initializes flash and ROMFS, brings up WLAN/RF, restores persisted device configuration, starts both buttons and the battery ADC, then follows its timed deep-sleep path."},
  {"BK7231T Tuya three-gang WB3S switch initializes every channel","firmwares/BK7231T_Tuya_3CH_Light_Switch_WB3S_1.0.7.bin",c86_args,3,90,"",0,10,c86_exp,19,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A newly added stock BK7231T three-gang WB3S wall-switch dump on Tuya SDK 1.0.2. It completes the classic BLE database and advertising path, restores its three-channel board map and verifies every relay, button and status LED before device and Wi-Fi initialization."},
  {"BL2028N Dogness D07 water fountain starts BLE and water controls","firmwares/BL2028N_Dogness_D07_WaterFountain_5.0.38.bin",c87_args,2,60,"",0,10,c87_exp,14,c87_unexp,1,0,0,0,0,0,0,0,0,0,0,c87_tags,3,1000,
   "A newly added plaintext BL2028N Dogness D07 smart water-fountain dump. With the BLE core modeled, it completes controller and host initialization, advertises its product name, starts its local AP and restores the configured water-level, pump and RGB-control state."},
  {"BK7231M VeSync Aubess BSD33 BSDOG02 plug (SDK 3.0.56) calibrates, advertises BLE and scans","firmwares/BK7231M_VeSync_Aubess_BSD33_BSDOG02_Plug_1.0.01.bin",c88_args,3,90,"",0,10,c88_exp,15,c88_unexp,2,0,0,0,0,0,0,0,0,0,0,c88_tags,3,1000,
   "A plaintext BK7231M VeSync/Aubess BSD33 (BSDOG02) smart-plug dump on Beken SDK 3.0.56. It enforces the BK7231N chip-ID check and pulses SARADC_ADC_CTRL_CFG bit 8 during RF calibration before mounting its FlashDB KV partitions, bringing up BLE 5.1 advertising, configuring pwm_new metering capture and starting a Wi-Fi scan."},
  {"BK7231M VeSync WHOGPLUG Plug 1.0.02 (SDK 3.0.56) calibrates, advertises BLE and scans","firmwares/BK7231M_VeSync_GreenSun_WHOGPLUG_Plug_1.0.02.bin",c89_args,3,90,"",0,10,c89_exp,16,c89_unexp,2,0,0,0,0,0,0,0,0,0,0,c89_tags,3,1000,
   "The 1.0.02 firmware revision of the VeSync/GreenSun WHOGPLUG EU smart plug on Beken SDK 3.0.56. Unlike 1.0.01, this build enforces the BK7231N chip-ID check and exercises the SARADC_ADC_CTRL_CFG trigger during RF calibration before starting FlashDB, BLE 5.1 advertising, pwm_new pulse capture and Wi-Fi scanning."},
  {"BK7231M ChinaMobile HeJiaQin X1S plug joins JX-ChanCe-M2 and starts BL0937 calibration","firmwares/BK7231M_ChinaMobile_HeJiaQin_HuiJu_X1S_Plug_CB2S_1.0.4.bin",c90_args,7,90,"",0,10,c90_exp,18,c90_unexp,2,0,0,0,0,0,0,0,0,0,0,c90_tags,4,1000,
   "A plaintext BK7231M China Mobile HeJiaQin (HuiJu X1S) CB2S metering plug on Beken SDK 3.0.56. With the BK7231N chip identity and SARADC_ADC_CTRL_CFG trigger modeled, it completes RF calibration and BLE 5.1 startup, loads its factory CMEI/SN burninfo, associates with the virtual 'JX-ChanCe-M2' factory AP over DHCP, and enters BL0937B power-meter calibration."},
  {"BK7231M SparkleIoT XH-CB2S Matter mini switch boots into Matter init","firmwares/BK7231M_SparkleIoT_XH-CB2S_Mini_Switch.bin",c91_args,3,60,"",0,10,c91_exp,6,c91_unexp,1,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A 4 MB plaintext BK7231M dump from a SparkleIoT XH-CB2S mini switch built on Quectel's Matter SDK (svc_quec_matter) and Beken SDK 3.0.56. It boots the Beken RTOS on UART1, enters the Matter application ('going Chip') and runs its network-config and log-flag initialization on UART2."},
  {"BK7231Q Tuya MOES Relay 1.1.3-2 completes 3x-reset WDT reboot and enters EZ pairing","firmwares/BK7231Q_Tuya_MOES_Relay_WA2_1.1.3-2.bin",c92_args,3,90,"",0,10,c92_exp,14,0,0,0,0,0,0,0,0,0,0,0,0,c92_tags,2,1000,
   "A BK7231Q MOES WA2 one-gang relay dump captured after two fast power cycles (fsw_cnt_key=2). On first boot it reaches the three-cycle factory-reset threshold, clears the counter in flash and reboots through the hardware watchdog; on the second boot it reads back fsw_cnt_key=0, drains the SOC_BK7231 SARADC FIFO (bit 28), and brings up EZ-mode Wi-Fi pairing."},
  {"BK7231Q Tuya MOES Relay 1.0.6 (SDK 2.0.0) scans factory SSID and enters product test","firmwares/BK7231Q_Tuya_MOES_Relay_WA2_1.0.6.bin",c93_args,5,90,"",0,10,c93_exp,14,0,0,0,0,0,0,0,0,0,0,0,0,c93_tags,1,1000,
   "A factory-state BK7231Q MOES WA2 one-gang relay dump on Tuya IoT SDK 2.0.0 (version 1.0.6). With SOC_BK7231 SARADC FIFO_EMPTY bit 28 reported when drained, it completes SARADC calibration and Wi-Fi scanning against the virtual tuya_mdev_test1 AP, measures RSSI -45 dB, and enters product test mode."},
  {"BK7231Q Tuya MOES Relay 1.0.4 (SDK 2.0.0) maps GPIO and enters product test","firmwares/BK7231Q_Tuya_MOES_Relay_WA2_1.0.4.bin",c94_args,5,90,"",0,10,c94_exp,13,0,0,c94_pins,2,0,0,0,0,0,0,0,0,c94_tags,1,1000,
   "The 1.0.4 factory firmware of the BK7231Q MOES WA2 one-gang relay on Tuya IoT SDK 2.0.0 (rst_cnt:1). It configures P18 as an active-high relay output and P6 as a pull-up button input, completes SARADC calibration, scans the virtual tuya_mdev_test1 AP, and enters product test mode."},
  {"BK7231Q Tuya MOES Relay 1.0.4-2 (rst_cnt:2) drives relay high and scans","firmwares/BK7231Q_Tuya_MOES_Relay_WA2_1.0.4-2.bin",c95_args,1,90,"",0,10,c95_exp,13,0,0,c95_pins,3,0,0,0,0,0,0,0,0,0,0,1000,
   "A second BK7231Q MOES WA2 one-gang relay unit on firmware 1.0.4 captured with rst_cnt:2. Before manufacturing-test override, its default power-on state drives P18 high (relay closed), configures P6 as a pull-up button input and P7 low, and initiates a Wi-Fi scan."},
  {"BK7231U C-Chip C-8133U AliOS LED driver (reset_counts:3) configures 5ch PWM and activates reset","firmwares/BK7231U_C-Chip_C-8133U_LED_Driver_AliOS_Reset3.bin",c96_args,3,90,"",0,10,c96_exp,10,0,0,c96_pins,5,0,0,0,0,0,0,0,0,0,0,1000,
   "The first BK7231U dump in the suite: a C-Chip C-8133U 5-channel RGB+CCT LED driver running AliOS Things. It restores its persisted light parameters (50% brightness, 4500 K, Red=255), configures all five PWM pads (P7, P8, P9, P24, P26) into second-function mode, initiates a Wi-Fi scan, and fires its three-cycle reset handler."},
  {"BK7231U C-Chip C-8133U AliOS LED driver (reset_counts:1) configures 500 Hz PWM without resetting","firmwares/BK7231U_C-Chip_C-8133U_LED_Driver_AliOS_Reset1.bin",c97_args,3,90,"",0,10,c97_exp,8,c97_unexp,1,c97_pins,5,c97_pwm,1,0,0,0,0,0,0,0,0,1000,
   "A companion BK7231U C-Chip C-8133U AliOS Things dump captured with reset_counts:1 in flash. It configures all five PWM pads (P7, P8, P9, P24, P26), programs PWM0 for a 500 Hz period (0xCB20 ticks at 26 MHz), starts its Wi-Fi scan, and confirms the 3x reset handler is not triggered when reset_counts is below 3."},
  {"BK7231U C-Chip C-8133U RT-Thread 3.1.0 firmware mounts FAL partitions","firmwares/BK7231U_C-Chip_C-8133U_LED_Driver_RTThread.bin",c98_args,3,90,"",0,10,c98_exp,9,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "An RT-Thread 3.1.0 build from the C-Chip C-8133U BK7231U directory (contrasting with the AliOS Things dumps from the same board). It completes RF calibration, brings up lwIP 2.0.2, registers station and soft-AP WLAN interfaces, and initializes the RT-Thread Flash Abstraction Layer (FAL V0.4.0) partition table."},
  {"BK7231U C-Chip CC8000 HLK-B30 dev board boots RT-Thread 3.1.0 and FAL partitions","firmwares/BK7231U_C-Chip_CC8000_HLK-B30_DevBoard.bin",c99_args,3,90,"",0,10,c99_exp,10,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A BK7231U dump from a C-Chip CC8000 / Hi-Link HLK-B30 development board running RT-Thread 3.1.0. It reads its factory RF calibration parameters from flash (temp 280, crystal offset 48), initializes lwIP 2.0.2, registers station and soft-AP WLAN interfaces, and mounts the RT-Thread FAL V0.4.0 partition table."},
  {"BK7231U Arnoo AiDot RGBW A19 bulb (Leedarson CC8000 2.0.4.51) enters Wi-Fi monitor pairing","firmwares/BK7231U_Arnoo_AiDot_RGBW_A19_Leedarson_CC8000_2.0.4.51.bin",c100_args,4,90,"",0,10,c100_exp,4,0,0,c100_pins,4,0,0,0,0,0,0,0,0,c100_tags,1,1000,
   "A BK7231U dump from an Arnoo AiDot RGBW A19 (2nd Gen, light.A001126) smart bulb on a Leedarson CC8000 module (V2.3.72::V2.04.51). It opens the Wi-Fi MAC RX filter (0x7fffffde) into monitor/sniffing mode, prints its CC8000 version banner, configures P2, P3, P20 and P21 as pull-up inputs, and starts the RivieraWaves rw_main task."},
  {"BK7231U Arnoo Winees AiDot RGBW A60 bulb (Leedarson CC8000 2.0.3.47) hosts SmartLight-5783 AP and answers ping","firmwares/BK7231U_Arnoo_Winees_AiDot_RGBW_A60_Leedarson_CC8000_2.0.3.47.bin",c101_args,5,90,"",0,10,c101_exp,8,0,0,c101_pins,5,0,0,0,0,0,0,0,0,c101_tags,1,1000,
   "An earlier V2.2.18::V2.03.47 firmware dump from an Arnoo Winees WS010078261 AiDot RGBW A60 (2nd Gen, light.A001139) smart bulb on a Leedarson CC8000 BK7231U module. It starts its open pairing Soft-AP 'SmartLight-5783' (1c:d6:bd:d4:57:82, channel 7), assigns 192.168.10.2/24 to the emulated station from router 192.168.10.1 via DHCP, and replies to ARP and ICMP ping while configuring P2, P3, P20, P21 and P23."},
  {"BK7231U Arnoo Winees AiDot RGBW A60 bulb (Leedarson CC8000 2.0.4.51) stays in Wi-Fi monitor pairing","firmwares/BK7231U_Arnoo_Winees_AiDot_RGBW_A60_Leedarson_CC8000_2.0.4.51.bin",c102_args,5,90,"",0,10,c102_exp,4,c102_unexp,1,c102_pins,4,0,0,0,0,0,0,0,0,c102_tags,1,1000,
   "The newer V2.3.72::V2.04.51 firmware dump from the Arnoo Winees WS010078261 AiDot RGBW A60 (2nd Gen, light.A001139) smart bulb on a Leedarson CC8000 BK7231U module. Unlike the earlier 2.0.3.47 build (which immediately switches from monitor mode to its SmartLight Soft-AP), 2.0.4.51 remains in Wi-Fi MAC monitor/sniffing mode (RX filter 0x7fffffde) after bringing up rw_main and configuring P2, P3, P20 and P21."},
  {"BK7252N CY365 Car IP camera 1.7.0 (--xvr-selfclear) completes RF cal and probes as station","firmwares/BK7252N_CY365_Car_IP_EEE-249530-GDVHL_1.7.0.bin",c103_args,6,90,"",0,10,c103_exp,3,0,0,c103_pins,9,0,0,0,0,0,0,0,0,c103_tags,2,1000,
   "A BK7252N dump from a CY365 Car IP camera (EEE-249530-GDVHL, firmware 1.7.0). Without --xvr-selfclear it spins at 0x0d4ba4 waiting for bit 31 of XVR register 0x00900000 to self-clear. With --xvr-selfclear it completes RF calibration, configures SDIO pads P14..P19 (0x378), button P7 (0x3C) and output P5 (0x02), transitions the Wi-Fi MAC RX filter from monitor mode (0x7fffffde) to normal reception (0x3503848c), and begins active station probing."},
  {"BK7252N INO-A13N-V2.2 square camera (3.00.21.01_250512) restores RTC 2025-3-1 and enters deep sleep","firmwares/BK7252N_Cam_INO-A13N-V2.2_Square_3.00.21.01.bin",c104_args,3,60,"",0,10,c104_exp,13,0,0,c104_pins,14,0,0,0,0,0,0,0,0,0,0,1000,
   "A BK7252N dump from an INO-A13N-V2.2 square battery camera running an earlier xciot build (sw_ver=3.00.21.01_250512 vs 250723). It initializes FAL and ROMFS, configures SDIO pads P14..P19 (0x378), buttons P12/P13 (0x3C) and drives P22/P23 low (0x00) on sleep entry after reading SARADC channel 0 (battary_status:139), xdev_opt (magic=11223344) and setting the local clock to 2025-3-1 8:1:50."},
  {"BK7252N Pinmei A9 camera (BKFIL, power on) loads xciot identity and spawns xrpc threads","firmwares/BK7252N_Pinmei_A9_Camera_BKFIL_3.00.42.01.bin",c105_args,3,60,"",0,10,c105_exp,20,0,0,c105_pins,14,0,0,0,0,0,0,0,0,0,0,1000,
   "A BK7252N dump from a Pinmei A9 camera captured via BKFIL with power status on in flash. Unlike the powered-off camera dumps (which enter deep sleep), this unit keeps P22 and P23 driven high (0x02), starts xled_start, reads its [iot] device credentials (did PPH0A906615F0207B0, scode 760441, prod.glbs.xcthings.com, MAC c8:47:8c:4c:81:2c), initializes AVSDK 3.00.42.01 and OSAL 1.00.01.64, and spawns the vtask_wor, unpack, xrpc.r and xrpc.w worker threads."},
  {"BK7252N Pinmei A9 camera (SPI NeoProgrammer dump, RTC 8:3:05) loads xciot identity and spawns xrpc threads","firmwares/BK7252N_Pinmei_A9_Camera_SPI_3.00.42.01.bin",c106_args,3,60,"",0,10,c106_exp,19,0,0,c106_pins,14,0,0,0,0,0,0,0,0,0,0,1000,
   "An external SPI programmer dump (NeoProgrammer EB6015) of the Pinmei A9 BK7252N camera captured at a later flash RTC timestamp (2025-03-01 00:03:05 UTC). It initializes FAL and ROMFS, restores its persisted [iot] credentials and MAC c8:47:8c:4c:81:2c, keeps P22 and P23 driven high (0x02), and brings up AVSDK 3.00.42.01 and the xrpc worker threads."},
  {"BK7252N XCThings Linklemo A9 INO-IPC-V2.3 camera loads PPH0A90E1F42CF3350 identity and starts vtask","firmwares/BK7252N_XCThings_Linklemo_A9_INO-IPC-V2.3.bin",c107_args,3,60,"",0,10,c107_exp,14,0,0,c107_pins,14,0,0,0,0,0,0,0,0,0,0,1000,
   "A BK7252N dump from an XCThings Linklemo A9 camera built on the INO-IPC-V2.3 (Blue PCB) board. It initializes FAL and ROMFS, configures SDIO pads P14..P19 (0x378), buttons P12/P13 (0x3C) and outputs P22/P23 (0x02), reads its distinct [iot] device credentials (did PPH0A90E1F42CF3350, scode 574818, MAC c8:47:8c:70:a1:b2) from flash, and starts AVSDK 3.00.42.01, OSAL 1.00.01.64 and the vtask_wor worker thread."},
  {"BK7252 Linklemo XCThings HA15 IN-A15-V1.0 camera (12.12.16) boots FAL/ROMFS, jpegdec and vbat_detect_test","firmwares/BK7252_Linklemo_XCThings_HA15_IN-A15-V1.0_12.12.16.bin",c108_args,3,60,"",0,10,c108_exp,14,0,0,c108_pins,3,0,0,0,0,0,0,0,0,0,0,1000,
   "A BK7252 (QFN48) dump from a Linklemo XCThings HA15 camera on the IN-A15-V1.0 (INNOA15 Blue PCB) board, firmware 12.12.16 (RT-Thread 3.1.0 built Apr 1 2024). It completes RF calibration (tssi_th b-145/g-130, BLE ch0/19/39 txpwr table), mounts the FAL partition table and ROMFS, creates the jpegdec thread, runs vbat_detect_test on SARADC channel 0, and initializes tc_entity_init."},
  {"BK7252 Naxclow MiniCam A11-A9 B-V2.7 (1.1.20, 48P) boots audio, WLAN and Naxclow camera app","firmwares/BK7252_Naxclow_MiniCam_A11-A9_B-V2.7_240426.bin",c109_args,3,60,"",0,10,c109_exp,14,0,0,c109_pins,5,0,0,0,0,0,0,0,0,0,0,1000,
   "A BK7252 (QFN48) dump from a Naxclow MiniCam on the A11-A9 B-V2.7 (240426) board (RT-Thread 3.1.0 built Aug 7 2024). It completes RF calibration (tssi_th b-130/g-123), initializes lwIP 2.0.2 and the station/soft-AP WLAN interfaces, configures audio DAC/ADC volumes (65/80) and board GPIOs, and launches com.naxclow.Camera_v0-1.1.20 for DevType A9_V2.7_ALERT_A_48P."},
  {"BK7252 A9 camera 20211124 (QFN68, SDK 3.0.33, UART1 log) calibrates RF and initializes WLAN","firmwares/BK7252_A9_Camera_20211124_SDK_3.0.33.bin",c110_args,3,60,"",0,10,c110_exp,17,0,0,c110_pins,4,0,0,0,0,0,0,0,0,0,0,1000,
   "A BK7252 (QFN68) dump from an A9 camera on Beken SDK 3.0.33 (OSK R-3.0.22, IP W4-3.0.33-P0, RT-Thread 3.1.0 built May 14 2022) that routes its console log to UART1 instead of UART2. It completes RF calibration (tssi b-125/g-115, xtal_cali 25), initializes lwIP 2.0.2 and both WLAN interfaces, runs drv_pm_init, and reaches msh />."},
  {"BK7252 Naxclow A9 QFN68 camera (20230509, 1.1.20) boots audio, WDT and reads device ID 0800C0140548","firmwares/BK7252_Naxclow_A9_Camera_Redacted_1.1.20_20230509.bin",c111_args,3,60,"",0,10,c111_exp,19,0,0,c111_pins,5,0,0,0,0,0,0,0,0,0,0,1000,
   "A BK7252 (QFN68) dump from a Naxclow A9 camera (RT-Thread 3.1.0 built May 9 2023). Unlike the Aug 2024 QFN48 MiniCam build, this earlier QFN68 image reports 'No TLV header found in flash', calibrates RF with tssi_th b-120/g-115, starts its watchdog device, registers the station and soft-AP WLAN interfaces, configures audio DAC/ADC volumes (65/80), initializes tc_entity_init, and reads device ID 0800C0140548 from flash."},
  {"BK7252 T99 MiniCam (QFN48, GC0311) probes SDIO and hosts EEE-196629-TLESS AP","firmwares/BK7252_T99_MiniCam_GC0311.bin",c112_args,5,60,"",0,10,c112_exp,8,0,0,c112_pins,13,0,0,0,0,0,0,0,0,0,0,1000,
   "A BK7252 (QFN48) dump from a T99 MiniCam with a GC0311 DVP sensor (CYCAM SDK). The release build silences UART console output via rt_console_set_device(\"no_uart\"), probes for an SD card on P14..P19 through the 0x00802d00 SDCard controller, checks SCTRL_USB_PLUG_WAKEUP (0x00800084 bit 21) and SARADC, and brings up the open 'EEE-196629-TLESS' Soft-AP at 192.168.169.100, assigning 192.168.169.101 over DHCP and replying to ARP and ICMP ping."},
  {"BK7252 A10 CYCAM (QFN68, VM101P_v85_0729) probes SDIO and hosts PIXA-077260-DNGUV AP","firmwares/BK7252_A10_CYCAM_VM101P_v85_0729.bin",c113_args,5,60,"",0,10,c113_exp,8,0,0,c113_pins,13,0,0,0,0,0,0,0,0,0,0,1000,
   "A BK7252 (QFN68) dump from an A10 CYCAM mini camera (LV-MW102P-V1.3 board, VM101P_v85_0729). It silences UART console output via rt_console_set_device(\"no_uart\"), probes for an SD card on P14..P19 via the 0x00802d00 SDCard controller, checks SCTRL_USB_PLUG_WAKEUP (0x00800084 bit 21) and SARADC, and brings up the open 'PIXA-077260-DNGUV' Soft-AP (90:91:64:08:54:80) at 192.168.169.100, assigning 192.168.169.101 over DHCP and replying to ARP and ICMP ping."},
  {"BK7252 Pix-Link A9 camera (QFN48, RW101P_v19_1010) probes SDIO and hosts PIXA-180431-XLPNL AP","firmwares/BK7252_A9_Pix-Link_CYCAM_RW101P_v19_1010.bin",c114_args,5,60,"",0,10,c114_exp,8,0,0,c114_pins,9,0,0,0,0,0,0,0,0,0,0,1000,
   "A BK7252 (QFN48) dump from a Pix-Link A9 mini camera (CYCAM_RW101P_v19_1010). It silences UART console output via rt_console_set_device(\"no_uart\"), probes for an SD card on P14..P19 via the 0x00802d00 SDCard controller, configures P7 as a pull-up input (0x3C), and brings up the open 'PIXA-180431-XLPNL' Soft-AP (90:91:64:0b:56:77) at 192.168.169.100, assigning 192.168.169.101 over DHCP and replying to ARP and ICMP ping."},
  {"BL2028N Dreo WH517S wall heater (PAI-053, 3.4.3) boots BLE stack and polls MCU","firmwares/BL2028N_Dreo_Wall_Heater_WH517S_PAI-053_3.4.3.bin",c115_args,3,60,"",0,10,c115_exp,15,0,0,c115_pins,4,0,0,0,0,0,0,0,0,c115_tags,4,1000,
   "A plaintext BL2028N dump from a Dreo WH517S wall heater (PAI-053 module, firmware 3.4.3 built Oct 23 2024 on SDK 3.0.46). It completes RF calibration, brings up the RW BLE 5.x stack (gapm_cmp_evt:BLE_STACK_OK), prints its DREOsh17 product banner and license serial number, transmits 55 AA protocol frames to the heater MCU over UART1, and enters Wi-Fi station scanning."},
  {"BL2028N Dreo WH517S wall heater (PAI-053, 3.8.7) boots BLE stack and polls MCU","firmwares/BL2028N_Dreo_Wall_Heater_WH517S_PAI-053_3.8.7.bin",c116_args,3,60,"",0,10,c116_exp,15,0,0,c116_pins,4,0,0,0,0,0,0,0,0,c116_tags,4,1000,
   "A plaintext BL2028N dump from a Dreo WH517S wall heater (PAI-053 module) running firmware 3.8.7 (built Oct 13 2025 on SDK 3.0.46, MAC 00:1c:c2:a6:a8:07, XTAL=86). It completes RF calibration, brings up the RW BLE 5.x stack (gapm_cmp_evt:BLE_STACK_OK), prints its DREOsh17 product banner and license serial number, transmits 55 AA protocol frames to the heater MCU over UART1, and enters Wi-Fi station scanning."},
  {"BL2028N XiaoMei WP8621N fish feeder (AliOS Things, WP8621NK_03) boots BLE and LinkKit","firmwares/BL2028N_XiaoMei_WP8621N_Fish_Feeder_WP8621NK_03.bin",c117_args,3,60,"",0,10,c117_exp,18,0,0,c117_pins,11,0,0,0,0,0,0,0,0,c117_tags,3,1000,
   "A plaintext BL2028N dump from a XiaoMei WP8621N automatic fish feeder (WIFI002_V20 board, APP: WP8621NK_03, built on SDK 3.0.65 with AliOS Things AOS-R-1.3.4 and LinkKit 2.3.0_FY_1.6.0). It exercises the boot_handlers.S `ldmfd sp!, {r0, r1}^` (0xe8fd0003) exception entry normalization, completes RF calibration, initializes the RW BLE 5.x stack, loads its VSON LinkKit credentials (product_key a1c3mFpE63X, MAC 86:21:02:FC:22:41), turns on the status LED, and reports calibration = 1365."},
  {"BK7238 Gledopto GL-SPI-206W SPI LED controller (TuyaOS 3.11.11) configures SPI pads and fast-connects","firmwares/BK7238_Gledopto_GL-SPI-206W_LED_Controller.bin",c118_args,9,90,"",0,10,c118_exp,13,0,0,c118_pins,8,0,0,0,0,0,0,0,0,0,0,1000,
   "A plaintext BK7238 QIO dump from a Gledopto GL-SPI-206W addressable SPI LED strip controller running TuyaOS 3.11.11 (built Mar 5 2025 for T1). It runs the Tuya OTA bootloader check (tuya_upgrade_main [255]), completes RF calibration, configures SPI pads P14..P17 (0x348/0x34A) for the addressable pixel strip, loads MAC 80:64:7c:99:dd:7f, and initiates a fast-connect scan for its stored 'DLINK_FastNet' network."},
  {"BK7238 BrizLabs Fairy LED RGB (SDK 3.0.65, 1.0.1) configures SPI/GPIO and scans for tuyatest","firmwares/BK7238_BrizLabs_Fairy_LED_RGB_1.0.1.bin",c119_args,3,60,"",0,10,c119_exp,14,0,0,c119_pins,11,0,0,0,0,0,0,0,0,c119_tags,3,1000,
   "A plaintext BK7238 dump from a BrizLabs Fairy RGB LED string light (sxebObqQRfXmgU, firmware 1.0.1 on Beken SDK 3.0.65 / OSK F-3.0.40). It completes RF calibration (lpf_i/q 74/76, xtal 23), configures SPI pads P14..P17 (0x48), button P6 (0x3C) and outputs P22/P28, loads its factory test credentials ('tuyatest' / 'fakepassword', MAC 44:4a:d6:0c:9e:14), computes its PSKC, and enters Wi-Fi station scanning."},
  {"BK7238 DX-Smart DX-WF24A AT module prints 'power on' and advertises 'WF24-BLE'","firmwares/BK7238_DX-Smart_DX-WF24A_AT.bin",c120_args,3,60,"",0,10,c120_exp,4,0,0,c120_pins,6,0,0,0,0,0,0,0,0,c120_tags,3,1000,
   "A plaintext BK7238 dump from a DX-Smart DX-WF24A AT-command Wi-Fi + BLE combo module. It suppresses SDK boot messages, prints 'power on', drives P6 and P26 low (0x00), initializes the BLE 5.2 controller, and stages its 'WF24-BLE' Complete Name advertisement in exchange memory at 0x0040e480 +0x0a54."},
  {"BK7238 Zengge ZJ21412 BP101 RGBCCT bulb (BP5758D I2C, 20240425) boots OS 8.43.0 and calibrates","firmwares/BK7238_Zengge_ZJ21412_BP101_RGBCCT_Bulb.bin",c121_args,3,60,"",0,10,c121_exp,12,0,0,c121_pins,6,0,0,0,0,0,0,0,0,c121_tags,3,1000,
   "A plaintext BK7238 dump from a Zengge AK001-ZJ21412 RGBCCT bulb (35_243_20240425_ZG-BK38-BP101 on Beken SDK 3.0.70 c063598, Zengge OS 8.43.0) using a BP5758D 5-channel I2C constant-current LED driver on P24 and P26 (both 0x02). It prints its Zengge banner and BP5758D channel currents (8/8/8/30/30), updates PWRRCD store index 23, and completes RF calibration."},
  {"BK7238 Zengge ZJ21413 Symphony USB SPI LED (OS 8.42.4, 20240423) configures SPI and advertises IOTWF8A9","firmwares/BK7238_Zengge_ZJ21413_Symphony_USB_SPI_LED.bin",c122_args,3,60,"",0,10,c122_exp,12,0,0,c122_pins,10,0,0,0,0,0,0,0,0,c122_tags,3,1000,
   "A plaintext BK7238 dump from a Zengge AK001-ZJ21413 Symphony USB SPI addressable LED controller (77_24_20240423_ZG-BK on Beken SDK 3.0.70 ecb1fb4, Zengge OS 8.42.4). It completes RF calibration, configures SPI pads P14..P17 (0x48) and inputs P6/P8 (0x3C), brings up the BLE 5.2 controller, stages its 'IOTWF8A9' Complete Name advertisement in exchange memory, and initializes the BRMT remote task."},
  {"BK7238 Zengge W43-M5 V1.2 ceiling light (OS 9.7.1, 20250724) configures PWM/SPI and advertises IOTWF615","firmwares/BK7238_Zengge_Ceiling_Light_W43-M5_V1.2.bin",c123_args,3,60,"",0,10,c123_exp,13,0,0,c123_pins,10,0,0,0,0,0,0,0,0,c123_tags,3,1000,
   "A plaintext BK7238 dump from a Zengge W43-M5 V1.2 ceiling light (AK001-ZJ21412, 3D_23_20250724_ZIL-BK38-M5-12 on Beken SDK 3.0.70 17e8b67, Zengge OS 9.7.1). It completes RF calibration, configures PWM pads P6/P8 (0x48) and SPI pads P14..P17 (0x48), brings up the BLE 5.2 controller, stages its 'IOTWF615' Complete Name advertisement in exchange memory, and checks its FOTA firmware version record."},
  {"BK7238 eWeLink W105SE0X door/window sensor (1.0.4, SDK 3.0.70) configures reed/button GPIOs and scans","firmwares/BK7238_eWeLink_DoorWindow_W105SE0X_1.0.4.bin",c124_args,3,60,"",0,10,c124_exp,8,0,0,c124_pins,8,0,0,0,0,0,0,0,0,0,0,1000,
   "A plaintext BK7238 dump from an eWeLink (CoolKit) door/window sensor (CK-BK7238-W105SE0X-01-DOOR-HB, firmware 1.0.4 built May 16 2025 on Beken SDK 3.0.70 / OSK F-3.0.44). It prints its CoolKit version banner and DeviceMac d02704434298, configures reed-switch and tamper/button inputs P7, P22 and P23 with pull-ups (0x3C) and drives status output P21 low (0x00), completes RF calibration, and enters Wi-Fi station scanning."},
  {"BK7238 Zengge Ledytech XGD-NZ-002 WB43-M5 V1.1 (OS 9.7.1, 20250107) configures PWM/SPI and advertises IOTWF829","firmwares/BK7238_Zengge_Ledytech_XGD-NZ-002_WB43-M5_V1.1.bin",c125_args,3,60,"",0,10,c125_exp,13,0,0,c125_pins,10,0,0,0,0,0,0,0,0,c125_tags,3,1000,
   "A plaintext BK7238 dump from a Zengge Ledytech XGD-NZ-002 ceiling light on the WB43-M5 V1.1 board (AK001-ZJ21412, 3D_22_20250107_ZIL-BK38 on Beken SDK 3.0.70 434b394, Zengge SDK 2.9.0 / OS 9.7.1). It completes RF calibration, configures PWM pads P6/P8 (0x48) and SPI pads P14..P17 (0x48), brings up the BLE 5.2 controller, stages its 'IOTWF829' Complete Name advertisement in exchange memory, and initializes the BRMT remote task."},
  {"BK7231T Tuya Arlec SSWF01G fan speed controller (1.0.5, SDK 2.0.0) configures fan relays and BLE","firmwares/BK7231T_Tuya_Fan_Speed_Controller_SSWF01G_1.0.5.bin",c126_args,9,90,"",0,10,c126_exp,13,0,0,c126_pins,12,0,0,0,0,0,0,0,0,0,0,1000,
   "A stock BK7231T dump from an Arlec / Tuya SSWF01G-WIFI ceiling fan speed controller (oem_bk7231s_fan_control_arlec:1.0.5 on TUYA IOT SDK 2.0.0 BS:30.06). It recovers its protected flash configuration, drives its fan-speed relay outputs (P6, P7, P9, P10 high 0x02; P26 low 0x00) and button inputs (P1, P8, P14 pull-up 0x3C), updates its bound BLE localkey, and completes GAPM initialization."},
  {"BK7231T Tuya Genio Nebular Projector I003323 (5ch 1000 Hz PWM + laser, 1.1.2) boots","firmwares/BK7231T_Tuya_Genio_Nebular_Projector_I003323_1.1.2.bin",c127_args,3,60,"",0,10,c127_exp,13,0,0,c127_pins,13,c127_pwm,5,0,0,0,0,0,0,0,0,1000,
   "A stock BK7231T dump from a Mirabella Genio Nebular Star Projector (I003323, oem_bk7231t_light3_laser_nanxin:1.1.2 on TUYA IOT SDK 1.0.2). It configures five 1000 Hz hardware PWM channels (P6/P8/P9/P24/P26 = PWM0/2/3/4/5, period 0x6590 = 26000 ticks, with PWM0 at 50% duty 0x32C8 and PWM2..4 at 100% duty) alongside GPIO output P7 (0x00) and button input P14 (0x3C)."},
  {"BK7231T Tuya MOES MS-102 garage door opener (WB2S, 1.1.2) configures contact/relay GPIOs and BLE","firmwares/BK7231T_Tuya_MOES_GarageDoor_MS-102_WB2S_1.1.2.bin",c128_args,9,90,"",0,10,c128_exp,12,0,0,c128_pins,10,0,0,0,0,0,0,0,0,0,0,1000,
   "A stock BK7231T dump from a MOES MS-102 smart garage door controller (oem_bk7231s_garage_door:1.1.2 on TUYA IOT SDK 1.0.2, WB2S module). It recovers its protected flash configuration, configures the door-contact sensor input P26 with a pull-down (0x2C), button P6 with a pull-up (0x3C) and relay/LED outputs P7 (0x02) and P24 (0x00), completes device_ctrl_init, and updates its BLE advertisement."},
  {"BK7231T Tuya qcsc400w-v5t smart curtain switch (WB3S, 1.6.0) loads 3-sub-channel curtain config","firmwares/BK7231T_Tuya_Smart_Curtain_qcsc400w-v5t_WB3S_1.6.0.bin",c129_args,3,60,"",0,10,c129_exp,14,0,0,c129_pins,10,0,0,0,0,0,0,0,0,0,0,1000,
   "A stock BK7231T dump from a Tuya qcsc400w-v5t motorized smart curtain switch (oem_bk7231s_curtain_switch:1.6.0 on TUYA IOT SDK 1.0.2, WB3S module). It recovers its protected user_param_key, parses and logs its three-sub-channel curtain relay and button mapping (open/stop/close buttons on P8/P10/P24, motor relays on P7/P6 with 200 ms interlock and drive times), and finishes curtain_json_init and dev_event_json_init."},
  {"BK7231T Tuya MCB238 DDSU238 DIN-rail breaker (WB3S, 1.1.8) starts BLE advertising and loads bistable pConfig","firmwares/BK7231T_Tuya_MCB238_Breaker_DDSU238_WB3S_1.1.8.bin",c130_args,3,60,"",0,10,c130_exp,9,0,0,c130_pins,6,0,0,0,0,0,0,0,0,0,0,1000,
   "A stock BK7231T dump from a Tuya MCB238 / DDSU238 DIN-rail circuit breaker (oem_bk7231s_rnd_switch:1.1.8 on TUYA IOT SDK 1.0.2, WB3S module). It brings up the BLE GATT database and advertising (STACK INIT OK, CREATE DB SUCCESS, appm start advertising), completes mf_init, and logs its recovered bistable-relay pConfig (rl_on1_pin:9, rl_off1_pin:8, bt1_pin:26, led1_pin:6, netled_pin:24)."},
  {"BK7231T Tuya TreatLife SS02 3-way switch (1.1.3, SDK 1.0.8) configures 3-way GPIOs and BLE","firmwares/BK7231T_Tuya_TreatLife_SS02_3-Way_Switch_1.1.3.bin",c131_args,9,90,"",0,10,c131_exp,9,0,0,c131_pins,11,0,0,0,0,0,0,0,0,0,0,1000,
   "A stock BK7231T dump from a TreatLife SS02 3-way smart wall switch (oem_bk7231s_double_switch_rmd:1.1.3 on TUYA IOT SDK 1.0.8). It configures 3-way traveler/button inputs P6, P26 and P28 with pull-ups (0x3C) and relay/LED outputs P8 (0x02), P9 (0x00) and P24 (0x00), brings up the BLE GATT database (STACK INIT OK, CREATE DB SUCCESS), and starts advertising."},
  {"BL2028N Zemismart Matter ZML2 RGBCT bulb (UAM026, 1.21) prints Matter SetupQRCode and drives pwm_new","firmwares/Zemismart_Matter_ZML2_RGBCT_CSA23291MAT40804-24_UAM026_1.21.bin",c132_args,4,60,"",0,10,c132_exp,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A plaintext BL2028N dump from a Zemismart ZML2 Matter over Wi-Fi RGBCT smart bulb (CSA23291MAT40804-24, Uascent UAM026 v1.21). It initializes the Matter commissioning stack, prints its SetupQRCode (MT:--VJ0-IC005WSL02810) and QR helper URL on UART, and drives the BK7231N/BL2028N pwm_new peripheral."},
  {"BK7238 Tuya Arlec PC191HA-S4 plug (T1-2S-NL, 1.0.2) calibrates and fast-connects to stored AP","firmwares/Tuya_Arlec_PC191HA-S4_Plug_(schemaID-e1mtsiag)_keykmm3rjyqy5r8p_74xo1nolwa6xkjeu_T1-2S-NL_1.0.2.bin",c133_args,4,60,"",0,10,c133_exp,8,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A plaintext BK7238 dump from an Arlec Grid Connect PC191HA Series 4 smart plug (oem_t1_plug:1.0.2 on TuyaOS, T1-2S-NL module). It completes RF calibration and TLS RNG initialization, reads its protected key at 0x1f5000, and triggers Wi-Fi station fast-connect to its stored 'amc' network."},
  {"BK7238 Tuya BSEED M1 metering socket (T1-2S-NL, 1.1.4) boots TuyaOS plug app and scans","firmwares/Tuya_BSEED_M1_Measure_Socket_(schemaID-e1nbktiw)_keykmm3rjyqy5r8p_vl7svb0a4o9gogeb_T1-2S-NL_1.1.4.bin",c134_args,4,60,"",0,10,c134_exp,8,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A plaintext BK7238 dump from a BSEED M1 energy-metering wall socket (oem_t1_plug:1.1.4 on TuyaOS, T1-2S-NL module). It completes RF calibration and TLS RNG initialization, recovers its protected flash key at 0x1f5000, finishes mf_init, and starts Wi-Fi scanning."},
  {"BK7238 Tuya QS-WIFI-S10-C04 curtain module (T1-M, 1.0.6) raises SmartLife-BF55 AP and BLE central","firmwares/Tuya_Curtain_QS-WIFI-S10-C04_(schemaID-e1n257is)_keyafhmcfuursnuw_ocz3mqv0v4ntkzva_ocz3mqv0v4ntkzva_T1-M_1.0.6.bin",c135_args,4,60,"",0,10,c135_exp,8,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A plaintext BK7238 dump from a Lonsonho QS-WIFI-S10-C04 smart curtain switch module (tuyaos_curtain:1.0.6 on TuyaOS 3.11.11, T1-M module). It initializes the BLE 5.2 stack and BLE central remote-control listener, validates product ID ocz3mqv0v4ntkzva, raises its open SmartLife-BF55 pairing Soft-AP, and starts app_key_task."},
  {"BK7238 Tuya AZIot 4-gang switch (T1-3S, 1.1.9) calibrates and starts Wi-Fi scan","firmwares/Tuya_AZIot_4GSwitch_(schemaID-e1mzybmo)_keyfxuj3d5wg53wx_yiijiejtycigal7g_T1-3S_1.1.9.bin",c136_args,4,60,"",0,10,c136_exp,8,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A plaintext BK7238 dump from an AZIot 4-gang smart wall switch (oem_t1_switch:1.1.9 on TuyaOS, T1-3S module). It completes RF calibration and TLS RNG initialization, recovers its protected flash key at 0x1f5000, finishes mf_init, and starts Wi-Fi scanning."},
  {"BK7238 Tuya TOMZN 63A SmartMeterPro breaker (T1-U-HL, 1.1.0) loads 63A config and fast-connects","firmwares/Tuya_TOMZN_63A_Breaker_SmartMeterPro_(schemaID-e1n27y7k)_key7mapd9sdpyyf9_suznd0ma6xr4caz0_T1-U-HL_1.1.0.bin",c137_args,4,60,"",0,10,c137_exp,6,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A plaintext BK7238 dump from a TOMZN 63A DIN-rail smart circuit breaker and energy meter (TuyaOS 3.11.16, T1-U-HL module, 1.1.0). It loads its bistable relay and 63000 mA over-current configuration from flash, completes RF calibration, and initiates Wi-Fi fast-connect to its stored 'shuoda5' network."},
  {"BK7231N Tuya Milfra 3-gang TB11 switch (SDK 2.3.3) initializes BLE 5.1 stack and GAPM config","firmwares/BK7231N_Milfra_3g_TB11_QIO_2024-20-8-15-32-47.bin",c138_args,6,60,"",0,10,c138_exp,6,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A stock BK7231N QIO dump from a Milfra 3-gang TB11 smart wall switch (TUYA IOT SDK 2.3.3). With the BK7231N BLE 5.1 controller modeled, it initializes the RW BLE stack, logs BLE MAC 50-8b-b9-6e-c3-ba, and completes GAPM device configuration."},
  {"BK7231N Tuya A228903 Plafon ceiling light initializes BLE 5.1 GATT DB and advertising","firmwares/BK7231N_QIO_A228903_Plafon_2024-13-1-19-42-24.bin",c139_args,6,60,"",0,10,c139_exp,6,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A stock BK7231N QIO dump from an A228903 RGBCW LED ceiling plafon light. It initializes the RivieraWaves BLE 5.1 stack (BLE_STACK_OK, STACK INIT OK), creates its GATT database (CREATE DB SUCCESS), and starts BLE advertising."},
  {"BK7231N Tuya LSC SmartPanel (CBU, 1.3.1) boots TuyaOS and scans for tuya_mdev_test2","firmwares/Tuya_LSC_SmartPanel_(schemaID-e1mu25o0)_keyj3w8cmutjmwk5_duxmr5beo0hrowle_CBU_1.3.1.bin",c140_args,6,60,"",0,10,c140_exp,7,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A stock BK7231N dump from an LSC Smart Connect LED SmartPanel (CBU module, firmware 1.3.1 on TuyaOS). It recovers its protected key at 0x1ee000, initializes TLS RNG and mf_init, completes RF calibration, and scans for the tuya_mdev_test2 factory SSID."},
  {"BK7231N Tuya Sprycle SPL11-528 water valve (CB3S, 1.0.2) starts BLE advertising and ADC channel 3","firmwares/Tuya_Sprycle_SPL11-528_Water_Valve_(SchemaID-000003w2yl)_s2vw3kf1575apfyc_CB3S_1.0.2.bin",c141_args,6,60,"",0,10,c141_exp,8,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A stock BK7231N dump from a Sprycle SPL11-528 smart irrigation water timer valve (fj_wifi_ble_controller_moto:1.0.2 on TUYA IOT SDK 2.3.1, CB3S module). It brings up BLE 5.1 advertising, validates product key s2vw3kf1575apfyc, initializes battery/valve SARADC channel 3, and starts Wi-Fi scanning."},
  {"BK7231N Tuya Eurodomest HDMI Sync Box (CBU, 1.0.7) starts BLE stack and lighting effect task","firmwares/Tuya_Eurodomest_HDMI_Sync_Box_(6204000100_schemaID-er8l48)_aceyfgaokqwk4nzj_CBU_1.0.7.bin",c142_args,6,60,"",0,10,c142_exp,7,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A stock BK7231N dump from a Eurodomest 6204000100 HDMI ambient-lighting TV sync box (CBU module, firmware 1.0.7). It brings up the BLE 5.1 stack and advertising, completes RF calibration, allocates its segmented lighting buffer, and launches the lighting effect main task."},
  {"BK7231N Tuya ATLO-RC-IR universal IR remote (SDK 2.3.1) loads IR pin map and starts BLE adv","firmwares/BK7231N_QIO_ATLO-RC-IR-TUYA_2025-16-1-16-44-50.bin",c143_args,6,60,"",0,10,c143_exp,8,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A stock BK7231N QIO dump from an ATLO-RC-IR-TUYA Wi-Fi universal infrared remote blaster (TUYA IOT SDK 2.3.1). It brings up the BLE 5.1 stack and advertising, reads its protected configuration mapping IR transmit (P7), IR receive (P8), reset button (P9) and status LED (P24), and scans for tuya_mdev_test1."},
  {"BK7231T Tuya W-105N motorized ball valve (WB3S, 1.1.5) loads pConfig and starts BLE adv","firmwares/Tuya_BallValve_W-105N_(WIFI&BLE-_schemaID-0000043vc2)_key7axydcvmea3x9_hjnivuviqdko7mzd_WB3S_1.1.5.bin",c144_args,3,60,"",0,10,c144_exp,6,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A stock BK7231T dump from a W-105N Wi-Fi + BLE motorized water/gas ball valve controller (oem_bk7231s_rnd_zero_dctrl_switch:1.1.5 on TUYA IOT SDK 1.0.6, WB3S module). It loads its pin configuration (bt1_pin:26, led1_pin:24), validates product key key7axydcvmea3x9, completes device_init, and starts BLE advertising."},
  {"BK7231T Tuya AHRISE AHR-083 power strip (WB3S, 1.1.0) loads multi-relay pConfig and advertises BLE","firmwares/Tuya_AHRISE_AHR-083_PowerStrip_(schemaID-000002qqwe)_keya4gvchmtapm8n_fn2jtjtvkegj2oll_WB3S_1.1.0.bin",c145_args,3,60,"",0,10,c145_exp,7,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A stock BK7231T dump from an AHRISE AHR-083 multi-socket smart power strip (oem_bk7231s_rnd_switch:1.1.0 on TUYA IOT SDK 1.0.2, WB3S module). It recovers its protected key at 0x1ee000, loads its multi-relay pin table (wifi_stat on P9), validates product key keya4gvchmtapm8n, and starts BLE advertising."},
  {"BK7231T Tuya MS-103 two-way TempHum switch (1.2.8, SDK 1.0.2) starts BLE advertising and scans","firmwares/Tuya_MS-103-Two-Way_TempHum_Switch_(NH-YM0430_schemaID-0000037bpa)_keyxnxg34hf5xw9e_dlsdk5zvkuuhufpy_1.2.8.bin",c146_args,3,60,"",0,10,c146_exp,6,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A stock BK7231T dump from an MS-103 two-way temperature and humidity thermostat switch (oem_bk7231_2onoff_wetemp:1.2.8 on TUYA IOT SDK 1.0.2). It completes RF calibration, brings up BLE GATT advertising, loads its relay/sensor pin configuration, and starts Wi-Fi scanning."},
  {"BK7231T Tuya TreatLife ST30 TV LED backlight (1.0.2, SDK 1.0.2) initializes RGB frame, BLE and ADC","firmwares/Tuya_TreatLife_ST30_TV_LED_Backlight_(ST30HSR_schemaID-000002fli4)_xhsx0clcbovz5xre_1.0.2.bin",c147_args,3,60,"",0,10,c147_exp,7,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A stock BK7231T dump from a TreatLife ST30 RGB TV ambient LED backlight strip (HYS_03_010_RGB_xhsx0clcbovz5xre:1.0.2 on TUYA IOT SDK 1.0.2). It initializes the light control frame in mode 1, brings up BLE advertising, validates PID xhsx0clcbovz5xre, initializes SARADC channel 3 for mic/music sensing, and starts Wi-Fi scanning."},
  {"BK7231T Tuya BIM8774 16A 3500W metering socket (WB2S, 1.3.5) boots dltj energy app and BLE","firmwares/Tuya_BIM8774_Socket_16A_3500W_(BOMSOC8774_schemaID-0000032ccw)_keym9qkuywghyrvs_jug4i7aunvvjfhq7_WB2S_1.3.5.bin",c148_args,3,60,"",0,10,c148_exp,7,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A stock BK7231T dump from a BIM8774 (BOMSOC8774) 16A 3500W energy-metering smart socket (oem_bk7231s_dltj:1.3.5 on TUYA IOT SDK 1.0.2, WB2S module). It recovers its protected key, validates product key keym9qkuywghyrvs, initializes the dltj energy accumulator, and starts BLE advertising."},
  {"BK7231T Tuya 120V A19 9W smart bulb (WB2L, 2.9.6) raises SmartLife-8811 AP and auto-blinks","firmwares/Tuya_120V_A19_9W_SMART_BULB_(schemaID-000002p9xg)_keytg5kq8gvkv9dh_mlgrefimabrept8a_WB2L_2.9.6.bin",c149_args,3,60,"",0,10,c149_exp,7,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A stock BK7231T dump from a 120V A19 9W RGBCW smart bulb (oem_bk7231s_light_ty:2.9.6 on TUYA IOT SDK 1.0.2, WB2L module). It initializes the 5-channel light frame, validates product key keytg5kq8gvkv9dh, raises its SmartLife-8811 pairing Soft-AP on channel 6, starts BLE advertising, and begins AP-config auto-blinking."},
  {"BK7231T Tuya 120V A60 9W smart bulb (WB2L, 2.9.25, SDK 2.3.2) loads 5ch light config and boots","firmwares/Tuya_120V_A60_9W_SMART_BULB_(schemaID-000004kvcq)_keytg5kq8gvkv9dh_uuyc3x6l7gusczhb_WB2L_2.9.25.bin",c150_args,3,60,"",0,10,c150_exp,7,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A stock BK7231T dump from a 120V A60 9W RGBCW smart bulb (oem_bk7231s_light_ty:2.9.25 on TUYA IOT SDK 2.3.2, WB2L module). It loads its 5-channel PWM pin map (2 1 0 4 3) and color balance table (28 28 28 50 50), initializes the BLE stack, and recovers its protected flash key."},
  {"BK7231T Tuya 1CH WB01 switch (WB3S, 1.1.5, SDK 1.0.6) loads pConfig and starts BLE adv","firmwares/Tuya_1CH_WB01_Switch_(schemaID-00000489v0)_key7axydcvmea3x9_etehelyd9guknfyi_WB3S_1.1.5.bin",c151_args,3,60,"",0,10,c151_exp,7,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A stock BK7231T dump from a 1-channel WB01 smart switch (oem_bk7231s_rnd_zero_dctrl_switch:1.1.5 on TUYA IOT SDK 1.0.6, WB3S module). It brings up BLE advertising, loads its relay/button/LED pConfig (bt1_pin:6, led1_pin:11), validates product key key7axydcvmea3x9, and reaches wifi_stat = 5."},
  {"OpenBeken DeepSleep 10 enters SCTRL deep sleep and wakes via RTC timer","firmwares/OpenBK7231T_QIO_1.18.300_mathDemo_obkStartupCommand_uartConsole.bin",c152_args,7,60,"",0,10,c152_exp,4,0,0,0,0,0,0,0,0,0,0,0,0,c152_tags,1,1000,
   "Exercises OpenBeken's `DeepSleep 10` command on BK7231T: the firmware counts down, programs the SCTRL 32 kHz ROSC timer (`0x0080011C`/`0x00800140`) for 10 seconds, and writes `0xADC1` to `SCTRL_SLEEP` (`0x00800104`). With `--deep-sleep-wake rtc`, the emulator wakes the chip via warm SCTRL reset with `ROSC_TIMER_INT_STATUS` set."},
  {"OpenBeken PinDeepSleep enters SCTRL deep sleep and wakes via GPIO P10","firmwares/OpenBK7231T_QIO_1.18.300_mathDemo_obkStartupCommand_uartConsole.bin",c153_args,7,60,"",0,10,c153_exp,4,0,0,0,0,0,0,0,0,0,0,0,0,c153_tags,1,1000,
   "Exercises OpenBeken's `PinDeepSleep` door-sensor sleep mode on BK7231T: after configuring P10 as a button (`SetPinRole 10 Btn`), `PinDeepSleep` arms `SCTRL_GPIO_WAKEUP_EN` (`0x00800120`) and `SCTRL_GPIO_WAKEUP_TYPE` (`0x00800124`) for active-low wake on P10, enters deep sleep via `0x00800104`, and wakes via `--deep-sleep-wake gpio:10`."},
  {"BK7231N Tuya Generic Motion Sensor (CBU, 1.0.6) enters deep sleep and wakes on PIR GPIO P16","firmwares/BK7231N_Tuya_Generic_Motion_Sensor_CBU_1.0.6.bin",c154_args,8,90,"",0,10,c154_exp,5,0,0,0,0,0,0,0,0,0,0,0,0,c154_tags,1,1000,
   "A stock battery-powered Tuya PIR motion sensor on BK7231N (CBU module, firmware 1.0.6). On cold boot it reports `cur_wake_type:0`, arms a 28800s RTC timer plus GPIO wake on P16 (PIR trigger, active-high) and P20 (button), and enters SCTRL deep sleep (`0x00800104`). When woken via `--deep-sleep-wake gpio:16`, the firmware reads the retained SCTRL/AON GPIO wake status registers (`0x00800128`/`0x00808004`) and reports `cur_wake_type:3` (PIR alarm)."},
  {"BK7231N Tuya Rain Sensor (CBU, 1.1.9) enters deep sleep and wakes on water-probe GPIO P16","firmwares/BK7231N_Tuya_Rain_Sensor_CBU_1.1.9.bin",c155_args,9,90,"",0,10,c155_exp,7,0,0,0,0,0,0,0,0,0,0,0,0,c155_tags,1,1000,
   "A stock battery-powered Tuya water/rain leak sensor on BK7231N (`oem_bk7231n_water_sensor_plus:1.1.9`, CBU module). On cold boot it logs `power_on wake up`, arms a 28800s RTC timer plus GPIO wake on P16 (water probe, active-high) and P20 (reset button, active-low), and enters SCTRL deep sleep. Waking on P16 (`--deep-sleep-wake gpio:16`) reboots the chip with GPIO wake status set so the firmware logs `IO wake up` and `g_dev_ctl.wake_event is :2`."},
  {"BK7231N Tuya TH01 TempHum Sensor (CBU, 1.1.17) enters 3600s deep sleep and wakes via RTC timer","firmwares/BK7231N_Tuya_TH01_TempHum_CBU_1.1.17.bin",c156_args,9,90,"",0,10,c156_exp,6,0,0,0,0,0,0,0,0,0,0,0,0,c156_tags,1,1000,
   "A stock battery-powered Tuya TH01 temperature and humidity sensor on BK7231N (`oem_bk7231n_temp_hum_sensor:1.1.17`, CBU module). On cold boot it logs `Wakeup_GetEvent is:0`, arms a 3600s SCTRL ROSC timer plus GPIO wake on P16/P20, and enters deep sleep. When woken via `--deep-sleep-wake rtc`, the warm reset sets `ROSC_TIMER_INT_STATUS` (`0x0080011C` bit 8) and the firmware recognizes an RTC wake (`Wakeup_GetEvent is:4`)."},
  {"BK7238 Tuya SRW-TH01 TempHum Sensor (T1-AHL, 1.0.1) enters 240s deep sleep and wakes via RTC timer","firmwares/BK7238_Tuya_SRW-TH01_TempHum_T1-AHL_1.0.1.bin",c157_args,6,90,"",0,10,c157_exp,6,0,0,0,0,0,0,0,0,0,0,0,0,c157_tags,1,1000,
   "A stock battery-powered Tuya SRW-TH01 temperature and humidity sensor on BK7238 (T1-AHL module, firmware 1.0.1). On cold boot (`start_type:0`) it scans for its AP, then arms GPIO wake on P8/P9 and a 240s ROSC timer before writing `0xADC1` to `SCTRL_SLEEP` (`0x00800104`). Waking via `--deep-sleep-wake rtc` resets the chip with `ROSC_TIMER_INT_STATUS` set, which `bk_misc_init_start_type` reports as `start_type:4` (`RESET_SOURCE_DEEPPS_RTC`)."},
  {"BK7252N Pinmei A9 Camera (SDK 3.00.42.01) probes GC0328C over I2C1 and captures JPEG frame via EJPEG+GDMA","firmwares/BK7252N_Pinmei_A9_Camera_BKFIL_3.00.42.01.bin",c158_args,5,90,"",0,10,c158_exp,7,0,0,0,0,0,0,0,0,0,0,0,0,c158_tags,2,1000,
   "End-to-end DVP camera sensor and hardware JPEG capture test on BK7252N (`BK7252N_Pinmei_A9_Camera_BKFIL_3.00.42.01.bin`). Typing `video_buffer open` and `video_buffer read 4096` into the RT-Thread MSH shell initializes Timer 5 (`0x00802A4C`), probes the emulated GalaxyCore GC0328C sensor at I2C1 slave `0x21` (chip ID `0x9D` at reg `0xF0`), writes all 374 sensor configuration registers, enables `YUV_BUF` (`0x00A04000`) and `EJPEG` (`0x00A03000`) with GDMA channel 4 circular capture (`0x00809140`), and reads back a complete 1185-byte JFIF JPEG frame (`ff,d8,ff,e0,...`)."},
  {"BK7252 Linklemo XCThings HA15 Camera (IN-A15-V1.0) probes GC0328C over I2C1 and streams EJPEG+GDMA frames","firmwares/BK7252_Linklemo_XCThings_HA15_IN-A15-V1.0_12.12.16.bin",c159_args,5,90,"",0,10,c159_exp,5,0,0,0,0,0,0,0,0,0,0,0,0,c159_tags,2,1000,
   "End-to-end DVP camera sensor and EJPEG streaming test on BK7252 (`BK7252_Linklemo_XCThings_HA15_IN-A15-V1.0_12.12.16.bin`). Typing `video open` into the RT-Thread MSH shell starts `video_transfer_main`, opens hardware `I2C1` (`0x00802300`), identifies the emulated GC0328C sensor (`id_data:9d`), writes 391 sensor configuration registers, and streams 640x480 JPEG frames through the BK7252 EJPEG encoder (`0x0080A000`) and GDMA channel 4 (`0x00809080`)."},
  {"BK7252 Tuya A9 Cam SC-10024 (1.0.4, SDK 3.0.29) raises SmartLife-2640 Soft-AP and serves DHCP","firmwares/BK7252_Tuya_A9_Cam_SC-10024_1.0.4.bin",c160_args,5,90,"",0,10,c160_exp,7,0,0,0,0,0,0,0,0,0,0,0,0,c160_tags,1,1000,
   "A 4 MiB plaintext CRC-framed BK7252 dump from a Tuya SC-10024 A9 VGA Wi-Fi camera (`SC008-WB00:1.0.4` on SDK 3.0.29, PID `keyph8vwhadwsx3a`). It initializes its 256 KiB external SDRAM heap at `0x00900000`, raises its `SmartLife-2640` pairing Soft-AP, and hands our virtual Wi-Fi station a `192.168.175.100` DHCP lease and ICMP ping reply."},
  {"BK7252 Tuya Doorbell IP06 V6.3 (SC028_WB30_ALK, 2.2.4) boots BLE, SDRAM heap and reads 4MB flash key","firmwares/BK7252_Tuya_Doorbell_IP06_V6.3_2.2.4.bin",c161_args,5,90,"",0,10,c161_exp,7,0,0,0,0,0,0,0,0,0,0,0,0,c161_tags,1,1000,
   "A 4 MiB TUYA-encrypted BK7252 dump from an IP06 V6.3 smart video doorbell (`SC028_WB30_ALK:2.2.4` on SDK 3.0.29). It initializes both internal SRAM and 224 KiB external SDRAM heaps, brings up the BLE stack (`ty bt sdk init success finish`), completes RF calibration, and reads its protected flash key from `0x397000` in upper 4 MiB SPI flash."},
  {"BK7252 Tuya ONENUO ML1T-B Doorbell (SC028_WB30_ALK, 2.2.4) boots BLE, SDRAM heap and reads 4MB flash key","firmwares/BK7252_Tuya_ONENUO_ML1T-B_Doorbell_2.2.4.bin",c162_args,5,90,"",0,10,c162_exp,8,0,0,0,0,0,0,0,0,0,0,0,0,c162_tags,1,1000,
   "A 4 MiB TUYA-encrypted BK7252 dump from an ONENUO ML1T-B smart video doorbell (`SC028_WB30_ALK:2.2.4` on SDK 3.0.29, PID `keytqn8773d8kwuu`). It initializes internal SRAM and external SDRAM heaps, brings up the BLE stack, completes RF calibration, and reads its protected flash key from `0x397000`."},
  {"BK7238 BanlanX SP548E SPI LED controller verifies SPI Flash UID, drives 5ch 16 kHz pwm_new and advertises SP548E","firmwares/BK7238_BanlanX_SP548E_SPI_LED.bin",c163_args,6,60,"",0,10,c163_exp,7,0,0,c163_pins,16,c163_pwm,5,0,0,0,0,0,0,c163_tags,4,1000,
   "A plaintext BK7238 dump from a BanlanX SP548E SPI addressable RGBIC LED controller. The firmware stubs out `bk_printf` (emitting no UART text) and performs an anti-clone check at boot by reading the 16-byte NOR Flash Unique ID (`0x4B` via `flash_bypass_op_read` on `0x00802700..0x0080270C`) and comparing it against `flash_uid_save` stored in EasyFlash (`0x1FE000`). Once verified, it configures 5-channel 16 kHz `pwm_new` (`P6/P7/P8/P24/P26`, period `0x0659` = 1625 ticks) and SPI pads (`P14..P17`), brings up its `JIEWU` Soft-AP on channel 6, and advertises `SP548E` (UUID `0xFFE1`, Manufacturer `0x5053`) over BLE 5.2."},
  {"BK7238 Tuya 1CH_USBA switch (T1-U-HL, 1.1.9) configures relay P9/LED P26 and scans","firmwares/Tuya_1CH_USBA_(schemaID-e1n2x0u0)_keyfxuj3d5wg53wx_tj4ripw7ufzqh50f_T1-U-HL_1.1.9.bin",c164_args,4,60,"",0,10,c164_exp,10,0,0,c164_pins,6,0,0,0,0,0,0,0,0,0,0,1000,
   "A plaintext BK7238 dump from a Tuya 1-channel USB-A smart switch (`oem_t1_switch:1.1.9` on TuyaOS 3.8.31, `T1-U-HL` module, PID `tj4ripw7ufzqh50f`). It recovers its protected flash key at `0x1F5000`, completes RF calibration and TLS RNG initialization, configures relay P9 and status LED P26 as outputs (`0x00`), and starts Wi-Fi scanning."},
  {"BK7238 Tuya 63A_Breaker_TMPH (T1-U-HL, 1.1.0) configures bistable relay/metering GPIOs and wifi soc","firmwares/Tuya_63A_Breaker_TMPH_(schemaID-e1n1p4fk)_key7mapd9sdpyyf9_au6dqazvkxqnpaak_T1-U-HL_1.1.0.bin",c165_args,6,60,"",0,10,c165_exp,9,0,0,c165_pins,9,0,0,0,0,0,0,0,0,0,0,1000,
   "A plaintext BK7238 dump from a Tuya 63A TMPH DIN-rail smart circuit breaker and energy meter (`oem_t1_cutout_self_switch:1.1.0` on TuyaOS 3.11.16, `T1-U-HL` module). It loads its bistable relay and 50000 mA over-current configuration from `uffile`, configures `rl_on1_pin` P6 (`0x00`), `rl_off1_pin` P26 (`0x00`), `netled1_pin` P15 (`0x02`), `led1_pin` P17 (`0x02`) and `bt1_pin` P21 (`0x3C`), completes `tuya_ele_hw_init` and `dltj rslt ok!`, and initializes `wifi soc init` (`pid:key7mapd9sdpyyf9`)."},
  {"BK7238 Tuya ATORCH GR2PWS energy meter (TuyaMCU, T1-2S-NL, 3.0.22) accepts raw product and scans","firmwares/Tuya_ATORCH_GR2PWS_(schemaID-e1kylf6c)_key83r5jq9qqeaxt_y1kgvuhjplpctr1o_89l4pwmpcpryhgm7_TuyaMCU_T1-2S-NL_3.0.22.bin",c166_args,9,60,"",0,10,c166_exp,11,0,0,c166_pins,8,0,0,0,0,0,0,0,0,c166_tags,1,1000,
   "A plaintext BK7238 dump from an ATORCH GR2PWS DIN-rail energy meter (`t1_common_user_config_ty:3.0.22` on TuyaOS 3.11.11, `T1-2S-NL` module). It opens UART1 at 115200 baud, completes the TuyaMCU heartbeat (`0x00`), product-info (`0x01`) and working-mode (`0x02`) handshake with raw product ID `y1kgvuhjplpctr1o`, logs `enter success_proc` and `serial_no:20f1b21a10c4`, and starts Wi-Fi scanning."},
  {"BK7238 Tuya DIN AT-SY1-TSW breaker (T1-U-HL, 1.0.3, --adc 7:264) reads NTC on P20/ADC3 and scans","firmwares/Tuya_DIN_AT-SY1-TSW_(schemaID-e1mz4v24)_key7mapd9sdpyyf9_rh8slxo4otrhaqx8_T1-U-HL_1.0.3.bin",c167_args,6,60,"",0,10,c167_exp,13,c167_unexp,2,c167_pins,9,0,0,0,0,0,0,0,0,c167_tags,1,1000,
   "A plaintext BK7238 dump from an AT-SY1-TSW 63A DIN-rail smart breaker with NTC temperature sensing (`oem_t1_cutout_self_switch:1.0.3` on TuyaOS 3.11.16, `temp_fun_en:1`, `temp_mea:20`). Passing `--adc 7:264` completes the background `temp_detect` SARADC channel 7 conversion (`cal_bias!`, `cal dpll!`) so `user_temp_measure.c` opens SARADC channel 3 on P20 (`0x08`) cleanly without `adc ddev_open error:1`."},
  {"BK7238 Tuya IHSENO WF-TR-1 soil sensor (TuyaMCU, T1-U-HL, 3.0.22) accepts raw product and scans","firmwares/Tuya_IHSENO_WF-TR-1_Soil_(schemaID-e1mpvvpo)_key5s5pkkwnfwgvr_rknwi0ctbbghzgla_TuyaMCU_T1-U-HL_3.0.22.bin",c168_args,9,60,"",0,10,c168_exp,12,0,0,c168_pins,8,0,0,0,0,0,0,0,0,c168_tags,1,1000,
   "A plaintext BK7238 dump from an IHSENO WF-TR-1 soil moisture and temperature sensor (`t1_common_user_config_ty:3.0.22` on TuyaOS 3.11.11, `T1-U-HL` module). It opens UART1 at 115200 baud, completes the TuyaMCU handshake with raw product ID `rknwi0ctbbghzgla`, resets its Wi-Fi link parameters to factory state (`nc_tp:10`), logs `serial_no:3c0b59a46608`, and starts Wi-Fi scanning."},
  {"BK7238 Tuya HomeMate 4CHSwitch (T1-3S, 1.1.9) configures 4 relays P22/P23/P24/P26 and scans","firmwares/Tuya_HomeMate_4CHSwitch_(T1_V2_schemaID-e1mx46bc)_keyfxuj3d5wg53wx_fpuupxtmit0vj8i5_T1-3S_1.1.9.bin",c169_args,4,60,"",0,10,c169_exp,10,0,0,c169_pins,9,0,0,0,0,0,0,0,0,0,0,1000,
   "A plaintext BK7238 dump from a HomeMate 4-channel smart switch (`T1_V2`, `oem_t1_switch:1.1.9` on TuyaOS 3.8.31, `T1-3S` module, PID `fpuupxtmit0vj8i5`). It recovers its protected flash key at `0x1F5000`, configures 4 relay outputs (`P22/P23/P24/P26 = 0x00`) and status LED `P9` (`0x00`), logs `dctrl channel_num:4` and `mac 0:33:7a:a3:22:bf`, and starts Wi-Fi scanning."},
  {"BK7231N Tuya ATLO-WB3K 3-gang switch (CB3S, 1.3.5) loads pConfig, starts BLE adv and scans","firmwares/BK7231N_QIO_ATLO-WB3K-TUYA_2025-16-1-14-11-09.bin",c170_args,6,60,"",0,10,c170_exp,11,0,0,c170_pins,7,0,0,0,0,0,0,0,0,0,0,1000,
   "A stock BK7231N QIO dump from an ATLO-WB3K-TUYA 3-gang smart switch (`oem_bk7231n_dctrl_switch:1.3.5` on TUYA IOT SDK 2.3.3, `CB3S` module). It brings up the BLE 5.1 stack and starts advertising (`ble mac:50-8b-b9-9c-32-0f`), recovers its protected flash key at `0x1EE000`, loads its 3-gang `pConfig` (`rl1_pin:7`, `rl2_pin:8`, `rl3_pin:9`, `bt1_pin:14`, `bt2_pin:26`, `bt3_pin:24`, `netled_pin:6`), logs `channel init success`, and starts Wi-Fi scanning."},
  {"BK7231N Tuya Adelid MPD-Z curtain switch (CB2S, 1.0.6) unpairs via WDT reset and configures curtain GPIOs","firmwares/BK7231N_QIO_Adelid_Curtains_MPD-Z_2024-23-10-18-21-33.bin",c171_args,6,60,"",0,10,c171_exp,14,0,0,c171_pins,11,0,0,0,0,0,0,0,0,0,0,1000,
   "A stock BK7231N QIO dump from an Adelid MPD-Z smart curtain switch (`oem_bk7231n_curtain_onoff:1.0.6` on TUYA IOT SDK 2.3.1, `CB2S` module). Captured after two power cycles, its first boot reaches `power on cnt = 3` (`power on cnt = 3 satisfy, unactive now!`), clears the counter in flash, and triggers a hardware watchdog reboot (`[WDT] watchdog expired; device reset #1`). On the second boot (with flash writes preserved across reset) it logs `power on cnt = 1`, advertises BLE service `0xA201`, and configures open/close relays (`P24/P26 = 0x00`), buttons (`P7/P8/P11 = 0x3C`) and Wi-Fi LED (`P10 = 0x02`)."},
  {"BK7231T Tuya WB_SW01 1CH light switch (WB3S, 1.0.7) configures relay/backlit GPIOs and raises SmartLife-30C5 AP","firmwares/Tuya_1CH-Light_Switch_(WB_SW01_schemaID-000002m8gk)_key7axydcvmea3x9_axahm7ilagakvp8i_WB3S_1.0.7.bin",c172_args,4,60,"",0,10,c172_exp,14,0,0,c172_pins,8,0,0,0,0,0,0,0,0,0,0,1000,
   "A stock BK7231T dump from a Tuya WB_SW01 1-channel light switch (`oem_bk7231s_rnd_zero_dctrl_switch:1.0.7` on TUYA IOT SDK 1.0.2, `WB3S` module, PID `axahm7ilagakvp8i`). With `--wifi` completing the factory `tuya_mdev_test1` scan, it loads `pConfig`, configures relay `P9` (`0x00`), button `P10` (`0x3C`), backlight LED `P11` (`0x00`), channel LED `P26` (`0x00`) and Wi-Fi LED `P1` (`0x00`), logs `device_init ok`, and raises its `SmartLife-30C5` pairing Soft-AP on channel 6."},
  {"BK7238 Tuya LSC 3202087 energy plug (T1-2S-NL, 1.0.2) configures relay/BL0937 GPIOs and wifi soc","firmwares/Tuya_LSC_3202087_(5225000201_schemaID-e1n2e16w)_keykmm3rjyqy5r8p_6sfrvjaazvxsdgbp_T1-2S-NL_1.0.2.bin",c173_args,5,60,"",0,10,c173_exp,12,0,0,c173_pins,9,0,0,0,0,0,0,0,0,0,0,1000,
   "A plaintext BK7238 dump from an LSC Smart Connect 3202087 (5225000201) energy-monitoring smart plug (`oem_t1_plug:1.0.2` on TuyaOS 3.8.31, `T1-2S-NL` module, PID `6sfrvjaazvxsdgbp`). With `--wifi` completing its initial scan, it recovers its protected flash key at `0x1F5000`, configures relay `P24` (`0x00`), LEDs `P9/P11` (`0x02`), BL0937 metering inputs `P6/P8` (`0x3C`) and button `P26` (`0x3C`), and completes `wifi soc init` (`serial_no:bc351e15cd31`)."},
  {"BK7238 Tuya LSC 5225000301 FR energy plug (T1-2S-NL, 1.0.2) configures relay/BL0937 GPIOs and wifi soc","firmwares/Tuya_LSC_5225000301_Plug(FR)_(schemaID-e1n2e11c)_keykmm3rjyqy5r8p_b61eihfqeaexn54g_T1-2S-NL_1.0.2.bin",c174_args,5,60,"",0,10,c174_exp,12,0,0,c174_pins,9,0,0,0,0,0,0,0,0,0,0,1000,
   "A plaintext BK7238 dump from an LSC Smart Connect 5225000301 French Type-E energy-monitoring smart plug (`oem_t1_plug:1.0.2` on TuyaOS 3.8.31, `T1-2S-NL` module, PID `b61eihfqeaexn54g`). With `--wifi` completing its initial scan, it recovers its protected flash key (`wr_cnt 9`), configures relay `P24` (`0x00`), LEDs `P1/P9/P11` (`0x02`), BL0937 metering inputs `P6/P8` (`0x3C`) and button `P26` (`0x3C`), and completes `wifi soc init` (`serial_no:bc351e3fc3f9`)."},
  {"BK7238 Tuya AU5101XA MiniBreaker (T1-CHL, 1.1.9) raises SmartLife-50C1 AP and advertises BLE 0xFD50","firmwares/Tuya_MiniBreaker_AU5101XA_(schemaID-e1n6c5uw)_keyfxuj3d5wg53wx_cwpezmgx7zbylz3a_T1-CHL_1.1.9.bin",c175_args,5,60,"",0,10,c175_exp,14,0,0,c175_pins,7,0,0,0,0,0,0,0,0,0,0,1000,
   "A plaintext BK7238 dump from an AU5101XA mini circuit breaker switch (`oem_t1_switch:1.1.9` on TuyaOS 3.8.31, `T1-CHL` module, PID `cwpezmgx7zbylz3a`). With `--wifi` and `--ble-core`, it recovers its protected flash key (`wr_cnt 22`), configures relay `P26` (`0x00`), inputs `P6/P15` (`0x3C`) and blinking LED `P17`, raises its `SmartLife-50C1` pairing Soft-AP (`192.168.176.1`), and broadcasts Tuya BLE `0xFD50` service data (`keyfxuj3d5wg53wx`)."},
  {"BK7238 Tuya RH-EU411-4U-W 5CH power strip (T1-U-HL, 1.0.2) advertises BLE and polls BL0942 over UART1","firmwares/Tuya_RH-EU411-4U-W_PowerStrip_(schemaID-e1mpvy6k)_keykmm3rjyqy5r8p_mhmodd3ymrpfcyaq_T1-U-HL_1.0.2.bin",c176_args,6,60,"",0,10,c176_exp,14,0,0,c176_pins,10,0,0,0,0,0,0,0,0,0,0,1000,
   "A plaintext BK7238 dump from an RH-EU411-4U-W 4-AC + USB smart power strip (`oem_t1_plug:1.0.2` on TuyaOS 3.8.31, `T1-U-HL` module, PID `mhmodd3ymrpfcyaq`). It recovers its protected flash key (`wr_cnt 20`), configures 5 relay outputs (`P6/P9/P21/P24/P26 = 0x02`) and button `P22` (`0x3C`), advertises Tuya BLE `0xFD50` (`ble mac:bc-35-1e-b4-d8-eb`), and continuously polls the emulated BL0942 energy meter over `UART1` (`58 aa`)."},
  {"BK7238 Tuya e1mzkfec energy switch (T1-2S-NL, 1.1.4) configures BL0942 USR_WRPROT/OT_FUNX and polls UART1","firmwares/Tuya_Switch_(schemaID-e1mzkfec)_keyfxuj3d5wg53wx_jsejjp8bin9lllh3_T1-2S-NL_1.1.4.bin",c177_args,6,60,"",0,10,c177_exp,12,c177_unexp,2,c177_pins,7,0,0,0,0,0,0,0,0,0,0,1000,
   "A plaintext BK7238 dump from a Tuya energy-monitoring smart switch (`oem_t1_switch:1.1.4` on TuyaOS 3.8.31, `T1-2S-NL` module, PID `jsejjp8bin9lllh3`). During `tdd_energy_monitor_bl0942.c` initialization it unlocks BL0942 `USR_WRPROT` (`a8 1d 55`), soft-resets (`a8 1c 5a 5a 5a`), verifies `USR_WRPROT` readback (`58 1d` -> `0x01`), configures `OT_FUNX` (`a8 18 26`, `58 18` -> `0x26`) for CF1 zero-cross output, and enters continuous `58 aa` full-packet energy polling."},
  {"BK7238 Tuya IHSENO TempHum sensor (TuyaMCU, T1-3S, 3.0.22) raises SmartLife-FE5F AP and advertises BLE","firmwares/Tuya_IHSENO_TempHum_(schemaID-e1my5reg)_key5s5pkkwnfwgvr_pxrmxy5zzzhfklxh_pxrmxy5zzzhfklxh_TuyaMCU_T1-3S_3.0.22.bin",c178_args,9,60,"",0,10,c178_exp,12,0,0,c178_pins,8,0,0,0,0,0,0,0,0,0,0,1000,
   "A plaintext BK7238 dump from an IHSENO temperature and humidity sensor (`t1_common_user_config_ty:3.0.22` on TuyaOS 3.11.11, `T1-3S` module). It completes the TuyaMCU handshake with raw product ID `pxrmxy5zzzhfklxh`, recovers its protected flash key (`wr_cnt 15`), raises its `SmartLife-FE5F` pairing Soft-AP (`192.168.176.1`), and broadcasts Tuya BLE `0xFD50` service data (`pxrmxy5zzzhfklxh`)."},
  {"BK7238 Tuya PJ-1103C dual clamp meter (TuyaMCU, 3.0.22) raises SmartLife-9943 AP and advertises BLE","firmwares/Tuya_PJ-1103C_DualClampMeter_(schemaID-e1kcg9c4)_key5s5pkkwnfwgvr_wifech3utowiyknu_ymmpqb0vqlipcvsl_TuyaMCU_3.0.22.bin",c179_args,9,60,"",0,10,c179_exp,12,0,0,c179_pins,8,0,0,0,0,0,0,0,0,0,0,1000,
   "A plaintext BK7238 dump from a PJ-1103C dual-channel CT clamp power meter (`t1_common_user_config_ty:3.0.22` on TuyaOS 3.11.11). It completes the TuyaMCU handshake with raw product ID `wifech3utowiyknu`, recovers its protected flash key at `0x1F5000`, raises its `SmartLife-9943` pairing Soft-AP on channel 6, and broadcasts Tuya BLE `0xFD50` service data (`wifech3utowiyknu`)."},
  {"BK7238 Tuya RMshebei WTDZW-63A2 breaker (TuyaMCU, T1-3S, 3.0.22) raises SmartLife-FC5C AP and advertises BLE","firmwares/Tuya_RMshebei_Breaker_WTDZW-63A2_(schemaID-f2l9rs)_key83r5jq9qqeaxt_z3jngbyubvwgfrcv_z7idaarns67a0va0_TuyaMCU_T1-3S_3.0.22.bin",c180_args,9,60,"",0,10,c180_exp,12,0,0,c180_pins,8,0,0,0,0,0,0,0,0,0,0,1000,
   "A plaintext BK7238 dump from an RMshebei WTDZW-63A2 DIN-rail smart circuit breaker (`t1_common_user_config_ty:3.0.22` on TuyaOS 3.11.11, `T1-3S` module). It completes the TuyaMCU handshake with raw product ID `z3jngbyubvwgfrcv`, recovers its protected flash key, raises its `SmartLife-FC5C` pairing Soft-AP (`192.168.176.1`), and broadcasts Tuya BLE `0xFD50` service data (`z3jngbyubvwgfrcv`)."},
  {"BK7238 Tuya TH03Pro TempHum sensor (TuyaMCU, T1-3S, 3.0.22) raises SmartLife-A0FC AP and advertises BLE","firmwares/Tuya_TH03Pro_(001TH02T1-3S_schemaID-e1my6fec)_key5s5pkkwnfwgvr_xeagimantb7d7apb_TuyaMCU_T1-3S_3.0.22.bin",c181_args,9,60,"",0,10,c181_exp,12,0,0,c181_pins,8,0,0,0,0,0,0,0,0,0,0,1000,
   "A plaintext BK7238 dump from a TH03Pro temperature and humidity sensor (`t1_common_user_config_ty:3.0.22` on TuyaOS 3.11.11, `T1-3S` module). It completes the TuyaMCU handshake with raw product ID `xeagimantb7d7apb`, recovers its protected flash key, raises its `SmartLife-A0FC` pairing Soft-AP (`192.168.176.1`), and broadcasts Tuya BLE `0xFD50` service data (`xeagimantb7d7apb`)."},
  {"BK7238 Tuya TH06 TempHum sensor (T1-CHL, 1.6.9, --adc 7:258) reads battery ADC on P26 and boots BLE","firmwares/Tuya_TH06_TempHum_C1118_P10_W1_SR03_(schemaID-e1n5ofug)_keyt7q4rh3aycerc_vcbxhbj855zfw5op_T1-CHL_1.6.9.bin",c182_args,7,60,"",0,10,c182_exp,11,c182_unexp,1,c182_pins,9,0,0,0,0,0,0,0,0,0,0,1000,
   "A plaintext BK7238 dump from a TH06 temperature and humidity sensor (`COMPILE_PLATFORM_TEMPLATE V1.6.9`, `QNE Version: V1.4-cc8a24d`, `T1-CHL` module, PID `vcbxhbj855zfw5op`). Passing `--adc 7:258` completes the background `temp_detect` SARADC channel 7 conversion (`temp_code:86 - adc_code:129`) so `tuya_temphum` opens SARADC on `P26` (`0x08`) cleanly without `adc ddev_open error:1`, reads `u32AdcValue 330`, and initializes BLE (`ble mac:e4-ae-e4-1b-92-ca`)."},
  {"BK7238 NiceMCU WB3S V1 SparkleIOT AT firmware prints ready banner on UART1","firmwares/QIO_NiceMCU_WB3S_V1-piece1_2024-10-12-14-19-05.bin",c183_args,5,60,"",0,10,c183_exp,3,0,0,c183_pins,4,0,0,0,0,0,0,0,0,0,0,1000,
   "A plaintext BK7238 factory dump from a NiceMCU WB3S V1 development board running SparkleIOT AT firmware. It configures UART1/UART2 pads (`P0/P1/P10/P11 = 0x78`), opens the Wi-Fi MAC RX filter, and prints `www.sparkleiot.com` and `ready` over UART1."},
  {"BK7238 Tuya QS-WiFi-S10-C04-c90a curtain switch (1.0.6) raises SmartLife-C90B AP and advertises BLE","firmwares/QS-WiFi-S10-C04-c90a.bin",c184_args,5,60,"",0,10,c184_exp,11,0,0,c184_pins,8,0,0,0,0,0,0,0,0,0,0,1000,
   "A plaintext BK7238 dump from a Lonsonho QS-WiFi-S10-C04 smart curtain switch (`tuyaos_curtain_switch_wifi:1.0.6` on TuyaOS 3.11.11, PID `ocz3mqv0v4ntkzva`). Captured with `count: 5` in `klts4`, it recovers its protected flash key at `0x1F5000`, raises its `SmartLife-C90B` pairing Soft-AP (`192.168.176.1`), broadcasts Tuya BLE `0xFD50` service data (`ocz3mqv0v4ntkzva`), and clears its local `klts4` counter in flash."},
  {"BK7238 Tuya BSEED 2G switch (T1-3S, 1.1.9) configures dual relay/LED GPIOs and scans","firmwares/Tuya_BSEED_2G_Switch_(schemaID-e1n1f8kg)_keyfxuj3d5wg53wx_oayppk7skuax4j6i_T1-3S_1.1.9.bin",c185_args,5,60,"",0,10,c185_exp,11,0,0,c185_pins,9,0,0,0,0,0,0,0,0,0,0,1000,
   "A plaintext BK7238 dump from a BSEED 2-gang smart wall switch (`oem_t1_switch:1.1.9` on TuyaOS 3.8.31, `T1-3S` module, PID `oayppk7skuax4j6i`). With `--wifi` completing its initial scan, it recovers its protected flash key (`wr_cnt 8`), configures outputs `P8/P9/P20/P22/P23` (`0x00`), initializes 2 multiplexed network LEDs (`sg_net_led.is_mux:2`), and completes `wifi soc init` (`serial_no:80647cc9ff10`)."},
  {"BK7238 Tuya MANHOT 63A 1P+N breaker (CBU/T1, 1.0.3) configures bistable relay/metering GPIOs and scans","firmwares/Tuya_MANHOT_63A_1P+N_(schemaID-e1n1p4fk)_key7mapd9sdpyyf9_au6dqazvkxqnpaak_CBU_1.0.3.bin",c186_args,5,60,"",0,10,c186_exp,11,0,0,c186_pins,9,0,0,0,0,0,0,0,0,0,0,1000,
   "A plaintext BK7238 dump from a MANHOT 63A 1P+N DIN-rail smart circuit breaker and energy meter (`oem_t1_cutout_self_switch:1.0.3` on TuyaOS 3.11.16, PID `au6dqazvkxqnpaak`). It loads its bistable relay and 50000 mA over-current configuration from `uffile`, configures `rl_on1_pin` `P6` (`0x00`), `rl_off1_pin` `P26` (`0x00`), LEDs `P15/P17` (`0x02`) and button `P21` (`0x3C`), completes `tuya_ele_hw_init success` and `dltj rslt ok!`, and scans for `tuya_mdev_test2`."},
  {"BK7238 Tuya RF Dry WF+RF switch (T1-2S-NL, 1.1.9) configures relay/button GPIOs and raises SmartLife-3466 AP","firmwares/Tuya_RF_Dry_WF+RF_(schemaID-e1n154og)_keyfxuj3d5wg53wx_dcbah0xg2uxpaman_T1-2S-NL_1.1.9.bin",c187_args,5,60,"",0,10,c187_exp,11,0,0,c187_pins,9,0,0,0,0,0,0,0,0,0,0,1000,
   "A plaintext BK7238 dump from a Tuya Wi-Fi + 433 MHz RF dry-contact relay switch (`oem_t1_switch:1.1.9` on TuyaOS 3.8.31, `T1-2S-NL` module, PID `dcbah0xg2uxpaman`). With `--wifi` completing its initial scan, it recovers its protected flash key (`wr_cnt 33`), configures outputs `P6/P9` (`0x00`) and `P24` (`0x02`) plus inputs `P8/P26` (`0x3C`), raises its `SmartLife-3466` pairing Soft-AP (`192.168.176.1`), and transitions `--->net state change to:13`."},
  {"BK7238 Tuya TH08 TempHum LCD sensor (TuyaMCU, T1-U-HL, 3.0.22) completes 9600-baud MCU handshake and scans","firmwares/Tuya_TH08_TempHumLCD_(schemaID-e1mtra8c)_key5s5pkkwnfwgvr_iunivahp92zlld0d_T1-U-HL_3.0.22.bin",c188_args,10,60,"",0,10,c188_exp,10,0,0,c188_pins,8,0,0,0,0,0,0,0,0,0,0,1000,
   "A plaintext BK7238 dump from a Tuya TH08 LCD temperature and humidity sensor (`t1_common_user_config_ty:3.0.22` on TuyaOS 3.11.11, `T1-U-HL` module). It recovers its protected flash key at `0x1F5000`, completes the 9600-baud TuyaMCU handshake (`VER:1.0.0,mcu tp :9`) with its provisioned product key `b8vqmdjmpswbap1q` (`enter success_proc`), and initializes Wi-Fi (`mac bc:35:1e:66:34:69`)."},
  {"BK7238 Tuya TMWF02 fan switch (TuyaMCU, CB2S/T1, 3.0.22) completes 9600-baud MCU handshake and scans","firmwares/Tuya_TMWF02_FanSwitch_(LG01-WF_schemaID-f5ai2w)_key5s5pkkwnfwgvr_lwt5eraqqvwh3pfw_TuyaMCU_CB2S_3.0.22.bin",c189_args,10,60,"",0,10,c189_exp,10,0,0,c189_pins,8,0,0,0,0,0,0,0,0,0,0,1000,
   "A plaintext BK7238 dump from a TMWF02 (LG01-WF) smart ceiling fan and light switch (`t1_common_user_config_ty:3.0.22` on TuyaOS 3.11.11, PID `lwt5eraqqvwh3pfw`). It recovers its protected flash key (`wr_cnt 40`), completes the 9600-baud TuyaMCU handshake (`VER:1.0.0,mcu tp :9`, `enter success_proc`), and initializes Wi-Fi (`mac 80:64:7c:d8:a0:40`)."},
  {"BK7238 Tuya TOMZN TOB9-63M breaker (T1-U-HL, 1.1.0) configures 63A metering/relay GPIOs and scans","firmwares/Tuya_TOMZN_TOB9-63M_(schemaID-e1n4le4w)_key7mapd9sdpyyf9_lo1i5edez6e6oyx6_T1-U-HL_1.1.0.bin",c190_args,5,60,"",0,10,c190_exp,11,0,0,c190_pins,9,0,0,0,0,0,0,0,0,0,0,1000,
   "A plaintext BK7238 dump from a TOMZN TOB9-63M 63A DIN-rail smart circuit breaker and energy meter (`oem_t1_cutout_self_switch:1.1.0` on TuyaOS 3.11.16, `T1-U-HL` module, PID `lo1i5edez6e6oyx6`). It loads its 63000 mA over-current configuration from `uffile`, configures outputs `P6/P8` (`0x00`), `P15/P21` (`0x02`) and input `P17` (`0x3C`), completes `tuya_ele_hw_init success`, and scans for `tuya_mdev_test1`."},
  {"BK7238 Tuya TempHum T1U sensor (T1-U-HL, 1.0.3) reads CHT8305 over bit-banged I2C (P23/P22)","firmwares/Tuya_TempHum_T1U_(schemaID-e1n1433o)_keypg4k4mhsvuvsx_0apiv8a0c4gnk0km_T1-U-HL_1.0.3.bin",c191_args,5,60,"",0,10,c191_exp,10,c191_unexp,1,c191_pins,4,0,0,0,0,0,0,0,0,0,0,1000,
   "A plaintext BK7238 dump from a Tuya temperature and humidity sensor (`WIFI_THS_T1U:1.0.3` on TuyaOS 3.11.25, `T1-U-HL` module, PID `0apiv8a0c4gnk0km`). It recovers its protected flash key at `0x1F5000`, starts `tdl_sensor_hub`, bit-bangs I2C on `SCL=P23, SDA=P22` to read the emulated CHT8305 sensor at slave `0x40` (`0x80`/`0x81`) without `i2c wait ack timeout`, and reports `Temp: 26.0` and `Humi: 50.0` via `example_drv_sensor.c`."},
  {"BK7238 Tuya WK604 4CH switch (CB3S/T1, 1.1.9) configures 4-gang GPIOs, raises SmartLife-F3CE AP and advertises BLE","firmwares/Tuya_WK604_4CH_(schemaID-e1n0qoxc)_keyfxuj3d5wg53wx_sjepnpg4qk9aqm6k_CB3S(T1)_1.1.9.bin",c192_args,5,60,"",0,10,c192_exp,8,0,0,c192_pins,4,0,0,0,0,0,0,0,0,0,0,1000,
   "A plaintext BK7238 dump from a WK604 4-gang smart switch (`oem_t1_switch:1.1.9` on TuyaOS 3.8.31, `CB3S(T1)` module, PID `sjepnpg4qk9aqm6k`). It recovers its protected flash key at `0x1F5000`, configures 4 channels (`dctrl channel_num:4`), raises its `SmartLife-F3CE` pairing Soft-AP (`192.168.176.1`), and broadcasts Tuya BLE `0xFD50` service data (`keyfxuj3d5wg53wx`)."},
  {"BK7238 Tuya WTDZW-63A breaker (T1, 3.0.22) completes TuyaMCU handshake and scans","firmwares/WTDZW-63A_BK7238_QIO_2026-12-7-11-34-52.bin",c193_args,9,60,"",0,10,c193_exp,10,0,0,c193_pins,8,0,0,0,0,0,0,0,0,0,0,1000,
   "A plaintext BK7238 dump from a WTDZW-63A smart breaker. It boots TuyaOS, initializes system timers and Wi-Fi monitor mode."},
  {"BK7238 Tuya wall touch switch 1G (T1, 1.1.4) boots TuyaOS and initializes Wi-Fi","firmwares/tuya-wall-touch-switch-1g-D8FC922D2222_1_backup_2026_12_4_17_59_45.bin",c194_args,5,60,"",0,10,c194_exp,10,0,0,c194_pins,4,0,0,0,0,0,0,0,0,0,0,1000,
   "A plaintext BK7238 dump from a Tuya 1-gang wall touch switch (oem_t1_switch:1.1.4). It boots TuyaOS and initializes Wi-Fi."},
  {"BK7238 Tuya wall touch switch 2G (T1, 1.1.4) boots TuyaOS and initializes Wi-Fi","firmwares/tuya-wall-touch-switch-2g-D8FC920F96D5_1_backup_2026_12_4_16_19_41.bin",c195_args,5,60,"",0,10,c195_exp,10,0,0,c195_pins,4,0,0,0,0,0,0,0,0,0,0,1000,
   "A plaintext BK7238 dump from a Tuya 2-gang wall touch switch (oem_t1_switch:1.1.4). It boots TuyaOS and initializes Wi-Fi."},
  {"BK7231N SBER Bulb E14 (SBDV-00117) decrypts flash and broadcasts BLE advertisement","firmwares/SBER_Bulb_E14_SBDV-00117.bin",c196_args,7,60,"",0,10,c196_exp,2,0,0,c196_pins,8,0,0,0,0,0,0,0,0,0,0,1000,
   "A hardware-encrypted BK7231N dump from an E14 SBER smart bulb. It decrypts via hardware cipher, starts Wi-Fi monitor mode, configures I2C pins P24/P26, and advertises BLE service data SBDV-00117."},
  {"BK7231N SBER Lamp RGBWW E27 (SBDV-00115) boots LibreTiny v1.0.2 and starts Soft-AP","firmwares/SBER_Lamp_RGBWW_E27_SBDV-00115.bin",c197_args,7,60,"",0,10,c197_exp,8,0,0,c197_pins,6,0,0,0,0,0,0,0,0,0,0,1000,
   "A hardware-encrypted BK7231N dump from a SBER RGBWW E27 lamp running LibreTiny v1.0.2. It initializes logger, configures flash preferences, and enables Soft-AP mode."},
  {"BK7231N SBER Relay (SBDV-00027) boots TuyaOS and initializes BLE SDK","firmwares/SBER_Relay_SBDV-00027.bin",c198_args,7,60,"",0,10,c198_exp,10,0,0,c198_pins,7,0,0,0,0,0,0,0,0,0,0,1000,
   "A hardware-encrypted BK7231N dump from a SBER smart relay. It decrypts via hardware cipher, initializes TuyaOS services, and brings up the RivieraWaves BLE controller."},
  {"BK7231N SBER Relay (SBDV-00050) boots TuyaOS and starts Wi-Fi monitor mode","firmwares/SBER_Relay_SBDV-00050.bin",c199_args,7,60,"",0,10,c199_exp,2,0,0,c199_pins,11,0,0,0,0,0,0,0,0,0,0,1000,
   "A hardware-encrypted BK7231N dump from a SBER smart relay. It decrypts via hardware cipher, initializes Wi-Fi monitor mode, and starts MQTT telemetry services."},
  {"BK7231T Tuya 2CH light switch (WB3S, 1.0.7) boots TuyaOS and inits BLE","firmwares/Tuya_2CH-Light_Switch_(WB_SW02_schemaID-000002m55m)_key7axydcvmea3x9_iygayy3dur6ccybl_WB3S_1.0.7.bin",c200_args,5,60,"",0,10,c200_exp,10,0,0,c200_pins,6,0,0,0,0,0,0,0,0,0,0,1000,
   "A Tuya-encrypted BK7231T dump from a 2-channel light switch. It initializes mqc app, timers, and the Tuya BLE stack."},
  {"BK7231T Tuya 3CH light switch (WB3S, 1.0.7) boots TuyaOS and inits BLE","firmwares/Tuya_3CH-Light_Switch_(WB_SW03_schemaID-000002m6pz)_key7axydcvmea3x9_wsg2wx9altyhxhee_WB3S_1.0.7.bin",c201_args,5,60,"",0,10,c201_exp,10,0,0,c201_pins,6,0,0,0,0,0,0,0,0,0,0,1000,
   "A Tuya-encrypted BK7231T dump from a 3-channel light switch. It initializes mqc app, timers, and the Tuya BLE stack."},
  {"BK7231T Tuya 4CH RF switch (WB3S, 1.0.7) boots TuyaOS and inits BLE","firmwares/Tuya_4CH_RF_Switch_(schemaID-000002gki4)_key7axydcvmea3x9_veiweudlsbxha7b7_WB3S_1.0.7.bin",c202_args,5,60,"",0,10,c202_exp,10,0,0,c202_pins,6,0,0,0,0,0,0,0,0,0,0,1000,
   "A Tuya-encrypted BK7231T dump from a 4-channel RF switch. It initializes mqc app, timers, and the Tuya BLE stack."},
  {"BK7231T Tuya 4-gang smart switch (WB3S, 1.0.7) boots TuyaOS and online log","firmwares/Tuya_4G_Smart_Switch_(schemaID-000002f1ti)_keya4gvchmtapm8n_xdqh5dxwhlsfm7fk_WB3S_1.0.7.bin",c203_args,5,60,"",0,10,c203_exp,9,0,0,c203_pins,6,0,0,0,0,0,0,0,0,0,0,1000,
   "A Tuya-encrypted BK7231T dump from a 4-gang smart switch. It initializes RTOS heap, sys_timer thread, and online log service."},
  {"BK7231T Tuya 902002 RGB+CCT bulb (WBLC9, 2.9.6) boots TuyaOS and mqc","firmwares/Tuya_902002-EU-2B_RGB+CCT_Bulb_(schemaID-000002j4ih)_keytg5kq8gvkv9dh_mg0bkk1pvum9oumc_WBLC9_2.9.6.bin",c204_args,5,60,"",0,10,c204_exp,10,0,0,c204_pins,6,0,0,0,0,0,0,0,0,0,0,1000,
   "A Tuya-encrypted BK7231T dump from a 902002 RGB+CCT smart bulb. It boots TuyaOS, initializes mqc app, and starts sys_timer."},
  {"BK7231T Tuya A60 E27 9W bulb (WB3L, 2.9.3) boots TuyaOS and registers mqc","firmwares/Tuya_A60_E27_9W_230V_(schemaID-000001zaaw)_keytg5kq8gvkv9dh_llb6ogbwtvlvtdvv_WB3L_2.9.3.bin",c205_args,5,60,"",0,10,c205_exp,10,0,0,c205_pins,6,0,0,0,0,0,0,0,0,0,0,1000,
   "A Tuya-encrypted BK7231T dump from an A60 9W smart bulb. It initializes mqc app callback and starts system timers."},
  {"BK7231T Tuya A60 RGBCWW smart bulb (WB2L, 1.1.2) boots TuyaOS","firmwares/Tuya_A60_Smart_Bulb_RGBCWW_(schemaID-00000225qx)_keytg5kq8gvkv9dh_mfud0amisxvhtkja_WB2L_1.1.2.bin",c206_args,5,60,"",0,10,c206_exp,5,0,0,c206_pins,6,0,0,0,0,0,0,0,0,0,0,1000,
   "A Tuya-encrypted BK7231T dump from an A60 RGBCWW smart bulb. It initializes RTOS heap and online log service."},
  {"BK7231T Tuya Alic RGB+CW lamp (WB3L, 1.1.7) boots TuyaOS","firmwares/Tuya_Alic_Lampara_RGB+CW_(schemaID-000002bi6n)_keytg5kq8gvkv9dh_u22tdsupmibn9o6a_WB3L_1.1.7.bin",c207_args,5,60,"",0,10,c207_exp,5,0,0,c207_pins,6,0,0,0,0,0,0,0,0,0,0,1000,
   "A Tuya-encrypted BK7231T dump from an Alic RGB+CW lamp. It boots TuyaOS, starts sys_timer, and loads user flash configuration."},
  {"BK7231T Tuya Calex UK plug (WB2S, 1.1.2) boots TuyaOS and mqc app","firmwares/Tuya_Calex_900011.1-v112_UKPlug_(5202000300_schemaID-000002j1v7)_keya4gvchmtapm8n_xpuiljbzmwjwakvw_WB2S_1.1.2.bin",c208_args,5,60,"",0,10,c208_exp,10,0,0,c208_pins,6,0,0,0,0,0,0,0,0,0,0,1000,
   "A Tuya-encrypted BK7231T dump from a Calex UK plug. It boots TuyaOS, initializes mqc app and registers system callbacks."},
  {"BK7231T Tuya Calex A60 RGBCW bulb (WB3L, 1.0.1) boots TuyaOS","firmwares/Tuya_Calex_A60_RGBCW_(schemaID-000001mvn0)_keyx7semxnwsecug_rk7scq9ybmlx3i4h_WB3L_1.0.1.bin",c209_args,5,60,"",0,10,c209_exp,5,0,0,c209_pins,6,0,0,0,0,0,0,0,0,0,0,1000,
   "A Tuya-encrypted BK7231T dump from a Calex A60 RGBCW smart bulb. It boots TuyaOS and starts online log service."},
  {"BK7231T Tuya CleverLife A60 RGBCW bulb (WB3L, 1.0.8) boots TuyaOS","firmwares/Tuya_CleverLife_A60_RGBCW_(schemaID-000001iaa6)_keytg5kq8gvkv9dh_jemt5kgdokncxwf7_WB3L_1.0.8.bin",c210_args,5,60,"",0,10,c210_exp,5,0,0,c210_pins,6,0,0,0,0,0,0,0,0,0,0,1000,
   "A Tuya-encrypted BK7231T dump from a CleverLife A60 RGBCW smart bulb. It initializes RTOS memory and online log service."},
  {"BK7231T Tuya DETA 6014HA switch (WB3S, 1.1.0) boots TuyaOS and mqc app","firmwares/Tuya_DETA_6014HA_Switch_(S702D-WF_schemaID-000001z6j8)_keya4gvchmtapm8n_taggrjpimvz6vn31_WB3S_1.1.0.bin",c211_args,5,60,"",0,10,c211_exp,10,0,0,c211_pins,6,0,0,0,0,0,0,0,0,0,0,1000,
   "A Tuya-encrypted BK7231T dump from a DETA 6014HA wall switch. It boots TuyaOS, initializes mqc app and registers callbacks."},
  {"BK7231T Tuya Daybetter strip lights (1.1.9) boots TuyaOS","firmwares/Tuya_Daybetter_Strip_lights_with_IR_and_Mic_(schemaID-000002ok5q)_keyexgh3aw8hjrcn_cxwfwenub0ahkhxr_1.1.9.bin",c212_args,5,60,"",0,10,c212_exp,10,0,0,c212_pins,6,0,0,0,0,0,0,0,0,0,0,1000,
   "A Tuya-encrypted BK7231T dump from Daybetter strip lights with IR and mic. It boots TuyaOS and initializes system timers."},
  {"BK7231N Tuya 72-087 module boots TuyaOS and brings up BLE stack","firmwares/BK7231N_QIO_72-087_2024-19-12-12-47-55.bin",c213_args,7,60,"",0,10,c213_exp,10,0,0,c213_pins,7,0,0,0,0,0,0,0,0,0,0,1000,
   "A Tuya-encrypted BK7231N dump from a 72-087 module. It initializes RTOS heap, creates system threads, and registers the BLE stack."},
  {"BK7231N Tuya Adelid MPD-B dry relay boots TuyaOS and inits BLE","firmwares/BK7231N_QIO_Adelid_DryRelay_MPD-B_2024-23-10-17-55-38.bin",c214_args,7,60,"",0,10,c214_exp,10,0,0,c214_pins,7,0,0,0,0,0,0,0,0,0,0,1000,
   "A Tuya-encrypted BK7231N dump from an Adelid MPD-B dry-contact relay. It boots TuyaOS and initializes the BLE controller."},
  {"BK7231N Tuya Aldi corner lamp boots TuyaOS and starts BLE","firmwares/BK7231N_QIO_AldiCornerLamp_2023-29-11-23-47-46.bin",c215_args,7,60,"",0,10,c215_exp,10,0,0,c215_pins,7,0,0,0,0,0,0,0,0,0,0,1000,
   "A Tuya-encrypted BK7231N dump from an Aldi corner lamp. It boots TuyaOS, creates worker threads, and brings up the BLE stack."},
  {"BK7231N Tuya AliExpress RGB 5V LED strip boots TuyaOS driver","firmwares/BK7231N_QIO_AliexpressLEDStripRGB5V_2026-17-1-15-53-42.bin",c216_args,7,60,"",0,10,c216_exp,8,0,0,c216_pins,7,0,0,0,0,0,0,0,0,0,0,1000,
   "A Tuya-encrypted BK7231N dump from an AliExpress 5V RGB LED strip. It enters driver_init and sets up peripheral channels."},
  {"BK7231N Tuya AVATTO N-WOT10 smart socket boots TuyaOS","firmwares/BK7231N_QIO_AVATTO N-WOT10-EU-W_2024-31-1-16-19-35.bin",c217_args,7,60,"",0,10,c217_exp,10,0,0,c217_pins,7,0,0,0,0,0,0,0,0,0,0,1000,
   "A Tuya-encrypted BK7231N dump from an AVATTO N-WOT10-EU-W wall socket. It boots TuyaOS, initializes RTOS heap, and starts BLE."},
  {"BK7231N Tuya AVATTO TS02-EU-B2 2-gang switch boots TuyaOS","firmwares/BK7231N_QIO_AVATTO TS02-EU-B2_2024-31-1-00-11-18.bin",c218_args,7,60,"",0,10,c218_exp,10,0,0,c218_pins,7,0,0,0,0,0,0,0,0,0,0,1000,
   "A Tuya-encrypted BK7231N dump from an AVATTO TS02 2-gang touch switch. It boots TuyaOS and brings up the BLE controller."},
  {"BK7231N Tuya AVATTO TS02-EU-W3 3-gang switch boots TuyaOS","firmwares/BK7231N_QIO_AVATTO TS02-EU-W3_2024-31-1-22-05-42.bin",c219_args,7,60,"",0,10,c219_exp,10,0,0,c219_pins,7,0,0,0,0,0,0,0,0,0,0,1000,
   "A Tuya-encrypted BK7231N dump from an AVATTO TS02 3-gang touch switch. It boots TuyaOS and brings up the BLE controller."},
  {"BK7231N Tuya brand new CB2S module boots TuyaOS factory firmware","firmwares/BK7231N_QIO_brand_new_CB2S_module_2024-07-6-22-17-00.bin",c220_args,7,60,"",0,10,c220_exp,10,0,0,c220_pins,7,0,0,0,0,0,0,0,0,0,0,1000,
   "A Tuya-encrypted BK7231N dump from a brand new CB2S module. It boots factory TuyaOS firmware and initializes BLE."},
  {"BK7231N Tuya brand new CB3S module boots TuyaOS factory firmware","firmwares/BK7231N_QIO_brand_new_module_cb3s_2024-05-6-18-20-59.bin",c221_args,7,60,"",0,10,c221_exp,10,0,0,c221_pins,7,0,0,0,0,0,0,0,0,0,0,1000,
   "A Tuya-encrypted BK7231N dump from a brand new CB3S module. It boots factory TuyaOS firmware and initializes BLE."},
  {"BK7231N Tuya CB2L TY-A60 18W bulb boots TuyaOS","firmwares/BK7231N_QIO_CB2L_TY-A60-18W-220V_C3009_2025-28-2-16-37-50.bin",c222_args,7,60,"",0,10,c222_exp,8,0,0,c222_pins,7,0,0,0,0,0,0,0,0,0,0,1000,
   "A Tuya-encrypted BK7231N dump from a TY-A60 18W bulb. It boots TuyaOS and initializes system timers."},
  {"BK7231N Tuya CB3S new module (2023) boots TuyaOS","firmwares/BK7231N_QIO_CB3S_NEW_MODULE_2023-28-10-16-35-35.bin",c223_args,7,60,"",0,10,c223_exp,10,0,0,c223_pins,7,0,0,0,0,0,0,0,0,0,0,1000,
   "A Tuya-encrypted BK7231N dump from a new CB3S module. It boots TuyaOS and starts BLE."},
  {"BK7231N Tuya CTree-001 smart device boots TuyaOS","firmwares/BK7231N_QIO_CTree-001_2024-13-12-00-04-07.bin",c224_args,7,60,"",0,10,c224_exp,10,0,0,c224_pins,7,0,0,0,0,0,0,0,0,0,0,1000,
   "A Tuya-encrypted BK7231N dump from a CTree-001 smart device. It boots TuyaOS and brings up system threads."},
  {"BK7231N Tuya EC79901 smart device boots TuyaOS","firmwares/BK7231N_QIO_EC79901_2025-11-6-18-19-31.bin",c225_args,7,60,"",0,10,c225_exp,6,0,0,c225_pins,7,0,0,0,0,0,0,0,0,0,0,1000,
   "A Tuya-encrypted BK7231N dump from an EC79901 smart device. It boots TuyaOS and initializes BLE."},
  {"BK7231N Tuya FS-05R fan switch boots TuyaOS","firmwares/BK7231N_QIO_FS-05R_2024-29-2-16-53-36.bin",c226_args,7,60,"",0,10,c226_exp,10,0,0,c226_pins,7,0,0,0,0,0,0,0,0,0,0,1000,
   "A Tuya-encrypted BK7231N dump from an FS-05R smart fan switch. It boots TuyaOS and starts BLE."},
  {"BK7231N Tuya MiBoxer FUT035W dual-white controller boots TuyaOS","firmwares/BK7231N_QIO_FUT035W_2024-05-1-16-21-24.bin",c227_args,7,60,"",0,10,c227_exp,10,0,0,c227_pins,7,0,0,0,0,0,0,0,0,0,0,1000,
   "A Tuya-encrypted BK7231N dump from a MiBoxer FUT035W dual-white LED controller. It boots TuyaOS and sets up PWM."},
  {"BK7231N Tuya MiBoxer FUT037W RGB controller boots TuyaOS","firmwares/BK7231N_QIO_FUT037W_2024-05-1-20-15-07.bin",c228_args,7,60,"",0,10,c228_exp,10,0,0,c228_pins,7,0,0,0,0,0,0,0,0,0,0,1000,
   "A Tuya-encrypted BK7231N dump from a MiBoxer FUT037W RGB LED controller. It boots TuyaOS and sets up PWM."},
  {"BK7231N Tuya 24W ceiling lamp boots TuyaOS","firmwares/BK7231N_QIO_lamp_24W_5001_2025-12-12-21-28-39.bin",c229_args,7,60,"",0,10,c229_exp,8,0,0,c229_pins,7,0,0,0,0,0,0,0,0,0,0,1000,
   "A Tuya-encrypted BK7231N dump from a 24W ceiling lamp. It boots TuyaOS and brings up BLE."},
  {"BK7231N Tuya RGB plafon lamp boots TuyaOS","firmwares/BK7231N_QIO_ledRGBPlafon_2024-08-12-00-21-57.bin",c230_args,7,60,"",0,10,c230_exp,8,0,0,c230_pins,7,0,0,0,0,0,0,0,0,0,0,1000,
   "A Tuya-encrypted BK7231N dump from an RGB ceiling plafon lamp. It boots TuyaOS and initializes PWM channels."},
  {"BK7231N Tuya LSC 3206306 CW + SM16703 controller boots TuyaOS","firmwares/BK7231N_QIO_LSC_3206306_CW_and_SM16703_2024-30-12-15-35-37.bin",c231_args,7,60,"",0,10,c231_exp,4,0,0,c231_pins,7,0,0,0,0,0,0,0,0,0,0,1000,
   "A Tuya-encrypted BK7231N dump from an LSC 3206306 LED strip with SM16703 driver. It boots TuyaOS and starts BLE."},
  {"BK7231N Tuya LSPA9 gray T34 BL0937 metering plug boots TuyaOS","firmwares/BK7231N_QIO_LSPA9_Gray_T34_BL0937_2025-18-3-19-35-58.bin",c232_args,7,60,"",0,10,c232_exp,10,0,0,c232_pins,7,0,0,0,0,0,0,0,0,0,0,1000,
   "A Tuya-encrypted BK7231N dump from an LSPA9 smart metering plug with BL0937. It boots TuyaOS and configures metering GPIOs."},
  {"BK7238 NiceMCU WB3S smart device boots and inits Wi-Fi monitor mode","firmwares/QIO_NiceMCU_WB3S_V1-piece2_2024-10-12-14-25-07.bin",c233_args,4,60,"",0,10,c233_exp,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A plaintext BK7238 dump from a NiceMCU WB3S smart device. It boots cleanly, starts up radio services, and enters Wi-Fi monitor mode."},
  {"BK7238 Tuya 1CH USB switch (T1-U-HL, 1.1.9) boots TuyaOS and timers","firmwares/Tuya_1CH_USB_(schemaID-e1n2x0u0)_keyfxuj3d5wg53wx_tj4ripw7ufzqh50f_T1-U-HL_1.1.9.bin",c234_args,4,60,"",0,10,c234_exp,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A plaintext BK7238 dump from a Tuya 1-channel USB smart switch (T1-U-HL module, v1.1.9). It initializes RTOS heap, system timers, and Wi-Fi power save."},
  {"BK7238 Tuya KWS-306WF power meter (T1-U-HL, 3.0.22) boots TuyaOS and TuyaMCU","firmwares/Tuya_KWS-306WF_Meter_(schemaID-e1mujb10)_key5s5pkkwnfwgvr_8afses1qqzgmof1w_TuyaMCU_T1-U-HL_3.0.22.bin",c235_args,5,60,"",0,10,c235_exp,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A plaintext BK7238 dump from a KWS-306WF DIN-rail smart energy meter (T1-U-HL module, v3.0.22). It initializes RTOS heap, timers, and the TuyaMCU upgrade subsystem."},
  {"BK7238 Tuya SRW-TH04 LCD temp/humidity (TuyaMCU, 3.0.22) boots TuyaOS","firmwares/Tuya_SRW-TH04_TempHumLCD_(schemaID-e1nfq7x4)_key5s5pkkwnfwgvr_j8otbggty0tjeow1_TuyaMCU_3.0.22.bin",c236_args,5,60,"",0,10,c236_exp,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A plaintext BK7238 dump from a Tuya SRW-TH04 LCD temperature and humidity meter (v3.0.22). It initializes RTOS heap, timers, and the TuyaMCU upgrade subsystem."},
  {"BK7238 Tuya Smart IR blaster (TuyaMCU, 1.0.2) boots TuyaOS and timers","firmwares/Tuya_Smart_IR_(schemaID-e1mqjcmc)_keycmhjv873dhvsn_ohmpsjqtvnanw6bf_TuyaMCU_1.0.2.bin",c237_args,5,60,"",0,10,c237_exp,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A plaintext BK7238 dump from a Tuya Smart IR blaster (v1.0.2). It initializes RTOS heap, system timers, and Wi-Fi power save."},
  {"BK7238 Tuya Xeefeel S1 (T1-M, 1.1.9) boots TuyaOS and timers","firmwares/Tuya_Xeefeel_S1_(ET-TI-M_schemaID-e1mw45eg)_keyfxuj3d5wg53wx_nvuyp0vl7cttr4cm_T1-M_1.1.9.bin",c238_args,4,60,"",0,10,c238_exp,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A plaintext BK7238 dump from a Tuya Xeefeel S1 device (T1-M module, v1.1.9). It initializes RTOS heap, system timers, and Wi-Fi power save."},
  {"BK7238 Tuya wall touch switch 1G D8FC9233E055 boots TuyaOS","firmwares/tuya-wall-touch-switch-1g-D8FC9233E055_1_backup_2026_12_4_18_59_58.bin",c239_args,4,60,"",0,10,c239_exp,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A plaintext BK7238 dump from a Tuya 1-gang wall touch switch. It boots TuyaOS, initializes system timers, and enters Wi-Fi power save."},
  {"BK7238 Tuya wall touch switch 2G D8FC920F973D boots TuyaOS","firmwares/tuya-wall-touch-switch-2g-D8FC920F973D_1_backup_2026_12_4_14_59_29.bin",c240_args,4,60,"",0,10,c240_exp,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A plaintext BK7238 dump from a Tuya 2-gang wall touch switch. It boots TuyaOS, initializes system timers, and enters Wi-Fi power save."},
  {"BK7238 Tuya wall touch switch 2G D8FC920FC678 boots TuyaOS","firmwares/tuya-wall-touch-switch-2g-D8FC920FC678_1_backup_2026_12_4_17_10_10.bin",c241_args,4,60,"",0,10,c241_exp,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A plaintext BK7238 dump from a Tuya 2-gang wall touch switch. It boots TuyaOS, initializes system timers, and enters Wi-Fi power save."},
  {"BK7231U E88 Drone Cam boots user_main and initializes Wi-Fi SSID","firmwares/E88_Drone_Cam_vt.201904251714.bin",c242_args,4,60,"",0,10,c242_exp,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A plaintext BK7231U dump from an E88 quadcopter drone camera module. It executes user_main, configures Wi-Fi monitor mode, and starts its AP network."},
  {"BK7252U MiniCam A13 boots RT-Thread and initializes calibration","firmwares/BK7252U_MiniCam_A13_raw_QFN48.bin",c243_args,4,60,"",0,10,c243_exp,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A plaintext BK7252 dump from an INO-A13 MiniCam (QFN48). It boots RT-Thread 3.1.0, initializes rwnxl wireless MAC layer, and runs RF calibration."},
  {"BL2028N Zemismart Matter ZML2 RGBCT (1.17) prints SetupQRCode","firmwares/Zemismart_Matter_ZML2_RGBCT_CSA23291MAT40804-24_UAM026_1.17.bin",c244_args,5,60,"",0,10,c244_exp,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A plaintext BL2028N dump from a Zemismart ZML2 Matter RGBCT smart bulb (v1.17). It boots Matter commissioning stack and prints its SetupQRCode."},
  {"BK7231N Uascent uHome Matter Bulb (CB2L, 3.2) decrypts and boots","firmwares/Uascent_uHome_Matter_Bulb_3684_CSA23207MAT40720-24_UAM022-V1.0_CB2L_3.2.bin",c245_args,7,60,"",0,10,c245_exp,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A hardware-encrypted BK7231N dump from a Uascent uHome Matter smart bulb (CB2L, v3.2). It decrypts via hardware cipher and initializes Wi-Fi monitor mode."},
  {"BK7231N SBER Bulb E14 Unit1 decrypts and broadcasts BLE advertisement","firmwares/SBER_Bulb_E14_SBDV-00117_Unit1.bin",c246_args,7,60,"",0,10,c246_exp,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A hardware-encrypted BK7231N dump from a SBER E14 smart bulb (Unit 1). It decrypts via hardware cipher, starts Wi-Fi monitor mode, and advertises SBDV-00117 over BLE."},
  {"BK7231N SBER Lamp E27 decrypts and reads flash storage","firmwares/SBER_Lamp_E27_SBDV-00019_1.23.0.bin",c247_args,7,60,"",0,10,c247_exp,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A hardware-encrypted BK7231N dump from a SBER E27 smart lamp (v1.23.0). It decrypts via hardware cipher, boots system services, and initializes Wi-Fi monitor mode."},
  {"BK7231N SBER Plug decrypts and initializes power control subsystem","firmwares/SBER_Plug_SBDV-00123_FW-1.21.5.bin",c248_args,7,60,"",0,10,c248_exp,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A hardware-encrypted BK7231N dump from a SBER smart plug (v1.21.5). It decrypts via hardware cipher, starts Wi-Fi monitor mode, and initializes power metering."},
  {"BK7231N SBER Lamp RGBWW E27 decrypts and boots TuyaOS/SBER stack","firmwares/SBER_Lamp_RGBWW_E27_SBDV-00115_FW-1.26.1.bin",c249_args,7,60,"",0,10,c249_exp,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A hardware-encrypted BK7231N dump from a SBER RGBWW E27 lamp (v1.26.1). It decrypts via hardware cipher, starts system services, and enters Wi-Fi monitor mode."},
  {"BK7231M ChinaMobile HeJiaQin X1S plug boots FreeRTOS","firmwares/ChinaMobile_HeJiaQin_HuiJu_X1S_Plug_CB2S_1.0.4.bin",c250_args,4,60,"",0,10,c250_exp,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A stock BK7231M dump from a China Mobile HeJiaQin HuiJu X1S smart plug (CB2S module, v1.0.4). It boots FreeRTOS, initializes heap memory and flash storage."},
  {"BK7231M VeSync Aubess BSD33 plug boots FreeRTOS","firmwares/VeSync_Aubess_BSD33_BSDOG02_Plug_1.0.01.bin",c251_args,4,60,"",0,10,c251_exp,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A stock BK7231M dump from an Aubess BSD33 smart socket plug (v1.0.01). It boots FreeRTOS and initializes system flash."},
  {"BK7231M VeSync GreenSun EU plug (1.0.01) boots FreeRTOS","firmwares/VeSync_GreenSun_EU_WHOGPLUG_CB2S_1.0.01.bin",c252_args,4,60,"",0,10,c252_exp,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A stock BK7231M dump from a GreenSun EU smart plug (CB2S, v1.0.01). It boots FreeRTOS, initializes memory heap and system flash."},
  {"BK7231M VeSync GreenSun EU plug (1.0.02) boots FreeRTOS","firmwares/VeSync_GreenSun_EU_WHOGPLUG_CB2S_1.0.02.bin",c253_args,4,60,"",0,10,c253_exp,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A stock BK7231M dump from a GreenSun EU smart plug (CB2S, v1.0.02). It boots FreeRTOS, initializes memory heap and system flash."},
  {"BK7231M SparkleIoT XH-CB2S mini switch boots FreeRTOS","firmwares/SparkleIoT_XH-CB2S_Mini_Switch.bin",c254_args,4,60,"",0,10,c254_exp,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A 4MB BK7231M dump from a SparkleIoT XH-CB2S smart mini relay switch. It boots FreeRTOS and configures on-chip flash."},
  {"BL2028N Dogness D07 smart water fountain boots TuyaOS","firmwares/Dogness_D07_410B8E_Water_Fountain_5.0.38.bin",c255_args,5,60,"",0,10,c255_exp,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A stock BL2028N dump from a Dogness D07 smart pet water fountain (v5.0.38). It boots TuyaOS and initializes memory heap."},
  {"BL2028N Dreo DR-HTF004S tower fan boots TuyaOS","firmwares/Dreo_DR-HTF004S_Fan_PAI-053.bin",c256_args,5,60,"",0,10,c256_exp,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A stock BL2028N dump from a Dreo DR-HTF004S smart tower fan (PAI-053 module). It boots TuyaOS and initializes memory heap."},
  {"BL2028N Dreo WH517S wall heater boots TuyaOS","firmwares/Dreo_Wall_Heater_WH517S_PAI-053_3.4.3.bin",c257_args,5,60,"",0,10,c257_exp,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A stock BL2028N dump from a Dreo WH517S smart wall heater (PAI-053 module, v3.4.3). It boots TuyaOS and initializes memory heap."},
  {"BL2028N XiaoMei WP8621N fish feeder boots TuyaOS and timers","firmwares/XiaoMei_WP8621N_Fish_Feeder_WIFI002_V20_WP8621NK_03.bin",c258_args,5,60,"",0,10,c258_exp,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A stock BL2028N dump from a XiaoMei WP8621N smart aquarium fish feeder. It boots TuyaOS, initializes flash and enters Wi-Fi power save."},
  {"BK7252N INO-A13N mini camera boots RT-Thread and inits FAL","firmwares/Cam_INO-A13N-V2.2_readResult_BK7252N_QIO_7252n_square_2025-28-9-17-39-25.bin",c259_args,4,60,"",0,10,c259_exp,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A stock BK7252N dump from an INO-A13N V2.2 mini camera. It boots RT-Thread, registers the FAL partition table, and enters Wi-Fi monitor mode."},
  {"BK7252N Pinmei A9 mini camera boots RT-Thread and inits FAL","firmwares/Pinmei_A9_BK7252N_BKFIL.bin",c260_args,4,60,"",0,10,c260_exp,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A stock BK7252N dump from a Pinmei A9 smart camera. It boots RT-Thread, mounts on-chip flash via FAL, and enters Wi-Fi monitor mode."},
  {"BK7252N Linklemo A9 mini camera boots RT-Thread and inits FAL","firmwares/XCThings_Linklemo_A9_INO-IPC-V2.3_(BluePCB).bin",c261_args,4,60,"",0,10,c261_exp,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A stock BK7252N dump from a Linklemo XCThings A9 smart IP camera (INO-IPC-V2.3). It boots RT-Thread and mounts FAL flash partitions."},
  {"BK7252U Linklemo HA15 camera boots RT-Thread OS","firmwares/Linklemo_XCThings_HA15_IN-A15-V1.0_INNOA15_BluePCB_12.12.16.bin",c262_args,4,60,"",0,10,c262_exp,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A stock BK7252 dump from a Linklemo XCThings HA15 camera (QFN48). It boots RT-Thread 3.1.0 and initializes rwnxl MAC."},
  {"BK7252U Naxclow A11-A9 camera boots RT-Thread OS","firmwares/Naxclow_MiniCam_A11-A9_B-V2.7_240426.bin",c263_args,4,60,"",0,10,c263_exp,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A stock BK7252 dump from a Naxclow A11-A9 mini camera (QFN48). It boots RT-Thread 3.1.0 and reads MAC from flash."},
  {"BK7252U T99 mini camera inits Wi-Fi reception","firmwares/T99_MiniCam_BK7252UQN48+GC0311-insmod.bin",c264_args,4,60,"",0,10,c264_exp,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A stock BK7252 dump from a T99 mini camera with GC0311 sensor. It boots, enters monitor mode, and transitions to normal Wi-Fi reception."},
  {"BK7252U Pix-Link A9 camera streams GC0328C video","firmwares/A9_Pix-Link_ΡΙΧΑ-180431-XLPNL_CYCAM_RW101P_v19_1010.bin",c265_args,4,60,"",0,10,c265_exp,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A stock BK7252 dump from a Pix-Link A9 camera. It initializes Wi-Fi and streams 320x240 JPEG frames via hardware video DMA."},
  {"BK7238 DX-Smart DX-WF24A boots and advertises WF24-BLE","firmwares/DX-Smart_DX-WF24A_AT.bin",c266_args,4,60,"",0,10,c266_exp,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A stock BK7238 dump from a DX-Smart DX-WF24A AT-command module. It starts radio services, enters monitor mode, and broadcasts WF24-BLE advertisements."},
  {"BK7238 eWeLink door sensor boots FreeRTOS and inits flash","firmwares/eWeLink_DoorWindow_(CK-BK7238-W105SE0X-01-DOOR-HB)_1.0.4.bin",c267_args,4,60,"",0,10,c267_exp,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A stock BK7238 dump from an eWeLink door and window contact sensor (v1.0.4). It boots FreeRTOS, initializes heap and system flash."},
  {"BK7238 Tuya Onforu 100W floodlight boots TuyaOS","firmwares/Tuya_Onforu_Floodlight100W_BDB100WF-3_(smart_cct-V10_schemaID-e1n0q1xk)_keytfesxxsqewnqy_unlndugueawgni1_T1-U-HL_1.0.43.bin",c268_args,4,60,"",0,10,c268_exp,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A stock BK7238 dump from an Onforu 100W smart LED floodlight (T1-U-HL module, v1.0.43). It boots TuyaOS, initializes heap, and enters Wi-Fi power save."},
  {"BK7238 Tuya PCIe remote boot card boots TuyaOS","firmwares/Tuya_Remote_Boot_PCIE_(WIFI远程开机卡_schemaID-e1n38ek0)_keygkhjdv8859wg5_jlicasd8h9mzckrz_T1-U-HL_1.0.4.bin",c269_args,4,60,"",0,10,c269_exp,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A stock BK7238 dump from a Tuya computer PCIe remote power boot card (T1-U-HL module). It boots TuyaOS, initializes heap, and enters Wi-Fi power save."},
  {"BK7238 Zengge W43-M5 ceiling light boots FreeRTOS","firmwares/Zengge_Ceiling_Light_W43-M5_V1.2_(3D_23_20250724_ZIL-BK38-M5-12)_v1.23.9335-A.bin",c270_args,4,60,"",0,10,c270_exp,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A stock BK7238 dump from a Zengge W43-M5 smart ceiling light. It boots FreeRTOS, initializes heap memory and flash configuration."},
  {"BK7238 Zengge ZG-BK38-BP101 RGBCCT bulb boots FreeRTOS","firmwares/Zengge_AK001-ZJ21412_35_243_20240425_ZG-BK38-BP101_RGBCCT_Bulb.bin",c271_args,4,60,"",0,10,c271_exp,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A stock BK7238 dump from a Zengge ZG-BK38-BP101 RGBCCT smart light bulb. It boots FreeRTOS, initializes heap and flash configuration."},
  {"BK7238 Zengge Symphony USB SPI LED boots FreeRTOS","firmwares/Zengge_AK001-ZJ21413_77_24_20240423_ZG-BK_Symphony_USB_SPI_LED.bin",c272_args,4,60,"",0,10,c272_exp,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A stock BK7238 dump from a Zengge Symphony addressable SPI LED light strip controller. It boots FreeRTOS, initializes heap and flash configuration."},
  {"BK7238 GBDQA 63V 16A smart breaker boots TuyaOS","firmwares/BK7238_QIO_GBDQA_63V_16A_2026-09-7-22-13-43.bin",c273_args,4,60,"",0,10,c273_exp,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A stock BK7238 dump from a GBDQA 63V 16A DIN-rail circuit breaker. It boots TuyaOS, initializes heap memory, and enters Wi-Fi power save."},
  {"BK7238 T1U PCIe PC card 2 boots TuyaOS","firmwares/BK7238_QIO_t1u_pci-e_PC_card_2_2026-13-6-19-42-48.bin",c274_args,4,60,"",0,10,c274_exp,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1000,
   "A stock BK7238 dump from a T1U PCIe computer power management card. It boots TuyaOS, initializes heap memory, and enters Wi-Fi power save."}
};
TestCase *g_cases = g_cases_arr;
int g_case_count = 275;


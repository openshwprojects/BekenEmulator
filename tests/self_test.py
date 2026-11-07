import subprocess
import threading
import time
import os
import sys
from datetime import datetime, timezone

# Firmware occasionally emits valid Unicode that the legacy Windows console
# code page cannot represent.  A failed assertion must still print its capture
# instead of replacing the useful failure with UnicodeEncodeError.
try:
    sys.stdout.reconfigure(errors="backslashreplace")
except (AttributeError, OSError):
    pass

# Get the path to the root directory
ROOT_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
MAIN_SCRIPT = os.path.join(ROOT_DIR, 'src', 'main.py')

# Register decoding lives in src/periph.py, shared with the GUI front-end.
sys.path.insert(0, ROOT_DIR)
from src import periph

# How many times a periodic line must repeat for the "1s timers" cases.
# Two is the sweet spot: a second tick already proves the timer REPEATS rather
# than merely starting, and each extra tick costs real wall time (emulation runs
# far slower than the clock - about 4 ticks per 90s here). Same everywhere so CI
# and local runs assert the same thing; override with REPEATS=n to dig deeper.
REPEATS = int(os.environ.get("REPEATS") or 2)

# Seconds to keep the emulator running AFTER a test has met all its markers.
# The verdict is already decided at that point, so this changes no pass/fail -
# it just keeps capturing output, giving the report more of the boot than the
# bare minimum, and letting periodic checks show how many ticks actually
# happened (e.g. 7/2) instead of stopping dead on the second one.
# Tune these two; env LINGER=n overrides both.
LINGER_SECONDS_CI = 5.0      # GitHub Actions: spend a little more for richer logs
LINGER_SECONDS_LOCAL = 0.0   # local runs stay as quick as before

LINGER = float(os.environ.get("LINGER") or
               (LINGER_SECONDS_CI if os.environ.get("CI") else LINGER_SECONDS_LOCAL))

# DRY Test definitions.
#
# Ordering matters: the boot tests below run the emulator until their timeout
# expires (the emulator never exits on its own), so each one costs its full
# timeout. The CLI tests come first because their process exits in about a
# second - a broken command line then fails the run in seconds instead of
# after twenty minutes of emulation.
TEST_CASES = [
    {
        # Guards parse_key(): an unusable key must be rejected up front with a
        # message naming the accepted forms, not fail deep inside emulation.
        "name": "CLI: invalid -key is rejected",
        "binary": os.path.join(ROOT_DIR, "firmwares", "OpenBK7231T_QIO_1.18.300.bin"),
        "args": ["--only-uart", "-key", "NOT_A_REAL_KEY"],
        "timeout": 60,
        "expected_strings": [
            "Invalid -key value",
            # The error lists the known key names and the accepted formats.
            "TUYA",
            "32 hex characters"
        ]
    },
    {
        # Guards the -chip lookup: an unknown chip must be rejected and the
        # known identities listed.
        "name": "CLI: invalid -chip is rejected",
        "binary": os.path.join(ROOT_DIR, "firmwares", "OpenBK7231T_QIO_1.18.300.bin"),
        "args": ["--only-uart", "-chip", "BK9999"],
        "timeout": 60,
        "expected_strings": [
            "Unknown -chip value",
            "BK7238",
            "BK7252N"
        ]
    },
    {
        # Guards the --ble-core layout choice: the model covers the BLE 5.1
        # (BK7231N/M) and 5.2 (BK7238) cores only. BK7252 has SDRAM where the
        # core would be, so it must be refused up front, not modelled over
        # the heap.
        "name": "CLI: --ble-core is rejected on a chip without a BLE 5.x core",
        "binary": os.path.join(ROOT_DIR, "firmwares", "OpenBK7252_QIO_1.18.300.bin"),
        "args": ["--only-uart", "-chip", "BK7252", "--ble-core"],
        "timeout": 60,
        "expected_strings": [
            "--ble-core models the BLE 5.x core",
            "this chip has no such core"
        ]
    },
    {
        # Guards the plaintext-vs-encrypted heuristic in crypto.py: running an
        # encrypted image with no key must warn that the slice is not ARM code
        # and suggest the key, instead of silently emulating garbage.
        "name": "CLI: encrypted image without a key warns",
        "binary": os.path.join(ROOT_DIR, "firmwares", "OpenBK7231T_QIO_1.18.300.bin"),
        "args": ["--only-uart"],
        "timeout": 60,
        "expected_strings": [
            "does not look like ARM code",
            "try: -key TUYA"
        ]
    },
    {
        # Guards parse_forwards(): a malformed port forward must be refused up
        # front with the expected shape, not crash once the network starts.
        "name": "CLI: malformed --net-forward is rejected",
        "binary": os.path.join(ROOT_DIR, "firmwares", "OpenBK7231T_QIO_1.18.300.bin"),
        "args": ["--only-uart", "-key", "TUYA", "--net-forward", "eighty"],
        "timeout": 60,
        "expected_strings": [
            "bad --net-forward 'eighty'",
            "want LPORT:DPORT",
        ]
    },
    {
        # Guards parse_adc_specs(): a malformed --adc spec must be refused up
        # front with the expected CH:VALUE shape before emulation starts.
        "name": "CLI: malformed --adc is rejected",
        "binary": os.path.join(ROOT_DIR, "firmwares", "OpenBK7231T_QIO_1.18.300.bin"),
        "args": ["--only-uart", "-key", "TUYA", "--adc", "P26"],
        "timeout": 60,
        "expected_strings": [
            "bad --adc 'P26'",
            "want CH:VALUE",
        ]
    },
    {
        # The Wi-Fi path end to end, IP traffic into the firmware's own stack.
        # --wifi-peer auto models the MAC (src/wifimac.py) and puts a virtual
        # station (src/wifipeer.py) plus a user-mode IP network
        # (src/netstack.py) on the far side of the air. OpenBeken with no
        # config raises its open AP; the peer sees its beacons, joins it, gets
        # a lease from the device's own DHCP server, pings it, and then sends
        # an HTTP GET to the device's command API. The echo argument showing
        # up in the device's UART log proves the request crossed 802.11 ->
        # LwIP -> OBK's HTTP server -> its command parser, and the logged
        # reply proves the answer made it back. Markers asserted in that
        # order are the stages that each needed a MAC-model fix: frmlen
        # without FCS (association), keyIndexValid in the RX descriptor (DHCP),
        # TX completion + TX trigger (every reply).
        #
        # The second half is the browser's route: --net-forward puts a real
        # listening socket on 127.0.0.1 (a free port, picked per run), and
        # once the device has answered the command the harness itself
        # connects to it and GETs /index with browser headers, exactly what
        # visiting http://127.0.0.1/ does. The page markers are text only the
        # real panel carries ("drivers active", "MQTT State:") - OpenBeken's
        # "Not found" page has the same title and footer, so the title alone
        # would prove nothing.
        "name": "OpenBK7231T Wi-Fi AP: virtual station joins, DHCP, ping, HTTP command runs, panel on localhost",
        "binary": os.path.join(ROOT_DIR, "firmwares", "OpenBK7231T_QIO_1.18.300.bin"),
        "args": ["--only-uart", "-key", "TUYA", "--wifi-peer", "auto",
                 "--net-http-get", "/cm?cmnd=echo%20HttpOverEmulatedWifiOK",
                 "--net-forward", "{free_port}:80"],
        "http_probe": {
            "after": "[NET] HTTP GET /cm?cmnd=echo%20HttpOverEmulatedWifiOK -> HTTP/1.1 200 OK",
            "path": "/index",
            "expect": ["drivers active", "MQTT State:", "version 1.18.300"],
        },
        "timeout": 900,
        "expected_strings": [
            "[WIFI] device acts as an access point (it sends beacons)",
            "[WIFI] found BSS 'OpenBK7231T_",
            "[WIFI] associated with 'OpenBK7231T_",
            "[NET] DHCP: we got 192.168.4.100/255.255.255.0 from the device (router 192.168.4.1)",
            "[NET] ping reply from 192.168.4.1",
            "Info:CMD:HttpOverEmulatedWifiOK",
            "[NET] HTTP GET /cm?cmnd=echo%20HttpOverEmulatedWifiOK -> HTTP/1.1 200 OK",
            "[NET] TCP forward: 192.168.4.100:",
            "[HTTP_PROBE] GET /index -> HTTP/1.1 200 OK",
            "[HTTP_PROBE] page contains: drivers active",
            "[HTTP_PROBE] page contains: MQTT State:",
            "[HTTP_PROBE] page contains: version 1.18.300",
        ]
    },
    {
        # Same path on a different silicon generation: BK7238 (-chip BK7238)
        # carries the same RivieraWaves MAC, so the model is unchanged. This
        # is the guard on "one method for all dumps" - a MAC-model change
        # that only suits the BK7231T image fails here.
        "name": "OpenBK7238 Wi-Fi AP: virtual station joins, DHCP, ping, HTTP command runs",
        "binary": os.path.join(ROOT_DIR, "firmwares", "OpenBK7238_QIO_1.18.300.bin"),
        "args": ["--only-uart", "-chip", "BK7238", "--wifi-peer", "auto",
                 "--net-http-get", "/cm?cmnd=echo%20BK7238HttpOverWifiOK"],
        "timeout": 1200,
        "expected_strings": [
            "[WIFI] found BSS 'OpenBK7238_",
            "[WIFI] associated with 'OpenBK7238_",
            "[NET] DHCP: we got 192.168.4.100/255.255.255.0 from the device (router 192.168.4.1)",
            "[NET] ping reply from 192.168.4.1",
            "Info:CMD:BK7238HttpOverWifiOK",
            "[NET] HTTP GET /cm?cmnd=echo%20BK7238HttpOverWifiOK -> HTTP/1.1 200 OK",
        ]
    },
    {
        # The other direction: OpenBeken as a Wi-Fi STATION. The image carries
        # an OBK config for an open network (tools/make_obk_config.py --ssid
        # BekenSim --mqtt-host mqtt.bekensim.test, plus the startup command the
        # next case exercises; the exact command is in the header of
        # run_OpenBK7231T_QIO_1.18.300_wifi_STA_mqttCapture.bat), so the
        # virtual peer plays the access point (--wifi-ssid BekenSim) and
        # src/netstack.py is the
        # network behind it: DHCP server, DNS, gateway. Offline by default -
        # the broker name resolves to a 10.200.x.x sinkhole and the firmware's
        # TCP connection is accepted and decoded, which is how a stock
        # firmware's cloud traffic is caught without letting it out. The
        # assoc/DHCP/connected markers need the RX rate fix: with an invalid
        # L-SIG code in the RX vector the connected station hit
        # ASSERT_ERR nxmac_ppdu_mcs_index_setf and wedged.
        "name": "OpenBK7231T Wi-Fi station: joins virtual AP, DHCP, HTTP in, MQTT out captured",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "OpenBK7231T_QIO_1.18.300_obkConfig_staOpen_BekenSim_netTraffic.bin"),
        "args": ["--only-uart", "-key", "TUYA", "--wifi-peer", "auto", "--wifi-ssid", "BekenSim",
                 "--net-http-get", "/cm?cmnd=echo%20StationHttpGetOK"],
        "timeout": 1200,
        "expected_strings": [
            "Info:MAIN:Using SSID [BekenSim]",
            "[WIFI] station c8:47:8c:00:00:00 associated to 'BekenSim'",
            "[NET] DHCP: device c8:47:8c:00:00:00 got 192.168.1.100",
            "WIFI_STA_CONNECTED",
            "Info:CMD:StationHttpGetOK",
            "[NET] HTTP GET /cm?cmnd=echo%20StationHttpGetOK -> HTTP/1.1 200 OK",
            "[NET] DNS mqtt.bekensim.test -> 10.200.",
            "MQTT CONNECT",
        ]
    },
    {
        # A PROTECTED network, which until now the emulator could not offer:
        # its access point was always open, so a device that had stored a
        # WPA2 network could see ours in a scan and never join it.
        # The image is configured (make_obk_config.py --ssid BekenSec --pass
        # simulator123) for exactly the network the peer raises, and both ends
        # hold the passphrase - the emulator from the command line, the
        # firmware from its own flash. The four-way handshake is how each
        # proves it, and nothing crosses the link until it finishes: the
        # address, the clock and the outgoing request below all happen on the
        # far side of it.
        # Nothing here weakens anything. No key is recovered or guessed; this
        # is the standard exchange between two parties that already share a
        # secret, and a station whose passphrase does not match gets no
        # further (src/wpa2.py covers that case).
        "name": "OpenBK7231T Wi-Fi station: joins a WPA2-protected network and uses it",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "OpenBK7231T_QIO_1.18.300_obkConfig_staWPA2_BekenSec.bin"),
        "args": ["--only-uart", "-key", "TUYA", "--wifi-peer", "auto",
                 "--wifi-ssid", "BekenSec", "--wifi-pass", "simulator123"],
        "timeout": 1200,
        "expected_strings": [
            "[WIFI] station c8:47:8c:00:00:00 associated to 'BekenSec' (AID 1)",
            "[WIFI] station c8:47:8c:00:00:00 associated to a protected network; "
            "starting the four-way handshake",
            "[WIFI] station c8:47:8c:00:00:00 completed the four-way handshake; "
            "the link is encrypted and open for traffic",
            # The firmware agrees it is connected, then uses the link.
            "WIFI_STA_CONNECTED",
            "[NET] DHCP: device c8:47:8c:00:00:00 got 192.168.1.100",
            "Info:NTP:Unix time:",
            "sends: HTTP GET /wpa2?ok=1 HTTP/1.1 host api.bekensim.test",
        ],
    },
    {
        # The device's own outgoing traffic, three protocols in one boot. The
        # image's OBK config (make_obk_config.py --ssid BekenSim --mqtt-host
        # mqtt.bekensim.test --mqtt-client obkSim) carries the startup command
        # "startDriver NTP; ntp_setServer 192.168.1.1; addRepeatingEvent 4 -1
        # sendGet http://api.bekensim.test/hello?dev=obk". Offline, each gets a
        # local answer: NTP from the host clock (OBK prints the time it
        # parsed), the GET from the offline HTTP responder - OBK's client
        # reports the whole 143-byte reply as received - and MQTT from the
        # offline broker: CONNECT, SUBSCRIBEs and the "connected = online"
        # PUBLISH, after which OBK reports the session up. The repeating GET
        # must be answered twice: a fresh connection, port and sequence each
        # time. "{pcap}" captures the air, and the capture is checked to hold
        # the same exchange in both directions.
        # The other MQTT direction too: --mqtt-publish has the broker push a
        # message to the device. It is held until the firmware subscribes to a
        # filter matching it ("cmnd/obkSim/+", which OBK chose itself), and
        # OBK then runs the command - so the "Info:CMD:" line below is the
        # device being controlled over MQTT, not just receiving bytes.
        "name": "OpenBK7231T Wi-Fi station: outgoing NTP, HTTP GET and MQTT answered offline, pcap holds them",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "OpenBK7231T_QIO_1.18.300_obkConfig_staOpen_BekenSim_netTraffic.bin"),
        "args": ["--only-uart", "-key", "TUYA", "--wifi-peer", "auto", "--wifi-ssid", "BekenSim",
                 "--mqtt-publish", "cmnd/obkSim/echo=MqttPushReachedTheFirmware",
                 "--pcap", "{pcap}", "--pcap-ip", "{pcap_ip}"],
        "timeout": 1200,
        "expected_strings": [
            "[NET] DHCP: device c8:47:8c:00:00:00 got 192.168.1.100",
            "[NET] NTP 192.168.1.100 -> 192.168.1.1 answered with the host clock",
            "Info:NTP:Unix time:",
            "[NET] DNS api.bekensim.test -> 10.200.",
            "sends: HTTP GET /hello?dev=obk HTTP/1.1 host api.bekensim.test",
            ("[NET] HTTP GET /hello?dev=obk HTTP/1.1 answered 200 (offline)", 2),
            "httpclient - no response buff, skipped 143",
            "[NET] DNS mqtt.bekensim.test -> 10.200.",
            "sends: MQTT CONNECT (v3.1.1, client id 'obkSim'",
            "connected to the offline broker",
            "[NET] MQTT SUBSCRIBE obkSim/+/set",
            "[NET] MQTT SUBSCRIBE cmnd/obkSim/+",
            "[NET] MQTT PUBLISH obkSim/connected = online",
            "Info:MQTT:mqtt_connection_cb: Successfully connected",
            "[NET] MQTT -> device PUBLISH cmnd/obkSim/echo = MqttPushReachedTheFirmware",
            "Info:CMD:MqttPushReachedTheFirmware",
        ],
        "pcap_expect": ["beacon", "authentication", "association request", "association response",
                        "data", "GET /hello?dev=obk", "BekenEmulator offline network",
                        "obkSim/connected"],
        # The same traffic at the IP level, which is what the GUI's details
        # window shows and what --pcap-ip writes: the frames themselves, both
        # directions, the bytes the device actually put on the wire.
        "ip_pcap_expect": ["GET /hello?dev=obk", "api.bekensim.test",
                           "BekenEmulator offline network", "obkSim", "cmnd/obkSim/echo"],
    },
    {
        # Snapshots (--snapshot-save / --snapshot-load), the core of it: the
        # emulator is deterministic - device time is the instruction count -
        # so a run resumed from a snapshot must print exactly what the
        # original run printed after the same point. MathDemo boots fast and
        # prints a quicktick line every few hundred thousand instructions, so
        # the comparison covers dozens of lines of timer, scheduler and UART
        # activity. See run_snapshot_roundtrip().
        "name": "Snapshot: resumed MathDemo run reproduces the original run",
        "kind": "snapshot_roundtrip",
        "binary": os.path.join(ROOT_DIR, "firmwares", "OpenBK7231T_QIO_1.18.300_mathDemo.bin"),
        "args": ["--only-uart", "-key", "TUYA"],
        "snapshot_at": 1500000,
        "min_compared_lines": 20,
        "timeout": 300,
        "expected_strings": [
            ("Info:MAIN:quicktick", 60),
        ]
    },
    {
        # The same guarantee for what the web-panel launcher relies on: a
        # snapshot of OpenBeken with its AP up and the Wi-Fi MAC modelled,
        # resumed with a fresh virtual peer attached. The MAC model, TRNG and
        # every descriptor pointer must survive the round trip, or the
        # resumed device associates, leases and answers ping differently.
        "name": "Snapshot: resumed OpenBK7231T Wi-Fi AP run reproduces the original run",
        "kind": "snapshot_roundtrip",
        "binary": os.path.join(ROOT_DIR, "firmwares", "OpenBK7231T_QIO_1.18.300.bin"),
        "args": ["--only-uart", "-key", "TUYA", "--wifi-peer", "auto"],
        "snapshot_at": 37000000,
        "min_compared_lines": 8,
        "timeout": 900,
        "expected_strings": [
            "[WIFI] associated with 'OpenBK7231T_",
            "[NET] DHCP: we got 192.168.4.100",
            "[NET] ping reply from 192.168.4.1",
        ]
    },
    {
        "name": "OpenBK7231T_QIO_1.18.300 Boot to MQTT and 1s timers",
        "binary": os.path.join(ROOT_DIR, "firmwares", "OpenBK7231T_QIO_1.18.300.bin"),
        "args": ["--only-uart", "-key", "TUYA"],
        # Boot finishes around 28M instructions and each Main_OnEverySecond tick
        # costs a few million more, so waiting for REPEATS ticks needs a longer
        # budget. Streaming stops as soon as the last one is seen.
        "timeout": 360,  # seconds
        "expected_strings": [
            "OpenBK7231T, version 1.18.300",
            "Main_Init_Delay",
            "Info:MQTT:MQTT_RegisterCallback called for",
            # Main_OnEverySecond runs off a FreeRTOS software timer - proves the
            # tick interrupt and the timer daemon task are alive.
            ", idle ",
            # Require the stable per-second line REPEATS times: the "Time N"
            # numbers can skip, but each Main_OnEverySecond prints this line
            # exactly once, so several of them prove the timer keeps firing, not
            # just starts. No WiFi in the emulator, so MQTT stays down and the
            # ping watchdog never starts. (string, count) asks for count matches.
            ("MQTT 0(0), bWifi 0, secondsWithNoPing -1", REPEATS),
            # With no SSID configured, Main_Init_Delay arms g_openAP = 5, so
            # five device-seconds in, Main_OnEverySecond calls
            # HAL_SetupWiFiOpenAccessPoint. These three lines are that HAL
            # path executing with the right values: the IP triple is the
            # compile-time APP_DRONE_DEF_NET_* constants, and the SSID prefix
            # is asserted WITHOUT its MAC-derived suffix (OpenBK7231T_8C000000
            # on this dump) so the check does not depend on which MAC a dump
            # carries. Output goes silent after this - bk_wlan_start() descends
            # into the SDK's RW MAC stack, which needs radio hardware the
            # emulator does not model - so this marks exactly how far the
            # WiFi bring-up gets.
            "no flash configuration, use default",
            "set ip info: 192.168.4.1,255.255.255.0,192.168.4.1",
            "ssid:OpenBK7231T_",
        ]
    },
    {
        "name": "MathDemo Boot and Float Verification",
        "binary": os.path.join(ROOT_DIR, "firmwares", "OpenBK7231T_QIO_1.18.300_mathDemo.bin"),
        "args": ["--only-uart", "-key", "TUYA"],
        "timeout": 120,
        "expected_strings": [
            "Info:MAIN:Advanced math test started",
            "Info:MAIN:Integer: (123 * 456) / 10 = 5608",
            "Info:MAIN:Float basic: (3.141590 * 1.860000) / (3.141590 - 1.0) = 2.728513",
            "Info:MAIN:Casting: float_to_int = 272, int_to_float = 1684.084106",
            # The mathDemo build logs every QuickTick - proves FreeRTOS software
            # timers fire (the same mechanism that drives Main_OnEverySecond).
            "Info:MAIN:quicktick",
            # No valid OBK config in flash -> default config path (the crafted
            # config test below is the complement: it must NOT print this).
            "CFG_InitAndLoad: Config crc or ident mismatch"
        ]
    },
    {
        # Bidirectional UART1: commands typed IN over the MCU/programming port
        # and executed - the receive counterpart of the uartSendHex case. Two
        # pieces make it work:
        #  - the injected config sets OBK flag 31
        #    (OBK_FLAG_CMD_ACCEPT_UART_COMMANDS), which brings up a command
        #    console on UART1 at boot. That flag lives in genericFlags at config
        #    offset 0x08 and is set by make_obk_config.py --flag 31.
        #  - --uart1-rx feeds two newline-separated lines into UART1's receive
        #    FIFO: "echo UartRxConsoleOK" then "nosuchcmd_uarttest". The emulator
        #    holds those bytes until the boot has run far enough that OBK has
        #    registered its console RX callback (it enables UART1 RX ~1M
        #    instructions before that, and the ISR discards RX with no callback),
        #    then delivers them via the RX FIFO + RX interrupt.
        # The console parses each line and runs it: the first is a valid command,
        # so echo logs its argument; the second is not a command, so OBK's
        # dispatcher looks it up, misses, and reports it NOT found. Together they
        # prove the whole receive path (FIFO, status, RX interrupt, ISR, ring
        # buffer, console) works end to end, and that a real parser - not a byte
        # reflector - runs: it accepts one line and rejects the next.
        "name": "UART1 Command Console: echo runs, unknown command rejected",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "OpenBK7231T_QIO_1.18.300_mathDemo_obkStartupCommand_uartConsole.bin"),
        "args": ["--only-uart", "-key", "TUYA", "--uart1-rx",
                 "echo UartRxConsoleOK\nnosuchcmd_uarttest"],
        "timeout": 300,
        "expected_strings": [
            "CFG_InitAndLoad: Correct config has been loaded",
            # Startup command ran (config + flags parsed) before the console.
            "Info:CMD:StartupBeforeConsole",
            # First UART1 line: a valid command - echo logs its argument.
            "Info:CMD:UartRxConsoleOK",
            # Second UART1 line, on its own newline: NOT a command, so OBK's
            # dispatcher looks it up, misses, and complains. Proves the console
            # kept reading past the first line and that a real parser (not a
            # byte reflector) runs - it accepts one line and rejects the next.
            "Error:CMD:cmd nosuchcmd_uarttest NOT found",
        ]
    },
    {
        # Same UART1 console, but proving OBK evaluates an arithmetic
        # argument up front. setChannel takes (channel, value); the value
        # here is the expression "12+28". OBK's command tokenizer resolves
        # simple math before the setChannel handler runs, so channel 5 is
        # set to 40 - not rejected as non-numeric, nor truncated to 12. The
        # channel-change log carries the resolved value, so "changed to 40"
        # is proof the expression was parsed and evaluated from the injected
        # UART1 line (something a byte reflector could never produce).
        "name": "UART1 Command Console: setChannel arg 12+28 evaluated to 40",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "OpenBK7231T_QIO_1.18.300_mathDemo_obkStartupCommand_uartConsole.bin"),
        "args": ["--only-uart", "-key", "TUYA", "--uart1-rx", "setChannel 5 12+28"],
        "timeout": 300,
        "expected_strings": [
            "CFG_InitAndLoad: Correct config has been loaded",
            # Config + flag 31 loaded (startup command ran before the console).
            "Info:CMD:StartupBeforeConsole",
            # setChannel received "12+28" and OBK evaluated it to 40 before
            # setting channel 5 - the resolved value shows in the change log.
            "Info:GEN:CHANNEL_Set channel 5 has changed to 40",
        ]
    },
    {
        # Closes the multi-command loop: a plain console line runs as ONE
        # command (OBK does not split on ';' - the adversarial check earlier
        # saw "echo A; echo B" echo the whole "A; echo B" as one argument).
        # backlog is the mechanism that splits a line on ';' and runs each
        # piece. Injected over UART1, "backlog echo BacklogOne; echo
        # BacklogTwo" runs BOTH echoes, so both arguments come back - proof
        # the console parses and dispatches multiple commands from one
        # received line.
        "name": "UART1 Command Console: backlog runs two commands from one line",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "OpenBK7231T_QIO_1.18.300_mathDemo_obkStartupCommand_uartConsole.bin"),
        "args": ["--only-uart", "-key", "TUYA", "--uart1-rx",
                 "backlog echo BacklogOne; echo BacklogTwo"],
        "timeout": 300,
        "expected_strings": [
            "CFG_InitAndLoad: Correct config has been loaded",
            "Info:CMD:StartupBeforeConsole",
            # backlog split the line on ';' and ran the first echo...
            "Info:CMD:BacklogOne",
            # ...and the second, from the same injected UART1 line.
            "Info:CMD:BacklogTwo",
        ]
    },
    {
        # End-to-end SARADC input test: --adc P23:2150 configures SARADC channel
        # 3 (P23 on BK7231T) to return raw sample 2150. Over the UART1 command
        # console, we bind P23 to channel 8 and set its role to ADC; on the next
        # per-second tick PIN_OnEverySecond() calls HAL_ADC_Read(23), which
        # triggers a SARADC conversion on channel 3, drains the emulated FIFO
        # via saradc_isr, and publishes 2150 to channel 8.
        "name": "UART1 Command Console: SetPinRole P23 ADC reads --adc P23:2150 into channel 8",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "OpenBK7231T_QIO_1.18.300_mathDemo_obkStartupCommand_uartConsole.bin"),
        "args": ["--only-uart", "-key", "TUYA", "--adc", "P23:2150", "--uart1-rx",
                 "backlog SetPinChannel 23 8; SetPinRole 23 ADC"],
        "timeout": 300,
        "tags": ["SARADC"],
        "expected_strings": [
            "CFG_InitAndLoad: Correct config has been loaded",
            "Info:CMD:StartupBeforeConsole",
            "Info:MQTT:Channel has changed! Publishing 2150 to channel 8",
        ]
    },
    {
        # Complement of the case above: the same mathDemo image, but with a
        # hand-crafted mainConfig_t written into the BK_PARTITION_NET_PARAM
        # partition (flash 0x1e1000). This proves the emulated flash controller
        # serves a full 32-byte page per operate-write - OBK pulls the whole
        # 3584-byte config through REG_FLASH_DATA_FLASH_SW (8 reads per page),
        # so a controller that repeats word 0 corrupts the config and it is
        # silently rejected as a crc mismatch.
        #
        # The stored crc byte is Tiny_CRC8 over config[4:sizeof] computed with
        # SIGNED char semantics (arithmetic >>), which is what the firmware's
        # own build does - the unsigned reading of the same code yields a
        # different byte and the config is refused.
        "name": "MathDemo Startup Command: echo",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "OpenBK7231T_QIO_1.18.300_mathDemo_obkStartupCommand_echo.bin"),
        "args": ["--only-uart", "-key", "TUYA"],
        "timeout": 180,
        "expected_strings": [
            # Config accepted - the mismatch branch was NOT taken.
            "CFG_InitAndLoad: Correct config has been loaded",
            # initCommandLine (offset 0x5E0) is "echo Test12343242343243";
            # CMD_Echo logs its argument under LOG_FEATURE_CMD.
            "Info:CMD:Test12343242343243"
        ]
    },
    {
        # Same config-injection trick, but the startup command drives OBK's own
        # uartSendHex to put bytes on UART1. This exercises the whole path end
        # to end: config load -> command registration (UART_AddCommands runs in
        # CMD_Init_Delayed, before the startup command is executed) -> HAL uart
        # write -> the emulator's UART1 hex capture. uartSendHex targets
        # UART_PORT_INDEX_0, which maps to BK_UART_1, so the bytes land on the
        # MCU link rather than the UART2 log.
        # This is a plain UART demo - the payload is arbitrary marker bytes, not
        # a TuyaMCU frame; TuyaMCU gets its own image and case.
        "name": "MathDemo Startup Command: uartSendHex on UART1",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "OpenBK7231T_QIO_1.18.300_mathDemo_obkStartupCommand_uartSendHex.bin"),
        "args": ["--only-uart", "--uart1-hex", "-key", "TUYA"],
        "timeout": 180,
        "expected_strings": [
            "CFG_InitAndLoad: Correct config has been loaded",
            # "backlog uartInit 115200; uartSendHex BADF00D12345"
            "[UART1/MCU] ba df 00 d1 23 45"
        ]
    },
    {
        # The suite's deepest TuyaMCU case: a whole two-way link, not just the
        # module talking into the void. Every other TuyaMCU case here stops at
        # "the firmware reached the link and sent something"; this one runs the
        # handshake, every data-point type, both mapping directions, and the
        # firmware's malformed-input recovery.
        #
        # "startDriver TuyaMCU" - the driver opens UART1 itself (TuyaMCU_Init
        # calls UART_InitUART(9600)), so no uartInit is needed. It then talks
        # unprompted: TuyaMCU_RunStateMachine_V3 starts with heartbeat_timer==0,
        # so the very first per-second tick emits a HEARTBEAT (cmd 0x00) without
        # the MCU ever having said anything. Frame is built by
        # TuyaMCU_SendCommandWithData: 55 AA <ver 00> <cmd> <lenHi> <lenLo>
        # <checksum>, checksum = 0xFF + cmd + lenHi + lenLo = 0xFF for a
        # zero-length heartbeat.
        #
        # The rest of the startup command is what a real TuyaMCU device is
        # configured with - data points bound to OpenBeken channels:
        #   linkTuyaMCUOutputToChannel 1 bool 1 / 2 val 2 / 4 enum 4
        #       MCU -> module: a reported data point must land on that channel.
        #   linkTuyaMCUOutputToChannel 20 bool 20
        #   addChangeHandler Channel1 != 0 setChannel 20 1
        #       module -> MCU: nothing ever reports DP 20, so the only way
        #       channel 20 moves is the change handler firing when DP 1 lands
        #       on channel 1 - and OBK must then SEND a 0x06 SET_DP frame.
        #       That closes the loop the other way round, off one MCU report.
        #
        # A simulated MCU is attached (--tuyamcu, src/tuyamcu.py) so the link
        # is a conversation: it answers the heartbeat, the product query and
        # the working-mode query. OpenBeken accepts the product record and
        # completes the handshake, which no stock TuyaOS image here does.
        # It goes one step further: it sends QUERY_STATE (0x08), and
        # --tuyamcu-dp gives the MCU one data point of EVERY wire type to
        # report back (bool, value, string, enum, bitmap). OBK parses each 0x07
        # report exactly, so the whole DP decoder is covered, not just two arms.
        #
        # --tuyamcu-inject is the MCU speaking on its own initiative once the
        # handshake is over. Real MCUs do this, and it reaches firmware paths a
        # pure question-and-answer link never touches:
        #   55AA0004000003          0x04 WiFiReset  -> OBK answers 0x04
        #   55AA001C00001B          0x1C SetTime    -> OBK answers with the time
        #   55AA0000000000          heartbeat with a WRONG checksum (0xFF is
        #                           correct) -> must be discarded, not acted on
        #   DEADBEEF55AA0099000098  four junk bytes, then an unknown command ->
        #                           the framer must resynchronise on 55 AA and
        #                           the unknown command must be reported, not
        #                           crash or desync the stream
        # The last two are the point of injecting RAW bytes rather than built
        # frames: a codec that can only build valid frames cannot test this.
        "name": "MathDemo Startup Command: TuyaMCU full link, data points and channels",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "OpenBK7231T_QIO_1.18.300_mathDemo_obkStartupCommand_tuyaMCU.bin"),
        "args": ["--only-uart", "--uart1-hex", "--tuyamcu",
                 "--tuyamcu-dp", "1:bool:1", "--tuyamcu-dp", "2:value:100",
                 "--tuyamcu-dp", "3:string:HELLO", "--tuyamcu-dp", "4:enum:2",
                 "--tuyamcu-dp", "5:bitmap:0x0F",
                 "--tuyamcu-inject", "55AA0004000003",
                 "--tuyamcu-inject", "55AA001C00001B",
                 "--tuyamcu-inject", "55AA0000000000",
                 "--tuyamcu-inject", "DEADBEEF55AA0099000098",
                 "-key", "TUYA"],
        "timeout": 420,
        "tags": ["TuyaMCU DP", "channels", "TuyaMCU errors"],
        "expected_strings": [
            "CFG_InitAndLoad: Correct config has been loaded",
            "Started TuyaMCU.",
            # The channel wiring from the startup command was accepted.
            "CMD_AddChangeHandler: added Channel1 with cmd setChannel 20 1",

            # --- the module talks first, with nothing having prompted it ---
            "[UART1/MCU] 55 aa 00 00 00 00 ff",
            "ProcessIncoming[v=0]: cmd 0 (Hearbeat) len 8",

            # --- handshake. With the simulated MCU answering, OpenBeken runs
            # the WHOLE sequence - unlike the stock images, it accepts the
            # product record and goes on to the working-mode query.
            "cmd 1 (QueryProductInformation)",
            'received {"p":"bekenemulator000","v":"1.0.0"}',
            "cmd 2 (MCUconf)",
            "ProcessIncoming: TUYA_CMD_MCU_CONF, TODO!",

            # --- query-state (0x08), then one 0x07 report per DP wire type.
            # Only the heartbeat keeps its [UART1/MCU] tag here: OBK sends
            # several frames back to back and the capture puts a whole burst on
            # one tagged line, so the rest assert the bytes alone.
            "55 aa 00 08 00 00 07",
            "ParseState: id 1 type 1-bool len 1",
            "ParseState: byte 1",
            "ParseState: id 2 type 2-val len 4",
            "ParseState: int32 100",
            "ParseState: id 3 type 3-str len 5",
            "ParseState: id 4 type 4-enum len 1",
            "ParseState: byte 2",
            "ParseState: id 5 type 5-bitmap len 1",
            "ParseState: byte 15",

            # --- MCU -> module: each mapped report reached its channel, with
            # the reported value intact (bool 1, int32 100, enum 2).
            "CHANNEL_Set channel 1 has changed to 1",
            "CHANNEL_Set channel 2 has changed to 100",
            "CHANNEL_Set channel 4 has changed to 2",

            # --- module -> MCU: channel 1 moving fires the change handler,
            # which sets channel 20, which OBK must push to the MCU as a 0x06
            # SET_DP. Payload dp 0x14 (20), type 01 (bool), len 0001, data 01;
            # checksum 0xFF+0x06+0x00+0x05 + 0x14+1+0+1+1 = 0x21.
            "executing command setChannel 20 1",
            "CHANNEL_Set channel 20 has changed to 1",
            "55 aa 00 06 00 05 14 01 00 01 01 21",

            # --- MCU-initiated frames: the firmware's own reply paths ---
            # OBK's 0x04 answer happens to be byte-identical to the 0x04 that
            # provoked it, so this looks like it could match the stimulus. It
            # cannot: the capture tags only what the DEVICE transmits, and it
            # prints lowercase, while OBK's log of what it RECEIVED is upper
            # case ("Received: 55 AA 00 04 ..."). Lowercase here means TX.
            "cmd 4 (WiFiReset) len 7",
            "ProcessIncoming: 0x04 replying",
            "55 aa 00 04 00 00 03",
            "cmd 28 (SetTime) len 7",
            "ProcessIncoming: received TUYA_CMD_SET_TIME, so sending back time",
            # No NTP in emulation, so the clock reads 0 and the reply carries
            # the epoch: valid flag 01, year 0x46 (70), month 01, day 01.
            "MCU time to set: 0",
            "55 aa 00 1c 00 08 01 46 01 01",

            # --- malformed input is rejected and the stream resynchronises ---
            "discarding packet bad expected checksum, expected 0 and got checksum 255",
            "Consumed 4 unwanted non-header byte in Tuya MCU buffer",
            "Skipped data (part) DE AD BE EF",
            "cmd 153 (Unknown) len 7",
            "ProcessIncoming: unhandled type 153",
        ]
    },
    {
        # Berry is OpenBeken's embedded scripting language (ENABLE_OBK_BERRY),
        # absent from the 1.18.300 image the other cases use, so this pulls the
        # dedicated 1.18.302 "berry" build. The startup command is a one-line
        # Berry snippet:  berry print("Hello " + str(5+2*2))
        # which exercises the VM end to end - integer arithmetic with operator
        # precedence (2*2 then +5 = 9), str() conversion, string concatenation,
        # and print(). Berry's print routes through be_writebuffer ->
        # ADDLOG_INFO(LOG_FEATURE_BERRY, ...), whose tag is "BERRY:", so a
        # correct evaluation shows up as exactly "Info:BERRY:Hello 9".
        #
        # OBK also runs its own "berry import autoexec" at boot, which fails
        # with "module 'autoexec' not found" because there is no user script -
        # that is expected and not asserted here.
        "name": "Berry Startup Command: print(\"Hello \" + str(5+2*2))",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "OpenBK7231T_QIO_1.18.302_berry_obkStartupCommand_berryHello.bin"),
        "args": ["--only-uart", "-key", "TUYA"],
        "timeout": 180,
        "expected_strings": [
            "CFG_InitAndLoad: Correct config has been loaded",
            # 5 + 2*2 = 9 -> str -> "Hello " + "9". The whole Berry VM in one line.
            "Info:BERRY:Hello 9",
        ]
    },
    {
        # Ground truth for the GPIO capture. Every other case can only show
        # whatever pin writes a firmware happens to make; here we ORDER a
        # specific pin to a known state and check the exact register word.
        #
        # "Rel" (not "Relay") is index 1 of htmlPinRoleNames[]; the role token
        # is only ever stricmp'd against that table, so a numeric index is
        # rejected with "Unknown role".
        #
        # Command order matters twice over:
        #  - SetPinChannel runs BEFORE SetPinRole so that the IOR_Relay setup,
        #    which immediately calls HAL_PIN_SetOutputValue(pin, channelValue),
        #    already sees the binding.
        #  - "SetChannel 1 0" precedes "SetChannel 1 1" to force a value
        #    TRANSITION: CMD_SetChannel passes iFlags=0, so CHANNEL_Set_Ex
        #    early-returns on prevValue == iVal and writes no GPIO at all.
        #
        # This works only because the startup command is executed from
        # Main_Init_AfterDelay_Unsafe, i.e. after g_enable_pins = 1 and
        # PIN_SetupPins() have run at the end of Main_Init_BeforeDelay_Unsafe.
        # Were it the other way round, the role would be stored and then
        # overwritten by the config pass.
        "name": "MathDemo Startup Command: SetPinRole drives relay pin P9",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "OpenBK7231T_QIO_1.18.300_mathDemo_obkStartupCommand_setPinRoleP9.bin"),
        "args": ["--only-uart", "-key", "TUYA"],
        "timeout": 180,
        "expected_strings": [
            "CFG_InitAndLoad: Correct config has been loaded",
            # GetChannel logs "Channel %i is %i" under LOG_FEATURE_CMD, which
            # proves the whole backlog ran rather than dying on argument one.
            "Info:CMD:Channel 1 is 1",
        ],
        # 0x02: bit1 = driven output level HIGH, bit3 = 0. Bit 3 is named
        # GCFG_OUTPUT_ENABLE_POS in the SDK header but is really an output
        # DISABLE - gpio_config() writes 0x00 for GMODE_OUTPUT and 0x0C/0x48
        # for the input and second-function modes. gpio_output() then
        # read-modify-writes bit 1 alone, giving 0x02 for a pin driving high.
        "expected_pins": {9: 0x02, 7: None},
    },
    {
        # The same thing on a different pin and a different channel. One pin
        # proving out could be a coincidence - some fixed write landing on the
        # captured index - so this twin moves BOTH the pin (9 -> 7) and the
        # channel (1 -> 2) and asserts the other pin stays untouched. Passing
        # both means the capture follows the command, which is the only reading
        # that supports using these numbers to check the emulator.
        "name": "MathDemo Startup Command: SetPinRole drives relay pin P7",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "OpenBK7231T_QIO_1.18.300_mathDemo_obkStartupCommand_setPinRoleP7.bin"),
        "args": ["--only-uart", "-key", "TUYA"],
        "timeout": 180,
        "expected_strings": [
            "CFG_InitAndLoad: Correct config has been loaded",
            "Info:CMD:Channel 2 is 1",
        ],
        "expected_pins": {7: 0x02, 9: None},
    },
    {
        # AlwaysHigh and AlwaysLow are the simplest possible output roles:
        # PIN_SetPinRoleForPinIndex calls HAL_PIN_Setup_Output then
        # HAL_PIN_SetOutputValue with a fixed 1 or 0, with no channel involved
        # at all. That makes them the cleanest check of the pin path itself.
        #
        # The AlwaysLow pin is the point of this case. 0x00 is a WRITTEN value
        # meaning "output driver enabled, driving low", and it is only
        # distinguishable from a pin nobody touched because the capture records
        # which registers were written. The inverted-bit-3 reading this decoder
        # started with rendered exactly this word as "configured, all functions
        # off", so a case asserting 0x00 would have caught that on its own.
        #
        # Pins 14 and 15 are plain GPIOs - no alias, no second function, and
        # clear of gpio_ops_filter which blocks GPIO20-23. Every other pin case
        # uses a pad that doubles as PWM or UART, so this one isolates the GPIO
        # path. Pin 16 is asserted untouched so "driving low" and "never
        # written" are shown to be different states rather than assumed to be.
        "name": "MathDemo Startup Command: AlwaysHigh and AlwaysLow pins",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "OpenBK7231T_QIO_1.18.300_mathDemo_obkStartupCommand_alwaysHighLow.bin"),
        "args": ["--only-uart", "-key", "TUYA"],
        "timeout": 180,
        "expected_strings": [
            "CFG_InitAndLoad: Correct config has been loaded",
            # No channel is involved, so there is no GetChannel to assert on;
            # the echo proves the backlog ran to the end rather than dying on
            # the first command.
            "Info:CMD:AlwaysPinsDone",
        ],
        "expected_pins": {14: 0x02, 15: 0x00, 16: None},
    },
    {
        # The PWM counterpart of the relay cases, and the only test where a
        # decoded FREQUENCY is known in advance. HAL_PIN_PWM_Start computes
        # period = 26000000 / freq, so an odd request like 1400 Hz gives
        # 18571 (0x488B) - a value nothing else in the system produces, which
        # is what makes a match meaningful rather than coincidental. Duty is
        # that period scaled by the channel value: 50% -> 9285 (0x2445).
        #
        # PWMFrequency must come FIRST: the IOR_PWM branch of
        # PIN_SetPinRoleForPinIndex reads g_pwmFrequency when it starts the
        # channel, so setting the frequency afterwards would not be picked up.
        #
        # Pin 9 is PWM index 3 (PIN_GetPWMIndexForPinIndex), and the pad goes
        # to second function (0x48 = GMODE_SECOND_FUNC) rather than being
        # driven as GPIO - which is the difference this case pins down against
        # the relay ones.
        "name": "MathDemo Startup Command: PWM 1400 Hz on pin 9",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "OpenBK7231T_QIO_1.18.300_mathDemo_obkStartupCommand_setPinRolePWM1400.bin"),
        "args": ["--only-uart", "-key", "TUYA"],
        "timeout": 180,
        "expected_strings": [
            "CFG_InitAndLoad: Correct config has been loaded",
            "Info:CMD:Channel 1 is 50",
        ],
        "expected_pins": {9: 0x48},
        # 26000000 // 1400 = 18571; 50% of that = 9285.
        "expected_pwm": {3: (0x488B, 0x2445)},
    },
    {
        # The 1400 Hz case proves the decode is right for ONE channel; this
        # moves every variable at once - pin 9 -> 7, PWM index 3 -> 1, channel
        # 1 -> 3, 1400 Hz -> 2500 Hz, 50% -> 25% duty - so a passing result
        # cannot come from a constant that happens to fit.
        #
        # It also discriminates the register STRIDE, which is the thing this
        # decoder got wrong for a long time. Under the confirmed stride-12
        # layout, PWM1's period sits at PWM_BASE + 0x08 + 12 = +0x94; under the
        # stride-8 reading this file used to assume, it would be at +0x90. Only
        # one of those can produce 0x28A0, so the case fails loudly if the
        # stride is ever regressed.
        "name": "MathDemo Startup Command: PWM 2500 Hz on pin 7",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "OpenBK7231T_QIO_1.18.300_mathDemo_obkStartupCommand_setPinRolePWM2500.bin"),
        "args": ["--only-uart", "-key", "TUYA"],
        "timeout": 180,
        "expected_strings": [
            "CFG_InitAndLoad: Correct config has been loaded",
            "Info:CMD:Channel 3 is 25",
        ],
        "expected_pins": {7: 0x48},
        # 26000000 // 2500 = 10400 exactly; 25% of that = 2600.
        "expected_pwm": {1: (0x28A0, 0x0A28)},
    },
    {
        # Two channels live at once, on the two highest PWM pads, at a
        # frequency an order of magnitude above the other cases.
        #
        # What this adds over the 1400/2500 pair:
        #  - P24 and P26 are PWM indices 4 and 5, the last two, so their period
        #    registers (+0xB8, +0xC4) sit near the top of the captured window;
        #    an off-by-one in the stride puts them outside it entirely.
        #  - the two pads share one frequency but carry DIFFERENT duties
        #    (10% and 75%), so period and duty cannot be confused for one
        #    another the way they were when this decoder read the packed
        #    layout with the wrong stride.
        #  - PWM_CTL should read 0x110000 - PWM4_EN (1<<16) and PWM5_EN
        #    (1<<20) together - which is the first multi-channel value the
        #    CTL cross-check has to handle.
        "name": "MathDemo Startup Command: PWM 10 kHz on pins 24 and 26",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "OpenBK7231T_QIO_1.18.300_mathDemo_obkStartupCommand_setPinRolePWM10000.bin"),
        "args": ["--only-uart", "-key", "TUYA"],
        "timeout": 180,
        "expected_strings": [
            "CFG_InitAndLoad: Correct config has been loaded",
            "Info:CMD:Channel 4 is 10",
            "Info:CMD:Channel 5 is 75",
        ],
        "expected_pins": {24: 0x48, 26: 0x48},
        # 26000000 // 10000 = 2600; 10% -> 260, 75% -> 1950.
        "expected_pwm": {4: (0x0A28, 0x0104), 5: (0x0A28, 0x079E)},
    },
    {
        # The N-family twin of the 1400/2500/10000 Hz cases: the same OBK
        # startup-command trick on the OpenBK7231N image, whose HAL lands in
        # the pwm_new block (0x802B00, T1..T4 edge registers) instead of the
        # T-family period/duty pair. All stock-firmware pwm_new evidence is
        # bulbs at their shipped frequencies; here WE choose 3700 Hz and a
        # 40% duty, so the expected registers exist nowhere else:
        #   T4 = 26000000 / 3700 = 7027 = 0x1B73 (period)
        #   T1 = 40% of 7027    = 2810 = 0x0AFA (toggle, init level high)
        # A duty reconstructed by toggle-replay coming out at exactly 40% is
        # the strongest single check the pwm_new decoder has.
        "name": "OpenBK7231N Startup Command: PWM 3700 Hz on pin 8 (pwm_new)",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "OpenBK7231N_QIO_1.18.300_obkStartupCommand_setPinRolePWM3700.bin"),
        "args": ["--only-uart", "-key", "TUYA"],
        "timeout": 240,
        "expected_strings": [
            "CFG_InitAndLoad: Correct config has been loaded",
            "Info:CMD:Channel 1 is 40",
        ],
        "expected_pins": {8: 0x48},
        "expected_pwm": {2: (0x1B73, 0x0AFA)},
    },
    {
        # Two pwm_new channels with DIFFERENT commanded duties - the corner no
        # real firmware covers: every stock N dump leaves its sub-channel-1
        # slots (ch1/ch3/ch5) at 0%, so the toggle-replay decode for the upper
        # half of each group had only ever been checked against zero. Here
        # both driven channels ARE sub-channel 1 (P7 = ch1 in group 0, P9 =
        # ch3 in group 1), at 25% and 70% of a 4500 Hz period:
        #   T4 = 26000000 / 4500 = 5777 = 0x1691 (both)
        #   ch1 T1 = 25% -> 1444 = 0x05A4 ; ch3 T1 = 70% -> 4043 = 0x0FCB
        # Distinct duties on distinct groups also rule out one channel's
        # registers being read through the other's offsets.
        "name": "OpenBK7231N Startup Command: dual PWM 25%/70% on pins 7 and 9",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "OpenBK7231N_QIO_1.18.300_obkStartupCommand_setPinRolePWMdual.bin"),
        "args": ["--only-uart", "-key", "TUYA"],
        "timeout": 240,
        "expected_strings": [
            "CFG_InitAndLoad: Correct config has been loaded",
            "Info:CMD:Channel 1 is 25",
            "Info:CMD:Channel 2 is 70",
        ],
        "expected_pins": {7: 0x48, 9: 0x48},
        "expected_pwm": {1: (0x1691, 0x05A4), 3: (0x1691, 0x0FCB)},
    },
    {
        "name": "OpenBK7231U_QIO_1.18.300 Boot to 1s timers",
        "binary": os.path.join(ROOT_DIR, "firmwares", "OpenBK7231U_QIO_1.18.300.bin"),
        # Plaintext image (beken_freertos_sdk layout) - no key.  The explicit
        # U variant enables its GPIO32..39 register bank despite sharing the
        # generic BK7231 chip/device ID.
        "args": ["--only-uart", "-chip", "BK7231U"],
        "timeout": 180,
        "expected_strings": [
            "OpenBK7231U, version 1.18.300",
            ", idle ",
            # Require the per-second line 10x: each Main_OnEverySecond prints it
            # once, so ten proves the timer keeps firing (the "Time N" numbers
            # themselves can skip). (string, count) asks for count occurrences.
            ("MQTT 0(0), bWifi 0, secondsWithNoPing -1", REPEATS)
        ]
    },
    {
        "name": "OpenBK7238_QIO_1.18.300 Boot to 1s timers",
        "binary": os.path.join(ROOT_DIR, "firmwares", "OpenBK7238_QIO_1.18.300.bin"),
        # Plaintext image; needs the BK7238 chip identity (bk_check_chip_id).
        "args": ["--only-uart", "-chip", "BK7238"],
        "timeout": 180,
        "expected_strings": [
            "OpenBK7238, version 1.18.300",
            ", idle ",
            # Require the per-second line 10x: each Main_OnEverySecond prints it
            # once, so ten proves the timer keeps firing (the "Time N" numbers
            # themselves can skip). (string, count) asks for count occurrences.
            ("MQTT 0(0), bWifi 0, secondsWithNoPing -1", REPEATS)
        ]
    },
    {
        # BK7231N now reaches the per-second Main_OnEverySecond loop. It used
        # to hang there: Main_OnEverySecond's first call reads the chip
        # temperature via the SARADC and blocks on the SARADC interrupt (ICU
        # bit 11), which the emulator now models. N (unlike T) unmasks bit 11,
        # so this is the case that guards the SARADC model.
        "name": "OpenBK7231N_QIO_1.18.300 Boot to 1s timers",
        "binary": os.path.join(ROOT_DIR, "firmwares", "OpenBK7231N_QIO_1.18.300.bin"),
        "args": ["--only-uart", "-key", "TUYA"],
        "timeout": 180,
        "expected_strings": [
            "OpenBK7231N, version 1.18.300",
            "calibration_main over",
            "app_init finished",
            # Full OpenBeken init completed.
            "Info:MAIN:Main_Init_After_Delay done",
            # The temperature read (SARADC) inside Main_OnEverySecond completes.
            ", idle ",
            # Require the per-second line 10x: each Main_OnEverySecond prints it
            # once, so ten proves the timer keeps firing (the "Time N" numbers
            # themselves can skip). (string, count) asks for count occurrences.
            ("MQTT 0(0), bWifi 0, secondsWithNoPing -1", REPEATS)
        ]
    },
    {
        # BK7231M is the BK7231N build stored UNENCRYPTED (verified: the two app
        # images are byte-identical for their first 828KB, and this one prints
        # the "OpenBK7231N" banner). Runs with no -key - the regression guard
        # for the plaintext path through crypto.py on a BK7231N-family image.
        # With the SARADC model it reaches the per-second loop like N.
        "name": "OpenBK7231M_QIO_1.18.300 Boot to 1s timers (no key)",
        "binary": os.path.join(ROOT_DIR, "firmwares", "OpenBK7231M_QIO_1.18.300.bin"),
        "args": ["--only-uart"],
        "timeout": 180,
        "expected_strings": [
            # M ships the N build, so the banner really does say 7231N.
            "OpenBK7231N, version 1.18.300",
            "calibration_main over",
            "app_init finished",
            # Full OpenBeken init completed.
            "Info:MAIN:Main_Init_After_Delay done",
            ", idle ",
            # Require the per-second line 10x: each Main_OnEverySecond prints it
            # once, so ten proves the timer keeps firing (the "Time N" numbers
            # themselves can skip). (string, count) asks for count occurrences.
            ("MQTT 0(0), bWifi 0, secondsWithNoPing -1", REPEATS)
        ]
    },
    {
        "name": "OpenBK7252_QIO_1.18.300 Boot to 1s timers",
        "binary": os.path.join(ROOT_DIR, "firmwares", "OpenBK7252_QIO_1.18.300.bin"),
        # Plaintext image; needs the BK7252 chip identity (bk_check_chip_id).
        "args": ["--only-uart", "-chip", "BK7252"],
        "timeout": 480,  # heaviest boots; 240s left too little headroom on a slow CI runner
        "expected_strings": [
            "OpenBK7252, version 1.18.300",
            ", idle ",
            # Require the per-second line 10x: each Main_OnEverySecond prints it
            # once, so ten proves the timer keeps firing (the "Time N" numbers
            # themselves can skip). (string, count) asks for count occurrences.
            ("MQTT 0(0), bWifi 0, secondsWithNoPing -1", REPEATS)
        ]
    },
    {
        "name": "OpenBK7252N_QIO_1.18.300 Boot to 1s timers",
        "binary": os.path.join(ROOT_DIR, "firmwares", "OpenBK7252N_QIO_1.18.300.bin"),
        # Plaintext image; needs the BK7252N chip identity (bk_check_chip_id).
        "args": ["--only-uart", "-chip", "BK7252N"],
        "timeout": 480,  # heaviest boots; 240s left too little headroom on a slow CI runner
        "expected_strings": [
            "OpenBK7252N, version 1.18.300",
            ", idle ",
            # Require the per-second line 10x: each Main_OnEverySecond prints it
            # once, so ten proves the timer keeps firing (the "Time N" numbers
            # themselves can skip). (string, count) asks for count occurrences.
            ("MQTT 0(0), bWifi 0, secondsWithNoPing -1", REPEATS)
        ]
    },
    {
        # 4MB dump; guards two emulator fixes at once:
        #  - SPI-flash-mirror size cap in setup() (a >2MB image used to map past
        #    RAM_BASE and throw UC_ERR_MAP before any code ran -> 0 output).
        #  - XVR (RF transceiver) 0x900100 transaction register: RF init spins
        #    on its bit-31 "busy" flag; without the model it hangs after
        #    xvr_reg_init, before "enter normal mode".
        "name": "BK7238 Sonoff 4MB Dump Boots (SPI mirror + XVR)",
        "binary": os.path.join(ROOT_DIR, "firmwares", "Sonoff_S61s_EUPlug_WBBK_01P_V1.3.bin"),
        "args": ["--only-uart", "-chip", "BK7238"],
        # "enter normal mode" lands around the 3-minute mark on a typical
        # machine; keep headroom so the last marker is not timing-flaky.
        "timeout": 240,
        # Markers form a boot ladder so a failure pinpoints the stage:
        "expected_strings": [
            # Proves the 4MB image maps and code executes (SPI mirror cap).
            "bk_misc_init_start_type",
            "[Flash]init over",
            # Early SDK banner of this build.
            "SDK Rev: 3.0.70",
            # Wifi init reached; the -chip BK7238 identity is served.
            "chip id=7238 device id=21128000",
            # Past the XVR transaction wait loop: RF cal completes, the SDK
            # brings up all threads and goes operational.
            "calibration_main over",
            "enter normal mode"
        ]
    },
    {
        # BK7231Q original Tuya firmware (MOES 1-gang relay). Plaintext image,
        # no key. Runs the full Tuya IoT SDK v2.3.1 - unlike the OpenBeken
        # builds this is stock vendor firmware. Its UART log goes quiet at the
        # TCP/IP banner, so exact-PC checkpoints prove what the log cannot:
        # tcpip_init returns, then the Wi-Fi supplicant and core tasks run.
        # After that this factory-state unit waits normally for external
        # manufacturing/provisioning input.
        "name": "BK7231Q Tuya MOES Relay Boot",
        "binary": os.path.join(ROOT_DIR, "firmwares", "BK7231Q_Tuya_MOES_Relay_WA2_1.1.3.bin"),
        "args": ["--only-uart",
                 # Instruction immediately after tcpip_init().
                 "--trace-pc", "0x52c48",
                 # Entry points passed to xTaskCreate for wpas/core_thread.
                 "--trace-pc", "0x4b0b8", "--trace-pc", "0x1283c"],
        "timeout": 120,
        "expected_strings": [
            "TUYA IOT SDK V:2.3.1",
            # The build identifies itself as a 1-switch BK7231 OEM app.
            "oem_bk7321_bk_1_switch:1.1.3",
            # Device brings up its pin map (relay on pin 18, button on pin 6).
            "IO - relay[0]:",
            "Initializing TCP/IP stack",
            # These are all past the final UART line. They distinguish a
            # healthy external-input wait from a hang inside tcpip_init.
            "[EMU_PC] reached 0x00052c48",
            "[EMU_PC] reached 0x0004b0b8",
            "[EMU_PC] reached 0x0001283c",
        ]
    },
    {
        # BL2028N is BK7231N silicon rebadged: this dump self-reports
        # "chip id=7231a device id=18520001" - the exact BK7231 default identity
        # - so it runs with no -key and the default -chip. The old version of
        # this case stopped at rwble_hl_init: that is the hand-off to the
        # RivieraWaves controller, not a completed boot. With --ble-core the
        # controller supplies its FIQs and HCI completions, so the real fan app
        # starts, publishes its BLE name and raises its Wi-Fi pairing AP. A
        # virtual station joins that AP and proves the device's DHCP/IP side is
        # alive too. (Ordinary Tuya BL2028N dumps are plaintext; the Uascent
        # Matter builds are custom-keyed and are intentionally not used here.)
        "name": "BL2028N Dreo fan clears BLE gate and serves its pairing AP",
        "binary": os.path.join(ROOT_DIR, "firmwares", "BL2028N_Dreo_DR-HTF004S_Fan_PAI-053.bin"),
        "args": ["--only-uart", "--ble-core", "--wifi-peer", "auto",
                 "--pcap", "{pcap}"],
        "timeout": 180,
        "tags": ["BLE", "BLE core", "FIQ"],
        "expected_strings": [
            # Proves BL2028N presents as BK7231N.
            "chip id=7231a device id=18520001",
            "calibration_main over",
            # The old endpoint. Everything below is on the far side of it.
            "rwble_hl_init ok",
            "BLE Rev: B5-3.0.46-P0",
            "gapm_cmp_evt:BLE_STACK_OK",
            # The product application is alive and stages its own BLE identity.
            "[BLE_ADV]   Complete Name",
            "DREOtf04sbe",
            "iot_uart_read begin",
            # It then starts AP-mode provisioning; the virtual phone-side peer
            # joins, receives a lease from the firmware and reaches its IP stack.
            "[WIFI] device acts as an access point (it sends beacons): peer takes the station role",
            "[WIFI] associated with 'DREOtf04sbe' (AID 1)",
            "[NET] DHCP: we got 192.168.0.100/255.255.255.0 from the device (router 192.168.0.1)",
            "[NET] ping reply from 192.168.0.1 (seq 1)",
        ],
        "pcap_expect": ["beacon", "authentication", "association request",
                        "association response", "data", "DREOtf04sbe"],
    },
    {
        # Original Tuya TuyaMCU firmware (BLE+WiFi fan switch). This dump's
        # TCP/IP init drives a hardware crypto/accelerator at 0x810000 (set a
        # busy bit, spin until it clears); without that block modelled the boot
        # hangs at "Initializing TCP/IP stack". These markers are all PAST that
        # stall - BLE stack bring-up and BLE-netcfg advertising - so this case
        # guards the 0x810000 / 0x81001c accelerator model. -key TUYA, and it is
        # the deepest-booting original-Tuya dump in the suite.
        #
        # A simulated MCU is attached to UART1 (--tuyamcu, src/tuyamcu.py): it
        # answers the module's frames, so the link is a conversation rather than
        # a monologue. The heartbeat ACK unblocks the next step - QUERY_PRODUCT
        # (0x01) - which the device never sends with nothing on the wire.
        # Verified A/B over a shared EMULATED-INSTRUCTION budget (wall-clock
        # comparison flakes; the frame lands near the cut-off):
        #    with peer: heartbeat @18.4M insns, 0x01 @18.9M, 0x02 @19.1M
        #    without  : heartbeat @18.4M insns, no 0x01 through 30.9M
        # Only one heartbeat is asserted, not a repeat: once the peer answers,
        # the device advances into the query loop instead of idling on
        # heartbeats, so progress through the handshake is the liveness signal.
        # This 1.1.71 SDK is the only stock image that ACCEPTS our product
        # record (no 'prod len' rejection) and advances again, to the
        # working-mode query MCU_CONF (0x02) - the furthest any stock dump gets.
        "name": "Tuya TMWF02 TuyaMCU Boots past crypto accel",
        "binary": os.path.join(ROOT_DIR, "firmwares", "BK7231T_Tuya_TMWF02_Fan_Switch_TuyaMCU_1.1.71.bin"),
        # --uart1-hex keeps any TuyaMCU 55AA bytes off the UART2 log stream the
        # markers match, and exercises the dual-UART feature.
        "args": ["--only-uart", "--uart1-hex", "--tuyamcu", "-key", "TUYA"],
        "timeout": 300,
        "expected_strings": [
            "Initializing TCP/IP stack",
            # Past the 0x810000 crypto-accel stall: BLE host stack comes up.
            "STACK INIT OK",
            "CREATE DB SUCCESS",
            # BLE network-config advertising starts.
            "appm start advertising",
            # The MCU link is up and the device talks first.
            "[UART1/MCU] 55 aa 00 00 00 00 ff",
            # Peer-unblocked: the product query, never sent without a peer.
            "55 aa 00 01 00 00 00",
            # Accepted our reply and moved on to the working-mode query.
            "55 aa 00 02 00 00 01",
        ]
    },
    {
        "name": "Woox Tuya Original Firmware Boot",
        "binary": os.path.join(ROOT_DIR, "firmwares", "BK7231T_QIO_Woox_R5111_2023-14-10-23-46-06.bin"),
        "args": ["--only-uart", "-key", "TUYA"],
        # Reaches the Wi-Fi scan now (deeper than the old key-read stall), so it
        # needs a larger budget; streaming stops as soon as the markers are hit.
        "timeout": 300,
        # Timestamps are stripped from the expected strings: the RTOS tick now
        # advances Tuya's clock, so lines print at 18:12:15/16/... depending on
        # emulation timing.
        "expected_strings": [
            # Tuya keeps its "protected" key block at flash 0x1ee000, which is
            # above the end of the CRC-stripped (logical) image - a 2MB physical
            # dump yields only ~1.88MB of logical space. The emulator used to
            # answer 0xFF there, so the firmware saw blank flash and took the
            # "init key:" path (simple_flash.c:486), inventing a key. Serving the
            # dump's real bytes makes it retrieve the stored one instead, which
            # is what the hardware does; assert that stronger behaviour.
            "simple_flash.c:432] key_addr: 0x1ee000",
            "simple_flash.c:500] get key:",
            "0xcb 0x4e 0x3e 0xa4 0x0 0x30 0x9d 0xab 0x65 0x6d 0x8d 0xbf 0xe4 0xb9 0x3f 0x35",
            "TUYA Notice][tuya_main.c:311] **********[oem_bk7231s_light_ty] [2.9.6] compiled at Oct 29 2020 14:38:00**********",
            # Boot used to die right after the key read, spinning on a threshold
            # poll of the 0x810020 accelerator result register (never served, so
            # the value stayed 0 and the loop's "> 0x1d3" test was never met).
            # With that register served, Woox runs on through activation, the
            # SDK banner, BLE pairing advertise and the start of the Wi-Fi scan -
            # the markers below prove each of those stages is now reached.
            "tuya_main.c:341] mf_init succ",
            "< TUYA IOT SDK V:1.0.2 BS:40.00_PT:2.2_LAN:3.3_CAD:1.0.2_CD:1.0.0 >",
            "appm start advertising",
            # Manufacturing-test SSID baked into the firmware (not a user network).
            "current product ssid name:tuya_mdev_test1",
            "scan_start_req_handler",
        ],
        # The control half of the pair below: with no network on air at all,
        # this firmware never enters production test. Only the run where the
        # virtual AP answers its factory probe does.
        "unexpected_strings": ["PROD FACTORY START"],
    },
    {
        # The same Woox dump, with its own factory test network put on air.
        # Stock Tuya firmware scans for "tuya_mdev_test1" at boot; the peer
        # refuses it by default (answering it changes what the firmware does,
        # which should be the test's choice, not a surprise). Offered with
        # --wifi-ssid, the device finds it in the scan and switches into
        # manufacturing test - light_prod.c "PROD FACTORY START!".
        # It never actually joins: 26 probe requests, 26 responses, and no
        # authentication. Seeing the network advertised is the whole trigger,
        # which is why the pcap assertions below are probe traffic only.
        # Paired with the case above, which is the same firmware for the same
        # time with nothing on air and must NOT reach production test, this
        # shows the firmware changing course because of what crossed the air.
        "name": "Woox Tuya stock firmware: factory test network on air switches it to production test",
        "binary": os.path.join(ROOT_DIR, "firmwares", "BK7231T_QIO_Woox_R5111_2023-14-10-23-46-06.bin"),
        "args": ["--only-uart", "-key", "TUYA", "--wifi-peer", "auto",
                 "--wifi-ssid", "tuya_mdev_test1", "--pcap", "{pcap}"],
        "timeout": 900,
        "expected_strings": [
            "current product ssid name:tuya_mdev_test1",
            "[WIFI] device acts as a station (an SSID was configured): peer takes the access point role",
            "light_prod.c:1068] PROD FACTORY START!",
            "First prod test!",
        ],
        "pcap_expect": ["beacon", "probe request", "probe response", "tuya_mdev_test1"],
    },
    {
        # AP-mode pairing, the other way a Tuya device is set up: instead of
        # joining a network it raises its own, which the phone app connects to
        # in order to hand over the home Wi-Fi credentials. This dump does that
        # unprompted, so the peer takes the station role, joins "SmartLife-AA4C"
        # and is served DHCP BY THE DEVICE - the device is the router here
        # (192.168.176.1), which is why the lease and the ping go the other way
        # round compared with every other Wi-Fi case.
        # It then announces itself in Tuya's LAN protocol. This firmware speaks
        # the 3.4/3.5 framing, which is sealed with a key only that device has,
        # so the announcement is identified but not read; the paired dump in the
        # next case uses 3.3 and its contents ARE readable.
        "name": "BK7238 Tuya RGB controller: raises its SmartLife pairing AP, serves DHCP, answers ping",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7238_Tuya_RGBController_TY-02-3CH_PWM_T1-3S_1.0.14.bin"),
        "args": ["--only-uart", "-chip", "BK7238", "--ble-core", "--wifi-peer", "auto",
                 "--pcap", "{pcap}"],
        "timeout": 1200,
        "expected_strings": [
            "[WIFI] device acts as an access point (it sends beacons): peer takes the station role",
            "[WIFI] found BSS 'SmartLife-AA4C' 00:33:7a:9d:aa:4d ch 6 (open)",
            "[WIFI] associated with 'SmartLife-AA4C' (AID 1)",
            "[NET] DHCP: we got 192.168.176.100/255.255.255.0 from the device (router 192.168.176.1)",
            "[NET] ARP: device 192.168.176.1 is at 00:33:7a:9d:aa:4d",
            "[NET] ping reply from 192.168.176.1 (seq 1)",
            "Tuya LAN protocol 3.4/3.5 announcement, command 0x11",
        ],
        "pcap_expect": ["beacon", "authentication", "association request", "association response",
                        "data", "SmartLife-AA4C"],
    },
    {
        # A PAIRED stock Tuya dump: this one was captured after the owner set it
        # up, so its flash still holds their network name. It probes for that
        # name, the peer adopts it (that is what --wifi-peer auto does with the
        # first SSID a device asks for), and the device joins and takes a lease.
        # Then it does what it was built to do: announces itself on the LAN in
        # Tuya protocol 3.3 - readable, because 3.3 uses one key shared by every
        # Tuya device - and reaches for its cloud, first at a hardcoded address
        # and then by name at m2.tuyaeu.com:8886, opening TLS to both.
        # Nothing leaves this machine: offline is the default, so each
        # connection is accepted locally and decoded. That is the whole point -
        # a stock device's cloud traffic captured without letting it out, and
        # without knowing anything about this firmware's internals.
        # The announcement names the same product key that is passed on the
        # command line here, which is independent confirmation that the
        # decryption is right rather than plausible-looking rubbish.
        "name": "BK7231N Tuya 3-phase meter (paired): joins its stored network and reaches for the Tuya cloud",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7231N_Tuya_PC321_3PhaseMeter_TuyaMCU_2.0.2.bin"),
        "args": ["--only-uart", "--xvr-selfclear", "--tuyamcu", "--tuyamcu-pid",
                 "gqmmtjclqb7reg5p", "--tuyamcu-raw", "-key", "TUYA",
                 "--wifi-peer", "auto", "--pcap", "{pcap}"],
        "timeout": 1200,
        "expected_strings": [
            "[WIFI] device probes for 'HojnikB WiFi': adopting it as our SSID",
            "[WIFI] station 4c:a9:19:14:71:50 associated to 'HojnikB WiFi' (AID 1)",
            "[NET] DHCP: device 4c:a9:19:14:71:50 got 192.168.1.100",
            # Its Tuya LAN announcement, decrypted: the device's cloud identity.
            '[NET] Tuya LAN broadcast from 192.168.1.100',
            '"gwId":"bf0e0f689d000d56656gmp"',
            '"productKey":"gqmmtjclqb7reg5p"',
            # The cloud attempts: one straight to an address, one resolved by name.
            "-> 18.185.31.196:8886 (accepted for capture)",
            "[NET] DNS m2.tuyaeu.com -> 10.200.",
            "(m2.tuyaeu.com):8886 (accepted for capture)",
            ("sends: TLS ClientHello", 2),
            # What the cloud connection says about itself before any key is
            # exchanged. Tuya authenticates its devices with a shared secret
            # rather than a certificate, so the hello offers only PSK suites
            # and carries no server name.
            "client hello: version 1.2, 4 cipher suites (TLS_PSK_WITH_AES_256_CBC_SHA",
            "no server name",
            "answered with TLS_PSK_WITH_AES_256_CBC_SHA, waiting for the key identity",
            # The device accepts that answer and moves on to naming its key -
            # and its own TLS library then stops, because this dump holds no
            # pre-shared key to name. mbedTLS -0x7600 is PRIVATE_KEY_REQUIRED,
            # "the own private key or pre-shared key is not set, but needed"
            # (the vendor's own ssl.h). Before the stack answered at all, the
            # same point reported -0x7280, CONN_EOF, which said nothing about
            # the device. If a dump ever does carry a usable key, this line
            # changes and the identity is logged instead - so assert it, and
            # look again when it fails.
            "mbedtls_ssl_handshake returned 7600 -30208",
        ],
        "pcap_expect": ["beacon", "probe request", "authentication", "association request",
                        "association response", "data", "HojnikB WiFi"],
    },
    {
        # The bulb above, on air. It was written off as crashing in early
        # init; past the RF-calibration stall it turns out to be a PAIRED dump
        # that joins the network name still in its flash and reaches for the
        # Tuya cloud - the second device in the suite to do so, after the
        # 3-phase meter.
        # That matters beyond one more device: it is independent confirmation
        # of how Tuya secures the cloud link. A different device, a different
        # SDK and a longer cipher list, and still every real suite it offers
        # is a pre-shared-key one with no server name - no certificate
        # anywhere. Its TLS library then stops at exactly the same place for
        # exactly the same reason (mbedTLS -0x7600, "the own private key or
        # pre-shared key is not set, but needed"), because an un-activated
        # dump has no key to present.
        "name": "BK7231N Tuya bulb (paired): joins its stored network and reaches the Tuya cloud",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7231N_Tuya_Feconn_MR16_RGBCT_5ch_PWM_1000Hz_1.3.21.bin"),
        "args": ["--only-uart", "-key", "TUYA", "--xvr-selfclear", "--wifi-peer", "auto"],
        "timeout": 1200,
        "expected_strings": [
            "[WIFI] device probes for 'randomssid': adopting it as our SSID",
            "[WIFI] station cc:8c:bf:4d:51:56 associated to 'randomssid' (AID 1)",
            "[NET] DHCP: device cc:8c:bf:4d:51:56 got 192.168.1.100",
            # Its cloud attempts, and what the handshake gives up in clear.
            ":443 (accepted for capture)",
            "client hello: version 1.2, 8 cipher suites",
            "TLS_PSK_WITH_AES_128_CCM",
            "no server name",
            "answered with TLS_PSK_WITH_AES_256_CBC_SHA, waiting for the key identity",
            # Same verdict as the meter: nothing to present.
            "mbedtls_ssl_handshake returned 7600 -30208",
            "resolve mqtt host Fail:m2.tuyaeu.com",
        ],
    },
    {
        # EZ mode (SmartConfig), the third way a Tuya device is paired, and the
        # one that needs hardware support rather than ordinary frames: the
        # device puts the receiver into monitor mode and reads the lengths of
        # frames the phone sprays at a broadcast address, which carry the
        # credentials. The firmware calls hal_machw_enter_monitor_mode and
        # writes MM_RX_FILTER_MONITOR (mm.h) to the MAC's RX filter -
        # 0x7fffffde, every accept bit but three - and the emulator reports
        # that, so a dump sniffing the air can be told apart from one scanning.
        # This dump then gives up on EZ mode, returns the filter to
        # MM_RX_FILTER_ACTIVE (0x3503848c) and falls back to AP-mode pairing,
        # so one run shows both pairing modes and the switch between them.
        "name": "BK7231N Tuya thermostat: EZ-mode sniffing reported, then falls back to AP pairing",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7231N_Tuya_BEOK_TOL47WIFI_Thermostat_TuyaMCU_TuyaOS_3.11.12.bin"),
        "args": ["--only-uart", "--tuyamcu", "--tuyamcu-pid", "9cqoypoauuvhdjq4",
                 "--tuyamcu-raw", "-key", "TUYA", "--wifi-peer", "auto"],
        "timeout": 1800,
        "expected_strings": [
            "hal_machw_enter_monitor_mode",
            "[WIFI] device RX filter 0x7fffffde: monitor mode, every frame on air accepted",
            "[WIFI] device RX filter 0x3503848c: back to normal reception",
            "ssid:SmartLife-FD43, key:, channel: 6",
            "[WIFI] device changed role (it sends beacons)",
            "[WIFI] found BSS 'SmartLife-FD43' 80:64:7c:ed:fd:42 ch 6 (open)",
        ],
    },
    {
        # A second paired dump, for one reason: it proves the LAN announcement
        # is readable because the key is shared by every Tuya device, not
        # because of anything particular to the meter above. Different device,
        # different stored network, different gateway id - same key. Its
        # announcement also carries the 3.4 fields, still in the 3.3 framing.
        # The product key it announces is the one passed on the command line
        # here, which is what rules out the decryption producing plausible
        # rubbish that happens to parse.
        "name": "BK7231N Tuya alarm (paired): a second device's cloud identity decodes with the same shared key",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7231N_Tuya_A03CB3S_Alarm_TuyaMCU_2.1.17.bin"),
        "args": ["--only-uart", "--xvr-selfclear", "--tuyamcu", "--tuyamcu-pid",
                 "ztoh9ka787lzjkpy", "--tuyamcu-raw", "-key", "TUYA", "--wifi-peer", "auto"],
        "timeout": 1200,
        "expected_strings": [
            "[WIFI] device probes for 'KN': adopting it as our SSID",
            "[WIFI] station fc:3c:d7:3b:1a:d6 associated to 'KN' (AID 1)",
            "[NET] DHCP: device fc:3c:d7:3b:1a:d6 got 192.168.1.100",
            "[NET] Tuya LAN broadcast from 192.168.1.100",
            '"gwId":"bf9935888c84222522cnd4"',
            '"productKey":"ztoh9ka787lzjkpy"',
            '"version":"3.4"',
        ],
    },
    {
        # The BK7231N temp/humidity sensor exercises the same protected-key path
        # and was the case that exposed it: with 0xFF served it failed the magic
        # check ("flash is encrypted or empty" - the magic and crc32 it reported
        # are literally AES-128-ECB-decrypt(0xFF x16) under the firmware's
        # hardcoded key "qwertyuiopasdfgh"), then dropped to mf_test with
        # "db init fails". Serving the dump's real bytes lets it decrypt its key.
        # It then used to stop dead a few lines later, which was put down to
        # its being an unpaired dump parked in manufacturing test. That was
        # wrong: it was spinning on the RF-calibration register at 0x9000f8,
        # whose bit 31 the firmware sets and waits for the hardware to clear.
        # With --xvr-selfclear it reaches its TuyaMCU link.  It used to stop
        # there repeatedly asking an absent MCU for product information.  The
        # simulated MCU now answers with the exact 16-byte product id stored by
        # this dump (the tempting 17-byte spelling printed by an older probe is
        # rejected).  --ble-core then carries the accepted product through BLE
        # service/advertisement setup, while the Wi-Fi peer supplies the MAC
        # activity that advances the module to its working-mode query.
        "name": "BK7231N Tuya TempHum Sensor accepts its MCU identity and starts BLE/Wi-Fi",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7231N_Tuya_TempHum_LCD_Sensor_TuyaMCU_2025-26-9.bin"),
        "args": ["--only-uart", "--uart1-hex", "-key", "TUYA", "--xvr-selfclear",
                 "--ble-core", "--tuyamcu", "--tuyamcu-pid", "hwebffr1rcfvue3p",
                 "--wifi-peer", "auto"],
        "timeout": 300,
        "tags": ["BLE", "BLE core", "FIQ"],
        "expected_strings": [
            "key_addr: 0x1ee000",
            "get key:",
            "0x6b 0x24 0x0 0x73 0xf4 0x11 0x45 0xed 0x40 0xf1 0x9 0x9f 0x16 0xd3 0xb5 0xc0",
            "STACK INIT OK",
            "[BLE_ADV]   Service Data",
            "mf_init succ",
            # The module asks, the peer answers, and the firmware validates the
            # exact product id against protected storage.
            "[UART1/MCU] 55 aa 00 01 00 00 00",
            "wifi mcu init. pid:hwebffr1rcfvue3p",
            "gw_cntl.gw_if.product_key:hwebffr1rcfvue3p, input:hwebffr1rcfvue3p",
            "device_init succ",
            "hwebffr1rcfvue3p  uuid=0xA201",
            # Real forward progress beyond the old product-query retry loop.
            "[UART1/MCU] 55 aa 00 02 00 01 00 02",
        ],
        "unexpected_strings": ["abi/pid/tp invalid", "len:33 err"],
    },
    {
        # Stock BK7231N Tuya smart plug (SDK 2.3.3).  This used to run under the
        # ambiguous BK7231T/U identity and hide a BLE lld.c:404 assertion behind
        # shallow key/scan assertions. Selecting the real BK7231N silicon now
        # instantiates its BLE 5.1 hardware automatically: no --ble-core or
        # --xvr-selfclear escape hatch. The factory network is put on air so the
        # real application proceeds through all five relay channels, metering,
        # its scan result and production-mode entry.
        "name": "BK7231N Tuya plug auto-models BLE and enters factory mode",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7231N_Tuya_Plug_TSL-PS-F4U-01-W_1.1.17.bin"),
        "args": ["--only-uart", "--uart1-hex", "-key", "TUYA",
                 "-chip", "BK7231N", "--wifi-peer", "auto",
                 "--wifi-ssid", "tuya_mdev_test1", "--pcap", "{pcap}"],
        "timeout": 120,
        "tags": ["BLE", "BLE core", "FIQ"],
        "expected_strings": [
            "< TUYA IOT SDK V:2.3.3 BS:40.00_PT:2.2_LAN:3.4_CAD:1.0.5_CD:1.0.0 >",
            "IOT DEFS < WIFI_GW:1 DEBUG:1 KV_FILE:0 SHUTDOWN_MODE:0 LITTLE_END:1 TLS_MODE:2",
            "ENABLE_LAN_ENCRYPTION:1",
            "oem_bk7231n_plug:1.1.17",
            "firmware compiled at Jun 13 2023 20:36:20",
            # Protected key store read correctly (would be a blank-flash failure
            # chain ending in mf_test if the raw bytes above the logical end
            # were not served).
            "key_addr: 0x1ee000",
            "get key:",
            "0xe7 0x10 0xac 0x15 0x88 0x2b 0x38 0x50 0xb2 0x9a 0xd 0xfa 0x35 0xbe 0x99 0x37",
            # Automatic hardware, not an opt-in workaround: HCI completes,
            # the host creates its database and advertising reaches state 7.
            "ble sdk re_inited",
            "ty bt sdk init success finish",
            "gapm_cmp_evt:BLE_STACK_OK",
            "CREATE DB OK",
            "adv_state:7",
            # The actual plug application is alive beyond generic SDK setup.
            "mf_init succ",
            "channel 4 Init ok",
            "tuya_ele_hw_init success",
            "app_init finished",
            # Seeing the factory SSID is the production-test trigger; this
            # firmware intentionally does not associate with it.
            "current product ssid name:tuya_mdev_test1",
            "scan_start_req_handler",
            "Scan completed in",
            "enter prod mode",
        ],
        "unexpected_strings": ["lld.c:404", "ASSERTION FAILED"],
        "pcap_expect": ["beacon", "probe request", "probe response", "tuya_mdev_test1"],
    },
    {
        # BK7231N "zmai90" energy meter built around an RN8209C metering chip on
        # UART. Stock Tuya firmware (SDK 2.3.1). Reads its protected key fine
        # (0x1ee000, exercising the same above-logical-end fallback), then drops
        # into the mf_test manufacturing-test thread and does not reach normal
        # operation - so it never polls the RN8209C and emits nothing on UART1.
        # This is a *pre-pair* (unactivated) dump: with no activation record the
        # SDK treats the device as unprovisioned and parks in mf_test. Only a
        # post-pair dump would boot on and drive the RN8209C over UART1. The
        # check is that it boots this far and reads the key.
        # --xvr-selfclear for the same reason as the temp/humidity sensor
        # above: without it this dump stopped at the RF-calibration spin a few
        # lines past the key read (45 lines of log against 614 with it). The
        # markers below are from the far side of that stall - the device
        # identity it reports to its cloud stack, and the Wi-Fi status
        # callback that only the running app reaches.
        "name": "BK7231N Tuya zmai90 RN8209C Energy Meter Boots into its app",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7231N_Tuya_zmai90_RN8209C_EnergyMeter.bin"),
        "args": ["--only-uart", "--uart1-hex", "-key", "TUYA", "--xvr-selfclear"],
        "timeout": 300,
        "expected_strings": [
            "< TUYA IOT SDK V:2.3.1 BS:40.00_PT:2.2_LAN:3.3_CAD:1.0.3_CD:1.0.0 >",
            "firmware compiled at Apr 26 2022 15:12:39",
            "init protected data length 460",
            "key_addr: 0x1ee000",
            "get key:",
            "0x80 0xd9 0xca 0xc2 0x32 0xb4 0x9c 0x30 0x6e 0x8b 0xc2 0x3d 0xf4 0x4c 0x39 0x7c",
            "upd product_id type:0 nqbs1onwskmmmaac",
            "wf_nw_status_cb,wifi_status:0",
        ]
    },
    {
        # The first STOCK-firmware dump on BK7238. Every other image on this
        # silicon here is OpenBeken, so until now the BK7238 chip identity was
        # only ever exercised by firmware we build ourselves.
        #
        # Two things make it worth its own case:
        #
        # 1. The firmware reads the SCTRL device-id register back and PRINTS it
        #    ("device_id=0x21128000"), which is exactly the value src/chips/bk7238.py
        #    serves for -chip BK7238. No other case asserts a chip-id round
        #    trip: here the firmware itself confirms the identity we hand it,
        #    so a wrong -chip would show up in the device's own log.
        #
        # 2. It is UNENCRYPTED. Every other stock Tuya dump here needs
        #    -key TUYA; BK7238 images are plaintext, so this is the only stock
        #    case covering crypto.py's plaintext path on real vendor firmware
        #    rather than on an OpenBeken build.
        #
        # It boots a long way: RF calibration, the OTA check, the Tuya light
        # stack (colour gamma tables, dimmer curve, startup state), its own
        # product id, and into BLE init.
        #
        # Where it stops, and why the markers end here: at ~19M instructions,
        # just past BLE init, it wedges in an XVR spin - it sets bit 31 of
        # 0x00900000 and polls it until hardware clears it, with interrupts
        # masked (PC pins to 0x000E8C40, output freezes, rate drops ~18x and
        # stays there for at least 10 minutes). That is precisely the busy bit
        # --xvr-selfclear models, and passing it does push further (device
        # clock 01:00:21 vs 01:00:03, 224 vs 167 log lines) - but only into
        # the BLE HOST stack, which then fails waiting for an HCI ack it can
        # never get (ble_hs_hci_wait_for_ack rc = 19) and loops on assert +
        # tkl_hci_reset. So the flag is deliberately NOT used here: this dump
        # does real BLE init, exactly the case its help text warns about. The
        # assertions stop at the last state the dump reaches reliably.
        #
        # It never hands a pad to the PWM peripheral before that point, so
        # there are no PWM channels to assert on despite this being an RGB
        # controller. The case right after this one runs the same dump with
        # --ble-core, which is what gets past the spin.
        "name": "BK7238 Tuya RGB Controller TY-02-3CH (stock, unencrypted) boots its light stack",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7238_Tuya_RGBController_TY-02-3CH_PWM_T1-3S_1.0.14.bin"),
        # No -key: this image is plaintext, unlike every other stock Tuya dump.
        "args": ["--only-uart", "-chip", "BK7238"],
        "timeout": 420,
        "tags": ["BLE"],
        "expected_strings": [
            # The chip identity we serve, echoed back by the firmware itself.
            "device_id=0x21128000",
            # Vendor SDK bring-up: Wi-Fi MAC init and RF calibration.
            "[FUNC]rwnxl_init",
            "[FUNC]calibration_main",
            "tuya_upgrade_main need upgrade?",
            # Tuya light application: per-channel gamma tables and the dimmer
            # curve - an RGB controller doing RGB-controller things, not just
            # a generic Tuya boot.
            "sa_crc_r[18] sa_crc_g[18] sa_crc_b[18]",
            "val_cur:2835, val_max:7000, val_min:1050",
            "startup default",
            "light basic service init OK",
            # Reached the Tuya device layer and knows its own identity - the
            # firmware key this dump was licensed with.
            "upd product_id type:1 keyw97qjscyfmsyn",
            # ...and gets its BLE stack going unaided.
            "initial BLE...",
        ],
        # Only pins whose value never changes during the run are asserted: 1, 9
        # and 11 are each written twice, so pinning them would test timing
        # rather than decoding. 0x78 = second function + pull-up, the UART
        # pads. Pin 7 is the negative half - never written, which proves the
        # capture follows this firmware rather than emitting a fixed set.
        "expected_pins": {0: 0x78, 10: 0x78, 7: None},
    },
    {
        # Same BK7238 dump, with the BLE core modelled (--ble-core, src/blecore.py).
        #
        # Why the plain run wedges: the NimBLE HOST hands HCI commands to the
        # RivieraWaves CONTROLLER on the same CPU, and the controller only ever
        # runs (rwip_schedule) after one of its two FIQs - which come from the
        # core register block at 0x900000. Served as plain memory that block
        # never interrupts, so the controller arms deep sleep once and never
        # wakes, and the host times out (ble_hs_hci_wait_for_ack rc = 19) and
        # loops on assert. This emulator had also never delivered an FIQ.
        #
        # With the model the controller's real steady state runs: deep sleep
        # (capped at 10 ms of device time, as an external event would cut it
        # short on silicon) -> SLP interrupt -> clock correction -> slot-clock
        # interrupt -> wake-up complete -> schedule -> sleep. Every FIQ is
        # acknowledged, HCI commands get their completion events, and the
        # Tuya BLE service reports the advertisement applied - a line the
        # plain run never reaches. No radio is modelled, so nothing goes on
        # air; the BLE-side event interrupts are never raised.
        #
        # Decoded from register traces of this very image; the two interrupt
        # register sets are the reverse of the older SDK headers (set 1 is
        # the core side), which cost a storm of 2000 unacknowledged FIQs to
        # find out. See src/blecore.py for the full account.
        "name": "BK7238 Tuya RGB Controller with --ble-core: BLE host completes HCI, adv applied",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7238_Tuya_RGBController_TY-02-3CH_PWM_T1-3S_1.0.14.bin"),
        "args": ["--only-uart", "-chip", "BK7238", "--ble-core"],
        "timeout": 420,
        "tags": ["BLE", "BLE core", "FIQ"],
        "expected_strings": [
            "device_id=0x21128000",
            "initial BLE...",
            # The host asked for advertising...
            "ble_gap.c:2505] Start Adv",
            # ...and the controller answered: Tuya's BLE service only logs this
            # once the HCI advertising commands have completed. The plain run
            # gets "ble_hs_hci_wait_for_ack, rc = 19" here instead.
            "ble adv updated",
            # Device services keep initialising past the point the plain run
            # spends looping on the BLE host assert.
            "__devos_init_evt success",
        ],
    },
    {
        # The first non-Tuya vendor stack in the suite, and the first stock
        # BK7231M dump: a VeSync (Etekcity) EU plug running VeSync SDK v1.2.1
        # on top of the Beken SDK. Plaintext, no -key; M is N silicon, so the
        # default BK7231 identity - which --ble-core maps to the BLE 5.1 core.
        #
        # This dump is what brought the N-family 5.1 core into the model. Each
        # of these was a wall, in boot order (without --ble-core it still
        # stops at the first one):
        #   1. SLOTCLK sits at +0xF8 on 5.1 (+0x100 on 5.2). As plain memory its
        #      SAMP bit never clears: a spin right after "rwble_hl_init ok".
        #   2. lld_init asserts the BLE block's own VERSION at 0x900804
        #      (lld.c:404, param1 = 167776000 = 0x0A000F00). Earlier notes put
        #      that assert down to the prebuilt link layer; it is one register.
        #   3. The hardware AES engine. The host's GAPM_GEN_RAND_NB is computed
        #      with it; without it BLE init fails with "create cmd db fail".
        #   4. pwm_new, used here to capture the metering chip's pulses:
        #      REG_GROUP_PWM_CPU's read request never self-cleared, and
        #      GROUP_CTRL's write-1-to-clear interrupt status stuck at 1, so the
        #      PWM ISR acked a phantom capture forever.
        # With all four modelled BLE comes up, the advertisement is staged in
        # exchange memory, and network config starts a Wi-Fi scan - the edge
        # of what the emulator models.
        "name": "BK7231M VeSync WHOGPLUG Plug (VeSync SDK, BLE 5.1 core) advertises and scans",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7231M_VeSync_GreenSun_WHOGPLUG_Plug_1.0.01.bin"),
        "args": ["--only-uart", "--ble-core"],
        "timeout": 420,
        "tags": ["BLE", "BLE core", "FIQ"],
        "expected_strings": [
            "Vesync SDK version: v1.2.1",
            "ble mac:fc-58-4a-a1-d3-c3",
            # (1) past the slot-clock spin, (2) with the link layer initialised
            "rwble_hl_init ok",
            # (3) the AES engine answered, twice, and the stack declared itself up
            ("gapm_cmp_evt:GAPM_GEN_RAND_NB", 2),
            "gapm_cmp_evt:BLE_STACK_OK",
            "[THD]vesync_ble_task",
            # The advertisement, read out of exchange memory: Etekcity's
            # company id, then the BLE MAC above in reverse byte order.
            "company=0x06D0",
            "ff d0 06 01 c3 d3 a1 4a 58 fc",
            # (4) the firmware reads GROUP_CTRL back with no phantom status
            # bits - it printed 0xc0001c14 before the model.
            "REG_PWM_GROUP_CTRL= 0x1c14",
            "[THD]netcfg_task",
            "wpa rx E SCAN_STARTED",
        ],
        "unexpected_strings": [
            "lld.c",
            "create cmd db fail",
            "ble is not ready",
        ],
    },
    {
        # A "BLE-gated" stock Tuya dump - the class that used to be written
        # off. Its firmware brings the RivieraWaves GAP layer up during
        # start-up (ble_appm_send_gapm_reset_cmd) and, with the core served
        # as plain memory, spins on the slot clock or dies on the lld.c:404
        # version assert. 153 of the 218 full BK7231N/BL2028N dumps in
        # FlashDumps are built this way. --ble-core (5.1 layout) takes them
        # through REAL BLE init instead of the --xvr-selfclear bit trick the
        # ATORCH/PC321/A03CB3S cases use, and further: this paired Globe
        # Nashville ceiling fan (SDK 2.1.17, TuyaMCU) runs the whole
        # advertising state machine, stages Tuya's pairing beacon - service
        # UUID 0xA201 with its own product key in the service data - and,
        # with the MCU peer answering (licensed id qasrgndvbbtrula9), goes
        # past working-mode to report its Wi-Fi state (0x03).
        "name": "BK7231N Tuya Globe Ceiling Fan (BLE-gated, SDK 2.1.17) --ble-core: Tuya beacon, TuyaMCU to Wi-Fi state",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7231N_Tuya_GlobeNashville_CeilingFan_TuyaMCU_2.1.17.bin"),
        "args": ["--only-uart", "--uart1-hex", "--ble-core", "--tuyamcu",
                 "--tuyamcu-pid", "qasrgndvbbtrula9", "--tuyamcu-raw", "-key", "TUYA"],
        "timeout": 420,
        "tags": ["BLE", "BLE core", "FIQ"],
        "expected_strings": [
            "bk7231n_common_user_config_ty:2.1.17",
            "[ble_appm_send_gapm_reset_cmd]",
            # Real BLE init: the AES engine answered, the stack came up and
            # the advertising state machine ran to the start command.
            ("gapm_cmp_evt:GAPM_GEN_RAND_NB", 2),
            "gapm_cmp_evt:BLE_STACK_OK",
            "[ble_appm_start_advertising]",
            "mf_init succ",
            # Tuya's pairing beacon, read out of exchange memory.
            "uuid=0xA201",
            "qasrgndvbbtrula9  uuid=0xA201",
            # The MCU link: heartbeat, product query, working-mode...
            "[UART1/MCU] 55 aa 00 00 00 00 ff",
            "55 aa 00 01 00 00 00",
            "55 aa 00 02 00 00 01",
            "product_key:qasrgndvbbtrula9",
            # ...and one step further: the Wi-Fi state report.
            "55 aa 00 03 00 01 01 04",
        ],
        "unexpected_strings": [
            "lld.c",
            "prod len",
        ],
    },
    {
        # A stock Tuya breaker / leakage switch on the BK7231T, SDK 1.1.80.
        # Two things make it worth its own case rather than being one more
        # TuyaMCU dump:
        #
        # 1. It pins down where the product-info wire form actually changes.
        #    The only other 1.1.x image here (TMWF02 fan switch, 1.1.71)
        #    ACCEPTS the JSON record {"p":..,"v":..}; every 2.x/3.x image
        #    rejects it on length and needs the raw 16-byte id. 1.1.80 turns
        #    out to be on the RAW side of that line, so the switch happened
        #    within the 1.1.x series, not at the 2.x boundary as the other
        #    dumps here would suggest. Verified by running it both ways: with
        #    JSON it logs "prod len = 36" and stores the mangled key
        #    'product_key:{"p":"wcihaluccf' (the first 16 bytes of our reply,
        #    copied verbatim); with --tuyamcu-raw it stores the real id. The
        #    clean product_key assertion below is what separates the two.
        #
        # 2. It is the first stock dump here that brings its whole BLE stack up
        #    unaided - "STACK INIT OK", a GATT database, and advertising - with
        #    no --xvr-selfclear. The 2.x images (ATORCH, PC321, A03CB3S) all
        #    need that opt-in to get past their RF/BLE busy spins, so this one
        #    shows the emulator handling a wifi+BLE SDK on the real path.
        #
        # Paired dump: it reads a real protected key at 0x1ee000 (not the blank
        # magic an unprovisioned part returns) and reaches mf_init, so it gets
        # to normal operation and drives the MCU link instead of parking in the
        # manufacturing-test thread the way the pre-pair zmai90 dump does.
        "name": "BK7231T Tuya Breaker/Leakage Switch (TuyaMCU 1.1.80) accepts raw product",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7231T_Tuya_Breaker_LeakageSwitch_TuyaMCU_1.1.80.bin"),
        "args": ["--only-uart", "--uart1-hex", "--tuyamcu",
                 "--tuyamcu-pid", "wcihaluccfsoayqa", "--tuyamcu-raw", "-key", "TUYA"],
        "timeout": 420,
        "tags": ["BLE", "TuyaMCU"],
        "expected_strings": [
            "bk7231t_common_user_config_ty:1.1.80",
            # Provisioned: a real key comes back from the protected block, and
            # the SDK gets through manufacturing init to normal operation.
            "key_addr: 0x1ee000",
            "mf_init succ",
            # Whole BLE stack, with no --xvr-selfclear to help it along.
            "STACK INIT OK",
            "CREATE DB SUCCESS",
            "appm start advertising",
            # The MCU link is up and the module talks first (heartbeat).
            "[UART1/MCU] 55 aa 00 00 00 00 ff",
            # Peer-unblocked: the product query, never sent without a peer.
            "55 aa 00 01 00 00 00",
            # Our RAW product record was accepted and stored intact. With the
            # JSON form this same line reads product_key:{"p":"wcihaluccf, so
            # the clean id here is the proof the raw form is the one 1.1.80
            # wants - not merely that the firmware kept running.
            "product_key:wcihaluccfsoayqa",
            "wifi mcu init. pid:wcihaluccfsoayqa firmwarekey:key34ak4q5rmrkef v1:1.1.80",
            # ...and the stored key matched what we sent, so it goes on to the
            # working-mode query (0x02) and into Wi-Fi link setup.
            "gw_cntl.gw_if.product_key:wcihaluccfsoayqa, input:wcihaluccfsoayqa",
            "55 aa 00 02 00 00 01",
        ]
    },
    {
        # The first *stock Tuya* dump found that actually drives its MCU. An
        # ETWF4301 thermostat (AXZN / TM1640 panel): the MCU owns the display and
        # sensors, the Beken is only the radio, so the two talk over UART1.
        #
        # It is a PAIRED dump - it logs "have actived over 15 min, not enter
        # mf_init", so unlike the pre-pair dumps (TempHum sensor, zmai90) it does
        # not park in the manufacturing-test thread. It reaches normal operation
        # and then sends TuyaMCU heartbeats unprompted, with no MCU attached -
        # byte-identical to the frame OpenBeken's own driver emits.
        #
        # This case guards three things at once:
        #  - the PHYSICAL flash-addressing model: both KV copies must read valid
        #    with matching counts (the mirror at 0x1cf000 read as blank flash
        #    under the old stripped/logical model, which is what exposed the bug);
        #  - that a paired stock-Tuya image boots through to normal operation;
        #  - the UART1 capture path on stock vendor firmware, not just OpenBeken.
        #
        # A simulated MCU is attached to UART1 (--tuyamcu, src/tuyamcu.py) and,
        # like the real MCU wired to this thermostat, answers in the form
        # TuyaOS 3.x expects: --tuyamcu-raw replies to the product query with
        # the raw 16-byte product id + short version (not JSON), and
        # --tuyamcu-pid gives the device's OWN licensed id, recovered from its
        # gw_bi KV ('pk':'i3k1tsas1esbewba'). With the right form and id the
        # device accepts the record (its stored product_key matches our input)
        # and advances into the working-mode query (0x02) and Wi-Fi link setup
        # - far past the heartbeat loop it idles in with nothing attached. The
        # heartbeat is asserted once, not repeated: once the peer answers the
        # device advances instead of idling, so handshake progress is the
        # liveness signal. (A/B over a shared instruction budget: with peer,
        # heartbeat @19.2M / QUERY_PRODUCT @20.2M; without, no 0x01 by 43.2M.)
        "name": "BK7231N Tuya Ettroit ETWF4301 (paired) sends TuyaMCU heartbeats",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7231N_Tuya_Ettroit_ETWF4301_Thermostat_TuyaMCU_3.1.28.bin"),
        "args": ["--only-uart", "--uart1-hex", "--tuyamcu",
                 "--tuyamcu-pid", "i3k1tsas1esbewba", "--tuyamcu-raw", "-key", "TUYA"],
        # The first frame lands only after the SDK reaches normal operation
        # (~3-4 min here); streaming stops as soon as the required count is seen.
        "timeout": 420,
        "expected_strings": [
            "bk7231n_common_user_config_ty:3.1.28",
            # Paired: skips manufacturing test.
            "have actived over 15 min, not enter mf_init",
            "mf_init succ",
            # Physical flash addressing: BOTH kv copies valid, counts matching.
            "current kv info, addr: 1ed000, cnt: 97, is valid: 0",
            "mirror kv info, addr: 1cf000, cnt: 97, is valid: 0",
            # The MCU link is up and the module talks first (heartbeat).
            "[UART1/MCU] 55 aa 00 00 00 00 ff",
            # Peer-unblocked: the product query, never sent without a peer.
            "55 aa 00 01 00 00 00",
            # The device ACCEPTED our raw product record - its stored key
            # matched the id we sent (JSON is rejected on length; raw is not).
            "gw_cntl->gw_if.product_key:i3k1tsas1esbewba, input:i3k1tsas1esbewba",
            # ...and advances to the working-mode query (0x02): real forward
            # progress past the product stage, into Wi-Fi link setup.
            "55 aa 00 02 00 00 01",
        ]
    },
    {
        # Second paired stock-Tuya device that drives its MCU, and deliberately a
        # DIFFERENT SDK generation from the Ettroit case: TuyaOS 3.11.12 (built
        # 2025) rather than the older "TUYA IOT SDK V:2.3.3" line. It also takes a
        # different path - it never prints "have actived", yet still skips
        # manufacturing test - so the two cases together cover both variants.
        #
        # Picked by a static signal that separates paired from factory dumps
        # perfectly across every device checked: a paired unit has written BOTH
        # KV copies (mirror 0x1cf000 as well as current 0x1ed000), because the
        # store only rotates after real service use. Factory dumps leave the
        # mirror erased. (The earlier "user data %" heuristic was noise - it
        # scored this device the same as unpaired ones.)
        #
        # A simulated MCU is attached to UART1 (--tuyamcu, src/tuyamcu.py) and,
        # like the real MCU wired to this thermostat, answers in the form
        # TuyaOS 3.x expects: --tuyamcu-raw replies to the product query with
        # the raw 16-byte product id + short version (not JSON), and
        # --tuyamcu-pid gives the device's OWN licensed id, recovered from its
        # gw_bi KV ('pk':'9cqoypoauuvhdjq4'). With the right form and id the
        # device accepts the record (its stored product_key matches our input)
        # and advances into the working-mode query (0x02) and Wi-Fi link setup
        # - far past the heartbeat loop it idles in with nothing attached. The
        # heartbeat is asserted once, not repeated: once the peer answers the
        # device advances instead of idling, so handshake progress is the
        # liveness signal. (A/B over a shared instruction budget: with peer,
        # heartbeat @21.8M / QUERY_PRODUCT @22.8M; without, no 0x01 by 33.8M.)
        "name": "BK7231N Tuya BEOK Thermostat (TuyaOS 3.11.12) sends TuyaMCU heartbeats",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7231N_Tuya_BEOK_TOL47WIFI_Thermostat_TuyaMCU_TuyaOS_3.11.12.bin"),
        "args": ["--only-uart", "--uart1-hex", "--tuyamcu",
                 "--tuyamcu-pid", "9cqoypoauuvhdjq4", "--tuyamcu-raw", "-key", "TUYA"],
        "timeout": 420,
        "expected_strings": [
            "< TuyaOS V:3.11.12 BS:40.00_PT:2.3_LAN:3.5_CAD:1.0.5_CD:1.0.0 >",
            "mf_init succ",
            # Physical flash addressing: BOTH kv copies valid with matching counts.
            "current kv info, addr: 1ed000, cnt: 8, is valid: 0",
            "mirror kv info, addr: 1cf000, cnt: 8, is valid: 0",
            # Protected key decrypted from 0x1ee000.
            "0x53 0xc1 0xb1 0x85 0xe3 0xbf 0xb6 0x3 0xe8 0x43 0xad 0xfb 0x83 0x82 0x69 0xdf",
            # The MCU link is opened at the TuyaMCU baud before any frame goes out.
            "read baud rate 9600",
            "[UART1/MCU] 55 aa 00 00 00 00 ff",
            # Peer-unblocked: the product query, never sent without a peer.
            "55 aa 00 01 00 00 00",
            # The device ACCEPTED our raw product record - its stored key
            # matched the id we sent (JSON is rejected on length; raw is not).
            "gw_cntl->gw_if.product_key:9cqoypoauuvhdjq4, input:9cqoypoauuvhdjq4",
            # ...and advances to the working-mode query (0x02): real forward
            # progress past the product stage, into Wi-Fi link setup.
            "55 aa 00 02 00 00 01",
        ]
    },
    {
        # Third paired stock-Tuya device driving its MCU, chosen for a different
        # device CLASS rather than a different SDK: a dual-clamp power meter,
        # where the MCU owns the current clamps and all metering and the Beken is
        # purely the radio. (It ships the same TuyaOS 3.11.12 build as the BEOK
        # case, so this adds hardware-class coverage, not SDK coverage.)
        #
        # Its KV pair carries cnt 16 - a different rotation count from the other
        # two (97 and 8) - which is useful: it shows the physical-addressing fix
        # is not tied to one particular store state.
        #
        # A simulated MCU is attached to UART1 (--tuyamcu, src/tuyamcu.py) and,
        # like the real metering MCU wired to this device, it answers in the
        # form TuyaOS 3.x expects: --tuyamcu-raw replies to the product query
        # with the raw 16-byte product id + short version (not JSON), and
        # --tuyamcu-pid gives the device's OWN licensed id, recovered from its
        # gw_bi KV ('pk':'wifech3utowiyknu'). Format and length both matter:
        # the 0x01 handler at app PC 0x750b4 rejects any payload whose length
        # is outside [16,24] ('prod len = N') and otherwise copies the first
        # 16 bytes verbatim as gw_if.product_key. With the right form AND id
        # the device accepts the record (product_key == input), updates its
        # product id, and advances into the working-mode query (0x02) and on
        # into Wi-Fi/BLE network config - far past the heartbeat loop it idles
        # in with nothing attached. The heartbeat is asserted once, not
        # repeated: once the peer answers, the device advances instead of
        # idling, so handshake progress is the liveness signal.
        "name": "BK7231N Tuya PJ1103C Dual-Clamp Power Meter sends TuyaMCU heartbeats",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7231N_Tuya_PJ1103C_DualClampPowerMeter_TuyaMCU_TuyaOS_3.11.12.bin"),
        "args": ["--only-uart", "--uart1-hex", "--tuyamcu",
                 "--tuyamcu-pid", "wifech3utowiyknu", "--tuyamcu-raw", "-key", "TUYA"],
        "timeout": 420,
        "expected_strings": [
            "< TuyaOS V:3.11.12 BS:40.00_PT:2.3_LAN:3.5_CAD:1.0.5_CD:1.0.0 >",
            "mf_init succ",
            # Physical flash addressing: BOTH kv copies valid, counts matching.
            "current kv info, addr: 1ed000, cnt: 16, is valid: 0",
            "mirror kv info, addr: 1cf000, cnt: 16, is valid: 0",
            # This device's own protected key, decrypted from 0x1ee000.
            "0xfe 0x2f 0xe2 0x48 0x9 0x3f 0x8d 0x4a 0xad 0x1a 0xc1 0xf3 0x79 0x2f 0x56 0xf6",
            "read baud rate 9600",
            "[UART1/MCU] 55 aa 00 00 00 00 ff",
            # Peer-unblocked: the product query, never sent without a peer.
            "55 aa 00 01 00 00 00",
            # The device parsed and ACCEPTED our raw product record: its
            # stored key matched the id we sent, so it updates its product id
            # (the JSON form is rejected on length - this raw form is not).
            "upd product_id type:0 wifech3utowiyknu",
            # ...and advances to the working-mode query (0x02) - real forward
            # progress past the product stage, not just receipt of our reply.
            "55 aa 00 02 00 00 01",
        ]
    },
    {
        # A SECOND UART protocol on the same path, so the UART1 capture is not
        # only ever proven with TuyaMCU framing. Startup command
        # "startDriver BL0942" runs OpenBeken's BL0942 energy-meter driver, which
        # opens UART1 at 4800 baud (not 9600) and speaks a completely different
        # wire format - per drv_bl0942.c, write is 0xA8|addr and read is
        # 0x58|addr, each frame checksum-terminated:
        #   a8 1d 55 00 00 e5   write reg 0x1D (mode), checksum e5
        #   a8 19 8f 00 00 af   write reg 0x19,        checksum af
        #   58 aa               read reg 0xAA = the full measurement packet,
        #                       then repeated as the driver polls every second.
        # Nothing here resembles TuyaMCU's 55 AA framing, which is the point.
        "name": "MathDemo Startup Command: startDriver BL0942 drives the meter UART",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "OpenBK7231T_QIO_1.18.300_mathDemo_obkStartupCommand_BL0942.bin"),
        "args": ["--only-uart", "--uart1-hex", "-key", "TUYA"],
        "timeout": 240,
        "expected_strings": [
            "CFG_InitAndLoad: Correct config has been loaded",
            "Started BL0942.",
            # Init: the two register writes the driver sends before polling.
            "[UART1/MCU] a8 1d 55 00 00 e5 a8 19 8f 00 00 af",
            # Then the repeating full-packet read - proves the poll loop runs.
            ("[UART1/MCU] 58 aa", REPEATS)
        ]
    }
,
    {
        # Presence/radar sensor (TuyaOS 3.8.18) - a third SDK line and another
        # hardware class alongside the thermostats and the clamp meter.
        #
        # This is the case that guards FLASH WRITES. The device persists a 4K
        # sector during start-up, and the emulator used to ignore page-program
        # and sector-erase opcodes entirely - the opcode field was decoded from
        # the wrong bits of REG_FLASH_OPERATE_SW - so the firmware read back
        # stale bytes, failed its own verify with "Err write addr:0x001f1000"
        # and called bk_reboot, looping forever. With writes implemented it runs
        # on to normal operation and drives its MCU. Any firmware that saves
        # state at boot depends on this.
        #
        # A simulated MCU is attached to UART1 (--tuyamcu, src/tuyamcu.py) and,
        # like the real MCU wired to this presence sensor, answers in the form
        # TuyaOS 3.x expects: --tuyamcu-raw replies to the product query with
        # the raw 16-byte product id + short version (not JSON), and
        # --tuyamcu-pid gives the device's OWN licensed id, recovered from its
        # gw_bi KV ('pk':'o9a6at9cyfchb47y'). With the right form and id the
        # device accepts the record (its stored product_key matches our input)
        # and advances into the working-mode query (0x02) and Wi-Fi link setup
        # - far past the heartbeat loop it idles in with nothing attached. The
        # heartbeat is asserted once, not repeated: once the peer answers the
        # device advances instead of idling, so handshake progress is the
        # liveness signal. (A/B over a shared instruction budget: with peer,
        # heartbeat @18.1M / QUERY_PRODUCT @19.1M; without, no 0x01 by 33.1M.)
        "name": "BK7231N Tuya NAS-PS10 Presence Sensor sends TuyaMCU heartbeats",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7231N_Tuya_NAS-PS10_PresenceSensor_TuyaMCU_TuyaOS_3.8.18.bin"),
        "args": ["--only-uart", "--uart1-hex", "--tuyamcu",
                 "--tuyamcu-pid", "o9a6at9cyfchb47y", "--tuyamcu-raw", "-key", "TUYA"],
        "timeout": 420,
        "expected_strings": [
            "< TuyaOS V:3.8.18 BS:40.00_PT:2.3_LAN:3.5_CAD:1.0.5_CD:1.0.0 >",
            # Reached normal operation - it rebooted here before flash writes
            # worked, so this line is the flash-write regression guard.
            "mf_init succ",
            "current kv info, addr: 1ed000, cnt: 16, is valid: 0",
            "mirror kv info, addr: 1cf000, cnt: 16, is valid: 0",
            "0x9a 0xe5 0x6a 0x9b 0x84 0x26 0x71 0x1a 0x9f 0x6a 0xdb 0x77 0x34 0xf9 0xae 0xf7",
            "read baud rate 9600",
            "[UART1/MCU] 55 aa 00 00 00 00 ff",
            # Peer-unblocked: the product query, never sent without a peer.
            "55 aa 00 01 00 00 00",
            # The device ACCEPTED our raw product record - its stored key
            # matched the id we sent (JSON is rejected on length; raw is not).
            "gw_cntl->gw_if.product_key:o9a6at9cyfchb47y, input:o9a6at9cyfchb47y",
            # ...and advances to the working-mode query (0x02): real forward
            # progress past the product stage, into Wi-Fi link setup.
            "55 aa 00 02 00 00 01",
        ]
    }
,
    {
        # An extra TuyaOS 3.x TuyaMCU dump for breadth: an EV charger (Afyeev
        # GD4301, added from FlashDumps), a device CLASS the suite did not
        # cover, on the SDK 3.1.17 line. Like the other 3.x dumps it wants the
        # raw product form, so --tuyamcu-raw plus its own licensed id (from the
        # gw_bi KV 'pk', which the dump filename also records: dsmsam7xpb3ht7rl)
        # get it past the product query: it accepts the record (stored
        # product_key matches our input), updates its product id, sends the
        # working-mode query (0x02) and moves into Wi-Fi link setup - the full
        # stock-TuyaMCU advance, on one more device class and SDK point.
        "name": "BK7231N Tuya Afyeev GD4301 EV Charger accepts product, advances to working-mode",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7231N_Tuya_Afyeev_GD4301_EVCharger_TuyaMCU_3.1.17.bin"),
        "args": ["--only-uart", "--uart1-hex", "--tuyamcu",
                 "--tuyamcu-pid", "dsmsam7xpb3ht7rl", "--tuyamcu-raw", "-key", "TUYA"],
        "timeout": 420,
        "expected_strings": [
            "bk7231n_common_user_config_ty:3.1.17",
            "mf_init succ",
            # The MCU link is up and the module talks first (heartbeat).
            "[UART1/MCU] 55 aa 00 00 00 00 ff",
            # Peer-unblocked: the product query, never sent without a peer.
            "55 aa 00 01 00 00 00",
            # Accepted our raw product record - stored key matched our input.
            "gw_cntl->gw_if.product_key:dsmsam7xpb3ht7rl, input:dsmsam7xpb3ht7rl",
            # ...and advances to the working-mode query (0x02).
            "55 aa 00 02 00 00 01",
        ]
    },
    {
        # One more TuyaOS 3.x TuyaMCU dump for device-class breadth: a
        # 10-gang wall switch (from FlashDumps, SDK 3.1.28) - a multi-relay
        # switch panel, a class distinct from the meters, sensors, charger
        # and thermostats already here. Same raw-form handshake as the other
        # 3.x dumps: --tuyamcu-raw + its licensed id (ni2ztksbndubd9rf) get
        # it past the product query; it accepts the record (stored
        # product_key matches our input), updates its product id, sends the
        # working-mode query (0x02) and moves into Wi-Fi link setup.
        "name": "BK7231N Tuya 10-Gang Wall Switch accepts product, advances to working-mode",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7231N_Tuya_10Gang_WallSwitch_TuyaMCU_3.1.28.bin"),
        "args": ["--only-uart", "--uart1-hex", "--tuyamcu",
                 "--tuyamcu-pid", "ni2ztksbndubd9rf", "--tuyamcu-raw", "-key", "TUYA"],
        "timeout": 420,
        "expected_strings": [
            "bk7231n_common_user_config_ty:3.1.28",
            # Paired: skips manufacturing test.
            "have actived over 15 min, not enter mf_init",
            "mf_init succ",
            # The MCU link is up and the module talks first (heartbeat).
            "[UART1/MCU] 55 aa 00 00 00 00 ff",
            # Peer-unblocked: the product query, never sent without a peer.
            "55 aa 00 01 00 00 00",
            # Accepted our raw product record - stored key matched our input.
            "gw_cntl->gw_if.product_key:ni2ztksbndubd9rf, input:ni2ztksbndubd9rf",
            # ...and advances to the working-mode query (0x02).
            "55 aa 00 02 00 00 01",
        ]
    },
    {
        # The 2.x SDK generation, finally reachable. The ATORCH AT4P energy
        # meter (stock Tuya 2.1.17) wedges TWICE in RF/BLE init, on the XVR
        # busy-bit spins at 0x9000F8 (RF-cal) and 0x900000 (BLE llm_init) -
        # both the 'set bit 31, wait for hardware to clear it' pattern (see
        # the read hook and scratch/ble_stall.py). --xvr-selfclear models
        # those bits as self-clearing, which walks it past both spins, through
        # BLE init and mf_init to the TuyaMCU link. From there it runs the
        # raw-form handshake like the 3.x dumps: --tuyamcu-raw + its licensed
        # id (tjtigg991kvoiiqi) -> it accepts the product record and advances
        # to the working-mode query (0x02). Only 2.x dump in the suite. The
        # flag is opt-in precisely because other dumps use 0x9000F8 the
        # OPPOSITE way (they need bit 31 to stay set - see the read hook), so
        # nothing else is affected. BLE+RF init makes this a heavier boot,
        # hence the generous timeout.
        "name": "BK7231N Tuya ATORCH AT4P Meter (SDK 2.1.17) boots past BLE, accepts product",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7231N_Tuya_ATORCH_AT4P_EnergyMeter_TuyaMCU_2.1.17.bin"),
        "args": ["--only-uart", "--uart1-hex", "--xvr-selfclear", "--tuyamcu",
                 "--tuyamcu-pid", "tjtigg991kvoiiqi", "--tuyamcu-raw", "-key", "TUYA"],
        "timeout": 420,
        "expected_strings": [
            "bk7231n_common_user_config_ty:2.1.17",
            "mf_init succ",
            # Past the BLE gate and RF-cal (the point of --xvr-selfclear): it
            # reached the TuyaMCU link and sent a heartbeat.
            "[UART1/MCU] 55 aa 00 00 00 00 ff",
            # Peer-unblocked: the product query, never sent without a peer.
            "55 aa 00 01 00 00 00",
            # Accepted our raw product record and advanced to working-mode.
            "55 aa 00 02 00 00 01",
        ]
    },
    {
        # A second XVR-selfclear dump on a different 2.x sub-version: the
        # PC321 3-phase power meter (stock Tuya 2.0.2, from FlashDumps). Like
        # ATORCH it wedges on the 0x9000F8 / 0x900000 busy spins;
        # --xvr-selfclear walks it past both, through BLE init to the TuyaMCU
        # link, where its raw-form product record (licensed id
        # gqmmtjclqb7reg5p) is accepted and it advances to the working-mode
        # query (0x02). Adds SDK 2.0.2 (vs ATORCH's 2.1.17) and a distinct
        # device class (3-phase metering).
        "name": "BK7231N Tuya PC321 3-Phase Meter (SDK 2.0.2) boots past BLE, accepts product",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7231N_Tuya_PC321_3PhaseMeter_TuyaMCU_2.0.2.bin"),
        "args": ["--only-uart", "--uart1-hex", "--xvr-selfclear", "--tuyamcu",
                 "--tuyamcu-pid", "gqmmtjclqb7reg5p", "--tuyamcu-raw", "-key", "TUYA"],
        "timeout": 420,
        "expected_strings": [
            "bk7231n_common_user_config_ty:2.0.2",
            "mf_init succ",
            # Past the BLE gate and RF-cal via --xvr-selfclear: reached the
            # TuyaMCU link and sent a heartbeat.
            "[UART1/MCU] 55 aa 00 00 00 00 ff",
            # Peer-unblocked: the product query.
            "55 aa 00 01 00 00 00",
            # Accepted our raw product record and advanced to working-mode.
            "55 aa 00 02 00 00 01",
        ]
    },
    {
        # Device-class breadth on the flag-reachable 2.x line: a security
        # alarm / siren (A03CB3S, stock Tuya 2.1.17, from FlashDumps) - a
        # class the suite did not cover. Like ATORCH/PC321 it wedges on the
        # XVR busy spins; --xvr-selfclear walks it past both to the TuyaMCU
        # link, where its raw-form product record (licensed id
        # ztoh9ka787lzjkpy) is accepted and it advances to working-mode (0x02).
        "name": "BK7231N Tuya A03CB3S Alarm (SDK 2.1.17) boots past BLE, accepts product",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7231N_Tuya_A03CB3S_Alarm_TuyaMCU_2.1.17.bin"),
        "args": ["--only-uart", "--uart1-hex", "--xvr-selfclear", "--tuyamcu",
                 "--tuyamcu-pid", "ztoh9ka787lzjkpy", "--tuyamcu-raw", "-key", "TUYA"],
        "timeout": 420,
        "expected_strings": [
            "bk7231n_common_user_config_ty:2.1.17",
            "mf_init succ",
            # Past the BLE gate and RF-cal via --xvr-selfclear: reached the
            # TuyaMCU link and sent a heartbeat.
            "[UART1/MCU] 55 aa 00 00 00 00 ff",
            # Peer-unblocked: the product query.
            "55 aa 00 01 00 00 00",
            # Accepted our raw product record and advanced to working-mode.
            "55 aa 00 02 00 00 01",
        ]
    },
    {
        # A real single-colour (white-only) PWM bulb, chosen because its own
        # stored Tuya config declares pwmhz:3000 - so the decoded frequency is
        # checked against the DEVICE's own claim rather than against our
        # arithmetic. 26 MHz / 8666 = 3000.2 Hz, matching that declaration.
        #
        # It also drives P24 and P26, i.e. PWM4 and PWM5, whose period
        # registers (+0xB8, +0xC4) sit near the top of the captured window.
        # Woox and Arlec both use P6/P8, so no real firmware covered the high
        # channels until this one.
        #
        # The stored config maps c_pin:26 and w_pin:24, so PWM5 (P26) is the
        # COLD channel and PWM4 (P24) the warm one. Observed duties are 0x21DA
        # on PWM5 and 0x0000 on PWM4 - i.e. the bulb sits at full COLD white.
        # That is also why PWM_CTL reads 0x100000: only PWM5 is actually
        # running, which is consistent with rather than contradicted by both
        # channels having a configured period.
        "name": "BK7231T Tuya Geeni BW223 Filament Bulb (single-colour CW PWM) boots",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7231T_Tuya_Geeni_BW223_FilamentBulb_CW_PWM_3000Hz_1.1.1.bin"),
        "args": ["--only-uart", "-key", "TUYA"],
        "timeout": 300,
        "expected_strings": [
            "key_addr: 0x1ee000",
            "oem_bk7231s_light_ty_oldDp:1.1.1",
            "< TUYA IOT SDK V:1.0.2 BS:40.00_PT:2.2_LAN:3.3_CAD:1.0.2_CD:1.0.0 >",
            # Paired dump: it skips manufacturing test rather than parking in it.
            "mf_init succ",
        ],
        "expected_pins": {24: 0x48, 26: 0x48},
        # period 0x21DA = 8666 = 26 MHz / 3000, exactly what pwmhz:3000 implies.
        "expected_pwm": {4: (0x21DA, 0x0000), 5: (0x21DA, 0x21DA)},
    },
    {
        # A five-channel RGBCW bulb - the widest PWM configuration in the
        # suite, driving PWM0, 1, 2, 4 and 5 at once from a single image.
        #
        # Two things make it worth having beyond channel count:
        #  - PWM1 (P7) is exercised by real firmware here for the first time;
        #    every other real dump uses P6/P8 or P24/P26.
        #  - PWM2 is mid-ramp while the rest sit at zero, so this is the only
        #    real dump where the channels differ from one another at all.
        #
        # PWM2's DUTY is deliberately not asserted. It was measured at 0x6590
        # (100%) when the harness stops and 0x375A (54.5%) in longer manual
        # runs - the bulb ramps during light init, so the duty is a function of
        # how far the boot got, not an invariant. The period is set once at
        # init and is stable, so that is what the case pins.
        #
        # All five periods are 0x6590 = 26000 = 26 MHz / 1000, matching the
        # pwmhz:1000 this bulb's own stored config declares.
        #
        # PWM_CTL is deliberately NOT asserted: it is written late in light
        # init and was observed as both 0x0 and 0x100 depending on how far the
        # run got, so it is genuinely timing-dependent here.
        "name": "BK7231T Tuya A60 RGBCW Bulb (5-channel PWM) boots",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7231T_Tuya_A60_SmartBulb_RGBCWW_5ch_PWM_1000Hz.bin"),
        "args": ["--only-uart", "-key", "TUYA"],
        "timeout": 300,
        "expected_strings": [
            "< TUYA IOT SDK V:2.0.0 BS:30.06_PT:2.2_LAN:3.3_CAD:1.0.2_CD:1.0.0 >",
            "mf_init succ",
            # Paired: it skips manufacturing test instead of parking in it.
            "have actived over 15 min, not enter mf_init",
        ],
        "expected_pins": {6: 0x48, 7: 0x48, 8: 0x48, 24: 0x48, 26: 0x48},
        "expected_pwm": {0: (0x6590, 0x0000), 1: (0x6590, 0x0000),
                         2: (0x6590, None),   # ramps; period only
                         4: (0x6590, 0x0000), 5: (0x6590, 0x0000)},
    },
    {
        # The only real firmware in the suite that drives PWM3. Together with
        # the A60 (PWM0/1/2/4/5) this completes real-device coverage of all six
        # PWM channels - previously PWM1 and PWM3 existed only in the synthetic
        # OpenBeken images, where the pin and frequency are chosen by us rather
        # than by a shipped product.
        #
        # PWM3's period register sits at PWM_BASE + 0x08 + 12*3 = +0xAC, the
        # same offset the synthetic 1400 Hz case pins down. A real product
        # landing on it independently is what rules out that offset being an
        # artefact of how we drive OpenBeken.
        #
        # A CW bulb with the warm channel at 100% and the cold one off. Both
        # duties are stable here, unlike the A60's ramping PWM2, so both are
        # asserted.
        "name": "BK7231T Tuya Ledvance WW/CW Bulb (PWM3) boots",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7231T_Tuya_Ledvance_WW_CW_Bulb_PWM_2000Hz_1.0.8.bin"),
        "args": ["--only-uart", "-key", "TUYA"],
        "timeout": 300,
        "expected_strings": [
            "oem_bk7231s_ty_ffc_db_ldv",
            "< TUYA IOT SDK V:2.0.0 BS:30.06_PT:2.2_LAN:3.3_CAD:1.0.2_CD:1.0.0 >",
            "mf_init succ",
        ],
        "expected_pins": {8: 0x48, 9: 0x48},
        # 0x32C8 = 13000 = 26 MHz / 2000, matching this bulb's pwmhz:2000.
        "expected_pwm": {2: (0x32C8, 0x0000), 3: (0x32C8, 0x32C8)},
    },
    {
        # Picked for its period arithmetic. 26 MHz / 600 = 43333.33, and
        # HAL_PIN_PWM_Start uses integer division, so the register holds 43333
        # (0xA945) - which decodes BACK to 600.0046 Hz, not 600. Every other
        # real device in the suite either divides evenly or is close enough to
        # hide the truncation. Asserting the raw register pins the truncation
        # itself rather than a rounded frequency.
        #
        # Also the first five-channel PWM_CTL cross-check: 0x110111 enables
        # PWM0, 1, 2, 4 and 5, exactly the set decoded from the period
        # registers, corroborating the channel numbering from a different
        # register than the one being decoded.
        #
        # All five duties are 0 - the bulb is off in this dump - and stable,
        # so unlike the A60's ramping channel they can all be asserted.
        "name": "BK7231T Tuya ROOMLUX A60 RGBCW Bulb (600 Hz PWM) boots",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7231T_Tuya_ROOMLUX_B15515_A60_RGBCW_PWM_600Hz_1.1.2.bin"),
        "args": ["--only-uart", "-key", "TUYA"],
        "timeout": 300,
        "expected_strings": [
            "[oem_bk7231s_light_ty] [1.1.2]",
            "< TUYA IOT SDK V:2.0.0 BS:30.06_PT:2.2_LAN:3.3_CAD:1.0.2_CD:1.0.0 >",
            "mf_init succ",
        ],
        "expected_pins": {6: 0x48, 7: 0x48, 8: 0x48, 24: 0x48, 26: 0x48},
        # 0xA945 = 43333 = floor(26 MHz / 600); its stored config says pwmhz:600.
        "expected_pwm": {0: (0xA945, 0x0000), 1: (0xA945, 0x0000),
                         2: (0xA945, 0x0000),
                         4: (0xA945, 0x0000), 5: (0xA945, 0x0000)},
    },
    {
        # A strip CONTROLLER rather than a bulb - different product category,
        # and the only real device driving its colours through the UPPER half
        # of the channel map: r/g/b on P9/P24/P26 = PWM3/4/5, with the low
        # channels untouched. Every bulb so far clusters on P6/P7/P8.
        # oem firmware is oem_bk7231s_strip_ty 1.0.5 (Apr 2020), the oldest
        # light build in the suite.
        #
        # PWM_CTL is not asserted (observed 0x0 here - written later than the
        # harness runs, same lesson as the A60).
        "name": "BK7231T Tuya TreatLife RGB LED Strip (PWM3/4/5) boots",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7231T_Tuya_TreatLife_RGB_LEDStrip_PWM_1000Hz_1.0.5.bin"),
        "args": ["--only-uart", "-key", "TUYA"],
        "timeout": 300,
        "expected_strings": [
            "[oem_bk7231s_strip_ty] [1.0.5]",
            "< TUYA IOT SDK V:2.0.0 BS:30.05_PT:2.2_LAN:3.3_CAD:1.0.2_CD:1.0.0 >",
            "mf_init succ",
        ],
        "expected_pins": {9: 0x48, 24: 0x48, 26: 0x48},
        # 0x6590 = 26000 = 26 MHz / 1000, matching its stored pwmhz:1000.
        "expected_pwm": {3: (0x6590, 0x0000), 4: (0x6590, 0x0000),
                         5: (0x6590, 0x0000)},
    },
    {
        # Completes N-family channel coverage: its config puts the cool
        # channel on P9, which is pwm_new ch3 (group 1, SUB-CHANNEL 1) - the
        # one channel Arlec, the Plafon and Gleco all leave idle. Group 1's
        # CTRL word here (0x80000900) enables only sub 1, so the case also
        # proves the decoder honours per-sub enable bits rather than decoding
        # everything with a written period.
        #
        # A GU5.3 spotlight on IOT SDK 2.3.3 - the oldest N-family light SDK
        # in the suite next to the TuyaOS 3.3.x trio.
        #
        # It used to stop just past "ble remote ok" - 69 lines of log - which
        # looked like a crash in early init. It was the RF-calibration spin at
        # 0x9000f8: the firmware sets bit 31 and waits for the hardware to
        # clear it. With --xvr-selfclear it reaches its running application
        # (599 lines), brings the radio up and starts scanning. It is in fact
        # a PAIRED dump: given a virtual peer it joins the network stored in
        # its flash and reaches for the Tuya cloud, which the case below this
        # one covers.
        "name": "BK7231N Tuya Feconn MR16 RGBCT Bulb (pwm_new ch3) boots into its app",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7231N_Tuya_Feconn_MR16_RGBCT_5ch_PWM_1000Hz_1.3.21.bin"),
        "args": ["--only-uart", "-key", "TUYA", "--xvr-selfclear"],
        "timeout": 300,
        "expected_strings": [
            "< TUYA IOT SDK V:2.3.3 BS:40.00_PT:2.2_LAN:3.4_CAD:1.0.5_CD:1.0.0 >",
            "oem_bk7231n_light_ty:1.3.21",
            # Paired dump: skips manufacturing test at boot.
            "have actived over 15 min, not enter mf_init",
            # Past the RF-calibration stall: the radio is up and the running
            # app is retrying its connection.
            "wpa_supplicant_req_scan",
            "gw is long time(60) disconnect(stat:5), reconnect!!",
        ],
        "expected_pins": {6: 0x48, 7: 0x48, 9: 0x48, 24: 0x48, 26: 0x48},
        # All five declared channels at 26 MHz / 1000 = 0x6590; ch2 (P8) is
        # absent from its config and asserted nowhere.
        "expected_pwm": {0: (0x6590, 0x0000), 1: (0x6590, 0x0000),
                         3: (0x6590, 0x0000),
                         4: (0x6590, 0x0000), 5: (0x6590, 0x0000)},
    },
    {
        # Third N-family light, third distinct frequency: 5 kHz -> period
        # 5200 = 0x1450, again exactly what its stored pwmhz:5000 implies.
        # With Arlec at 1000 Hz and the Plafon at 16 kHz, the pwm_new decode
        # is pinned by three devices whose frequencies span 16x - no single
        # default or coincidence can satisfy all three.
        # Its config also maps the channels OPPOSITE to the Plafon (c on P26
        # here vs P8 there), so pad-to-channel numbering is exercised in both
        # directions.
        "name": "BK7231N Tuya Gleco Bulb (5ch pwm_new, 5 kHz) boots",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7231N_Tuya_Gleco_Bulb_5ch_PWM_5000Hz_CB2L_1.5.21.bin"),
        "args": ["--only-uart", "-key", "TUYA"],
        "timeout": 300,
        "expected_strings": [
            "< TuyaOS V:3.3.40 BS:40.00_PT:2.3_LAN:3.5_CAD:1.0.5_CD:1.0.0 >",
            "mf_init succ",
        ],
        "expected_pins": {6: 0x48, 7: 0x48, 8: 0x48, 24: 0x48, 26: 0x48},
        "expected_pwm": {0: (0x1450, 0x0000), 1: (0x1450, 0x0000),
                         2: (0x1450, 0x0000),
                         4: (0x1450, 0x0000), 5: (0x1450, 0x0000)},
    },
    {
        # Five pwm_new channels at once, at 16 kHz - the highest PWM frequency
        # of any device in the suite, an order of magnitude above the bulbs.
        # Period 1625 = 26 MHz / 16000 exactly, matching its stored
        # pwmhz:16000. Exercises all three pwm_new GROUPS simultaneously
        # (ch0/1 group 0, ch2 group 1, ch4/5 group 2), which the Arlec case
        # (ch0+ch2 only) does not.
        "name": "BK7231N Tuya LED RGB Plafon (5ch pwm_new, 16 kHz) boots",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7231N_Tuya_ledRGBPlafon_5ch_PWM_16000Hz_TuyaOS_3.3.43.bin"),
        "args": ["--only-uart", "-key", "TUYA"],
        "timeout": 300,
        "expected_strings": [
            "< TuyaOS V:3.3.43 BS:40.00_PT:2.3_LAN:3.5_CAD:1.0.5_CD:1.0.0 >",
            "mf_init succ",
        ],
        "expected_pins": {6: 0x48, 7: 0x48, 8: 0x48, 24: 0x48, 26: 0x48},
        # 1625 = 0x659; duties all 0 (light off at boot), stable in repeat runs.
        "expected_pwm": {0: (0x659, 0x0000), 1: (0x659, 0x0000),
                         2: (0x659, 0x0000),
                         4: (0x659, 0x0000), 5: (0x659, 0x0000)},
    },
    {
        # A PWM-driven light rather than an MCU device: the Beken itself drives
        # the LED channels through hardware PWM (its stored config declares
        # cool on P6, warm on P8 at 1000 Hz), so there is no TuyaMCU traffic on
        # UART1 by design - every other stock-Tuya case here talks to an MCU.
        #
        # Also a fourth Tuya SDK generation (TuyaOS 3.3.44) next to IOT SDK
        # 2.3.3, TuyaOS 3.8.18 and 3.11.12. Paired dump: reads its protected key
        # and reaches normal operation.
        "name": "BK7231N Tuya Arlec RGB Strip (PWM light, TuyaOS 3.3.44) boots",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7231N_Tuya_Arlec_Razer_RGBStrip_PWM_TuyaOS_3.3.44.bin"),
        "args": ["--only-uart", "-key", "TUYA"],
        "timeout": 300,
        "expected_strings": [
            "< TuyaOS V:3.3.44 BS:40.00_PT:2.3_LAN:3.5_CAD:1.0.5_CD:1.0.0 >",
            "oem_bk7231n_slide_strip_ty:1.3.3",
            "mf_init succ",
            "key_addr: 0x1ee000",
            "get key:",
            "0x34 0x8b 0xdd 0xe7 0x7b 0x9c 0x63 0xe0 0x44 0x1e 0xa7 0x1f 0x21 0x6b 0x5 0xc"
        ],
        # First N-family PWM assertions. BK7231N uses the pwm_new block at
        # 0x802B00 (three groups, T1..T4 edge registers), a different
        # peripheral from the T-family period/duty pair - these dumps sat
        # undecodable until that block was captured and decoded. Period
        # 0x6590 = 26000 = 26 MHz / 1000 matches this strip's own stored
        # pwmhz:1000; c_pin 6 -> ch0, w_pin 8 -> ch2 per its config.
        "expected_pins": {6: 0x48, 8: 0x48},
        "expected_pwm": {0: (0x6590, 0x0000), 2: (0x6590, 0x0000)}
    },
    {
        # First stock BK7238 light that actually configures and drives hardware
        # PWM (the TY-02-3CH RGB controller above never hands a pad to PWM
        # during boot). Three things make this Onforu BDB100WF-3 100W CCT
        # floodlight (TuyaOS 3.11.24, T1-U-HL 1.0.43, plaintext) worth having:
        #
        # 1. BK7238 uses the same pwm_new block at 0x802B00 as BK7231N/M (added
        #    to PWM_NEW_CHIPS in src/periph.py). Its decrypted Tuya config
        #    declares w_pin:6 (PWM0, group 0 sub 0), c_pin:26 (PWM5, group 2
        #    sub 1) and pwmhz:1000. Both channels get period 0x6590 = 26000 =
        #    26 MHz / 1000, and PWM0 is driven with TWO non-zero edge toggles
        #    (T1 = 0x171B = 5915, T2 = 0x4E75 = 20085) - a phase-shifted CW
        #    pulse that stays high for 20085 - 5915 = 14170 (0x375A, 54.5%)
        #    ticks. Every other stock pwm_new dump here sits at 0% duty, and the
        #    OpenBeken synthetic cases only set T1, so this is the first real
        #    firmware exercising two-edge toggle replay in pwm_new_channels().
        #
        # 2. Guards BK7238's 256-byte flash page-write FIFO at 0x00803024
        #    (REG_FLASH_PW_CONF). Where BK7231N's flash_write_data stages 32-byte
        #    pages (8 words) via 0x00803004, BK7238's flash_page_write resets the
        #    FIFO with 0x80000000 at 0x00803024, pushes 256 individual bytes to
        #    0x00803024, and triggers FLASH_OPCODE_PP (12) at 0x00803000 once per
        #    256-byte page. Before 0x00803024 was modelled, every 4K sector write
        #    (uf_flash_file saving light_data at 0x1e5000/0x1e7000 and
        #    simple_flash writing 0x1e4000) left bytes +0x20..+0xFF of each page
        #    erased, logging 11x "uf_open light_data err 4" and rebooting on
        #    "Err write addr:0x001e4000 size:4096".
        #
        # 3. First test asserting [BLE_ADV] exchange-memory extraction on the
        #    BLE 5.2 core (BK7238): TuyaOS 3.x advertises service UUID 0xFD50
        #    carrying the device's 16-byte key (keytfesxxsqewnqy), located via
        #    the runtime EM base register at 0x90019C rather than a fixed address.
        "name": "BK7238 Tuya Onforu 100W CCT Floodlight (pwm_new 2-toggle CW, 256B flash page write) boots",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7238_Tuya_Onforu_BDB100WF3_Floodlight_CW_PWM_1000Hz_1.0.43.bin"),
        "args": ["--only-uart", "-chip", "BK7238", "--ble-core"],
        "timeout": 420,
        "tags": ["BLE", "BLE core", "FIQ"],
        "expected_strings": [
            "device_id=0x21128000",
            "< TuyaOS V:3.11.24 BS:40.00_PT:2.3_LAN:3.5_CAD:1.0.5_CD:1.0.0 >",
            "name:oem_t1_light_ty:1.0.43",
            "key_addr: 0x1f5000",
            "0x4d 0x5d 0xbf 0x3 0x35 0x65 0x17 0x4c 0x91 0x38 0x56 0x78 0x19 0x4 0xf 0x58",
            "have actived over 15 min, not enter mf_init",
            "mf_init succ",
            "wifi soc init. pid:keytfesxxsqewnqy firmwarekey:keytfesxxsqewnqy ver:1.0.43",
            "ssid:SmartLife-2074, key:, channel: 6",
            # BLE 5.2 exchange-memory advertisement: TuyaOS 3.x UUID 0xFD50 + key.
            "uuid=0xFD50",
            "keytfesxxsqewnqy  uuid=0xFD50",
            "ble adv updated",
            "tfm_timing_storage.c:168] storage sucess",
        ],
        "unexpected_strings": [
            "uf_open light_data err",
            "Err write addr",
            "bk_reboot",
        ],
        "expected_pins": {6: 0x48, 26: 0x48},
        # Period 0x6590 = 26000 = 26 MHz / 1000 (pwmhz:1000); PWM0 high ticks
        # 0x375A = 14170 (T2 0x4E75 - T1 0x171B = 54.5% duty), PWM5 = 0%.
        "expected_pwm": {0: (0x6590, 0x375A), 5: (0x6590, 0x0000)},
    },
    {
        # First BK7238 TuyaMCU firmware in the suite (and a new device class:
        # S06Pro IR blaster + temperature/humidity station, t1_common_user_config_ir_ty
        # 1.0.1 on TuyaOS 3.11.16, T1-3S, plaintext).
        #
        # Exercises all three emulated peripheral/radio models on BK7238 in one
        # boot and proves they interlock:
        #   1. UART1 / TuyaMCU at 9600 baud (--tuyamcu --tuyamcu-raw with
        #      licensed product key jbe3snv4tki8oo9c): sends heartbeat (0x00),
        #      product query (0x01), validates both product_key and firmware_key
        #      (keyny8wdynkekrgw -> enter success_proc), and queries working
        #      mode (0x02).
        #   2. 802.11 MAC + virtual station (--wifi-peer auto): because the
        #      active KV sector (0x1fb000, wr_cnt 39) was reset to AP pairing
        #      mode (gw_wsm.stat:1, nc_tp:10), the device raises SmartLife-C8F2
        #      on channel 6; our station associates, gets a DHCP lease
        #      (192.168.176.100 from 192.168.176.1), answers ping, and captures
        #      the Tuya LAN 3.5 UDP broadcast on port 6667.
        #   3. BLE 5.2 core (--ble-core): brings up the controller, stages the
        #      UUID 0xFD50 pairing advertisement carrying jbe3snv4tki8oo9c in
        #      exchange memory, and logs ble adv updated.
        # Only once the AP and BLE stack are up does the firmware send its
        # Wi-Fi-state report (0x03, payload 0x01 = AP config mode:
        # 55 aa 00 03 00 01 01 04) back to the MCU over UART1.
        "name": "BK7238 Tuya S06Pro IR + TempHum (TuyaMCU + BLE 5.2 + Wi-Fi AP) handshakes and reports AP state",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7238_Tuya_S06Pro_IR_TempHum_TuyaMCU_T1-3S_1.0.1.bin"),
        "args": ["--only-uart", "--uart1-hex", "-chip", "BK7238", "--ble-core",
                 "--tuyamcu", "--tuyamcu-pid", "jbe3snv4tki8oo9c", "--tuyamcu-raw",
                 "--wifi-peer", "auto"],
        "timeout": 420,
        "tags": ["BLE", "BLE core", "FIQ"],
        "expected_strings": [
            "device_id=0x21128000",
            "< TuyaOS V:3.11.16 BS:40.00_PT:2.3_LAN:3.5_CAD:1.0.5_CD:1.0.0 >",
            "t1_common_user_config_ir_ty sw ver:1.0.1",
            "key_addr: 0x1f5000",
            "0x88 0xd3 0x2a 0x22 0x15 0xba 0x68 0x4b 0xca 0x9b 0x19 0x65 0x68 0xbf 0x16 0xd7",
            "have actived over 15 min, not enter mf_init",
            "mf_init succ",
            "read baud rate 9600",
            # TuyaMCU startup handshake: heartbeat, product query, working-mode query.
            "[UART1/MCU] 55 aa 00 00 00 00 ff 55 aa 00 01 00 00 00 55 aa 00 02 00 00 01",
            "gw_cntl->gw_if.product_key:jbe3snv4tki8oo9c, input:jbe3snv4tki8oo9c",
            "gw_cntl->gw_if.firmware_key:keyny8wdynkekrgw, input:keyny8wdynkekrgw",
            "enter success_proc",
            # BLE 5.2 exchange-memory advertisement + Wi-Fi state 0x03 report to MCU.
            "uuid=0xFD50",
            "jbe3snv4tki8oo9c  uuid=0xFD50",
            "ble adv updated",
            "[UART1/MCU] 55 aa 00 03 00 01 01 04",
            # Pairing AP up on air: virtual station joins, gets DHCP lease, pings, sees LAN broadcast.
            "[WIFI] found BSS 'SmartLife-C8F2'",
            "[WIFI] associated with 'SmartLife-C8F2' (AID 1)",
            "[NET] DHCP: we got 192.168.176.100/255.255.255.0 from the device (router 192.168.176.1)",
            "[NET] ping reply from 192.168.176.1",
            "Tuya LAN broadcast from 192.168.176.1:6667: Tuya LAN protocol 3.4/3.5 announcement, command 0x11, 265 bytes",
        ],
        # Both UART pairs (P0/P1 log, P10/P11 TuyaMCU) in second-function pull-up;
        # the external MCU owns the sensors and LEDs, so P6/P26 stay untouched.
        "expected_pins": {0: 0x78, 1: 0x78, 6: None, 10: 0x78, 11: 0x78, 26: None},
    },
    {
        # First paired BK7238 dump that joins its stored network as a Wi-Fi
        # station and calls the Tuya cloud: an IHSENO HAZ-IR-V3 standalone IR
        # blaster (oem_t1_irbox_mol_ty:0.0.5 on TuyaOS 3.8.31, T1-3S, plaintext).
        #
        # Unlike the two BK7231N paired cloud cases (the PC321 3-phase meter and
        # the Feconn MR16 bulb), which call the HTTPS activation endpoint with
        # no pre-shared key set and stop before ClientKeyExchange (-0x7600),
        # this dump is already bound ("already bind", psk_key / psk30_key stored
        # in KV) and connects directly to its MQTT-over-TLS broker
        # mqtts://m3-eu.lifeaiot.com:8883. The emulator recovers the image's own
        # PSK 3.0 internally, authenticates the client's encrypted Finished,
        # sends its encrypted Finished, then serves the device's MQTT 3.1 flow
        # from the offline broker. No device key is logged or placed in the UI.
        #
        # Also exercises:
        #   * fast-connect fallback: attempts tkl_wifi_station_fast_connect to
        #     the BSSID cached in flash (74:da:88:a5:9e:fb), times out, scans
        #     for 'DOM-1', associates to our virtual AP, and persists the new
        #     fast-connect record to flash ("writed fci to flash ssid=DOM-1").
        #   * bound BLE 5.2 beacon (--ble-core): UUID 0xFD50 with bound flag
        #     byte 0x4B ("50 fd 4b 0c 01 10 ...").
        #   * pin config from {reset_pin:9,wfst_pin:26,infrr:8,...}: P8 (IR RX)
        #     and P9 (reset button) in input pull-up (0x3C). After the cloud
        #     session is established, active-low P26 drives the Wi-Fi LED low.
        "name": "BK7238 Tuya IHSENO HAZ-IR-V3 IR Blaster (paired): joins stored network, BLE 5.2 adv, TLS-PSK cloud handshake",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7238_Tuya_IHSENO_HAZ_IR_V3_T1-3S_0.0.5.bin"),
        "args": ["--only-uart", "-chip", "BK7238", "--ble-core", "--wifi-peer", "auto",
                 "--pcap", "{pcap}", "--pcap-ip", "{pcap_ip}"],
        "timeout": 420,
        "tags": ["BLE", "BLE core", "FIQ"],
        "expected_strings": [
            "device_id=0x21128000",
            "sdk_info:< TuyaOS V:3.8.31 BS:40.00_PT:2.3_LAN:3.5_CAD:1.0.5_CD:1.0.0 >",
            "name:oem_t1_irbox_mol_ty:0.0.5",
            "key_addr: 0x1f5000",
            "gw_cntl->gw_wsm.stat:2",
            "have actived over 15 min, not enter mf_init",
            "mf_init succ",
            "wifi soc init. pid:keycmhjv873dhvsn firmwarekey:keycmhjv873dhvsn ver:0.0.5",
            "already bind",
            # Bound BLE 5.2 beacon (0x4B flag byte in UUID 0xFD50 service data).
            "uuid=0xFD50",
            "50 fd 4b 0c 01 10 d7 be 6e",
            "ble adv updated",
            # Fast-connect attempt -> scan fallback -> associate to 'DOM-1' -> FCI flash write -> DHCP.
            "[WIFI] device acts as a station (it authenticates): peer takes the access point role",
            "[WIFI] device probes for 'DOM-1': adopting it as our SSID",
            "[WIFI] station 80:64:7c:21:ff:7b associated to 'DOM-1'",
            "writed fci to flash ssid=DOM-1",
            "[NET] DHCP: device 80:64:7c:21:ff:7b got 192.168.1.100 (gateway/DNS 192.168.1.1)",
            # Cloud MQTT-over-TLS connection: DNS -> authenticated TLS -> the
            # real encrypted CONNECT/SUBSCRIBE/PUBLISH exchange.
            "connect to mqtt broker mqtts://m3-eu.lifeaiot.com:8883 port 8883",
            "[NET] DNS m3-eu.lifeaiot.com -> 10.200.0.1 (offline sinkhole)",
            "sends: TLS ClientHello (no server name)",
            "client hello: version 1.2, 4 cipher suites (TLS_PSK_WITH_AES_128_GCM_SHA256, TLS_PSK_WITH_AES_128_CBC_SHA256, 0xc037, 0x00ff), no server name",
            "answered with TLS_PSK_WITH_AES_128_GCM_SHA256, waiting for the key identity",
            "pre-shared key identity: 49 bytes",
            "handshake authenticated; serving encrypted MQTT offline",
            "connected to the offline broker",
            "MQTT SUBSCRIBE smart/device/in/",
            "MQTT PUBLISH smart/device/out/",
        ],
        "unexpected_strings": [
            "mbedtls_ssl_handshake returned 0x7280",
            "client Finished authentication failed",
        ],
        "pcap_expect": ["beacon", "probe request", "authentication", "association request",
                        "association response", "data", "DOM-1", "m3-eu"],
        "ip_pcap_expect": ["m3-eu", "lifeaiot"],
        "expected_pins": {0: 0x78, 1: 0x78, 8: 0x3C, 9: 0x3C, 10: 0x78, 11: 0x78, 26: 0x00},
    },
    {
        # Stock BK7231T BN-LINK metering plug from FlashDumps.  This older
        # application is useful because it exercises the 2.0.0 SDK line and
        # initializes its own relay/energy-meter service after protected-flash
        # recovery, rather than stopping at generic Tuya platform startup.
        "name": "BK7231T Tuya BN-LINK WB2S metering plug boots its relay application",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7231T_Tuya_BN-LINK_Plug_WB2S_1.0.3.bin"),
        "args": ["--only-uart", "-key", "TUYA"],
        "timeout": 180,
        "expected_strings": [
            "< TUYA IOT SDK V:2.0.0 BS:30.06_PT:2.2_LAN:3.3_CAD:1.0.2_CD:1.0.0 >",
            "oem_bk7231s_dltj:1.0.3",
            ",module: is WB2S",
            "have actived over 15 min, not enter mf_init",
            "mf_init succ",
            "gw_cntl.gw_if.product_key:keym9qkuywghyrvs, input:keym9qkuywghyrvs",
            "dltj_run_int",
            "channel 0 Init ok",
            "CREATE DB SUCCESS",
        ],
    },
    {
        # Stock one-gang WB3S-IPEX wall switch from FlashDumps.  Its SDK 1.0.1
        # application exposes the board map in its own UART output, providing
        # stable evidence that the recovered user configuration reaches the
        # relay/button/Wi-Fi LED hardware table.
        "name": "BK7231T Tuya one-gang WB3S-IPEX switch loads its hardware map",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7231T_Tuya_1G_Switch_WB3S-IPEX_1.0.4.bin"),
        "args": ["--only-uart", "-key", "TUYA"],
        "timeout": 180,
        "expected_strings": [
            "< TUYA IOT SDK V:1.0.1 BS:40.00_PT:2.2_LAN:3.3_CAD:1.0.2_CD:1.0.0 >",
            "oem_bk7231s_rnd_zero_dctrl_switch:1.0.4",
            "module:WB3S-IPEX",
            "gw_cntl.gw_if.product_key:key7axydcvmea3x9, input:key7axydcvmea3x9",
            "Channel [0] config !!!",
            "IO - relay[0]:\tpin-6\ttype high",
            "IO - button[0]:\tpin-7\ttype low",
            "IO - wifi_stat:\tpin-10\ttype high",
            "CREATE DB SUCCESS",
            "appm start advertising",
        ],
    },
    {
        # Stock paired LSC GLS A60 RGB+CCT bulb from FlashDumps.  Its exact
        # BK7231N identity selects the BLE 5.1 controller automatically.  The
        # device then joins the network retained in flash and takes TuyaOS
        # 3.3's older dynamic-TLS mode: the firmware passes one embedded
        # 32-byte bootstrap token as both mbedTLS identity and PSK.  The local
        # peer accepts that mode only for the matching decoded configuration
        # and only after the encrypted Finished authenticates, then serves the
        # bulb's real MQTT traffic without anything leaving this machine.
        "name": "BK7231N Tuya LSC GLS A60 CBLC5 joins and authenticates cloud MQTT",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7231N_Tuya_LSC_GLS_A60_CBLC5_1.6.0.bin"),
        "args": ["--only-uart", "-key", "TUYA", "-chip", "BK7231N",
                 "--wifi-peer", "auto"],
        "timeout": 180,
        "tags": ["BLE", "BLE core", "FIQ"],
        "expected_strings": [
            "BleSwitch=1",
            "first bright up",
            "mf_init succ",
            "< TuyaOS V:3.3.44 BS:40.00_PT:2.3_LAN:3.5_CAD:1.0.5_CD:1.0.0 >",
            "oem_bk7231n_light_ty:1.6.0",
            "wifi soc init. pid:key8u54q9dtru5jw firmwarekey:key8u54q9dtru5jw ver:1.6.0",
            "gw_cntl->gw_if.product_key:key8u54q9dtru5jw, input:key8u54q9dtru5jw",
            "enter success_proc",
            "ffc remote OK",
            "BLE IP Rev: 76f0dd2",
            "tkl_hci_callback_register",
            ("llm_init:312", 2),
            "ble sdk re_inited",
            "ty bt sdk init success finish",
            "app_init finished",
            "wpa_supplicant_req_scan",
            "[BLE_ADV] EM 0x00910000",
            "Start Adv",
            "ble adv && resp changed",
            "[WIFI] device acts as a station",
            "[NET] DHCP: device",
            "mqtt ip:",
            "sends: TLS ClientHello",
            "pre-shared key identity: 32 bytes",
            "handshake authenticated; serving encrypted MQTT offline",
            "connected to the offline broker",
            "MQTT SUBSCRIBE ",
            "MQTT PUBLISH ",
        ],
        "unexpected_strings": [
            "mbedtls_ssl_handshake returned 0x7280",
            "client Finished authentication failed",
        ],
    },
    {
        # A factory-state CB3S carrying Tuya's IR-box application.  This was
        # originally classified as stuck at scan_start_req_handler and covered
        # with two manual peripheral flags.  Under its real BK7231N identity,
        # BLE hardware is selected automatically and the stock image proceeds
        # through SoftAP setup into the MAC's monitor mode.  wifi_stat:1 is the
        # final callback printed by this exact image on real hardware too; the
        # capture additionally proves it keeps transmitting its unusual raw
        # authentication frame with the vendor payload "filter" while it waits.
        "name": "BK7231N Tuya CB3S IR module reaches production-monitor endpoint",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7231N_Tuya_CB3S_Factory_Module.bin"),
        "args": ["--only-uart", "-key", "TUYA", "-chip", "BK7231N",
                 "--wifi-peer", "auto", "--wifi-ssid", "tuya_mdev_test1",
                 "--pcap", "{pcap}"],
        "timeout": 90,
        "tags": ["BLE", "BLE core", "FIQ", "WiFi MAC"],
        "expected_strings": [
            "oem_bk7231n_irbox_mol_ty:1.2.1",
            "rwble_hl_init ok",
            ("gapm_cmp_evt:GAPM_GEN_RAND_NB", 2),
            "gapm_cmp_evt:BLE_STACK_OK",
            "CREATE DB SUCCESS",
            "[BLE_ADV]   Service Data",
            "uuid=0xA201",
            "mf_init succ",
            "ir_send =7",
            "ir_recv =8",
            "gw_cntl.gw_if.product_key:key54vrth5askhsj, input:key54vrth5askhsj",
            "ty_uart_ir_main_components_version:1.0.9",
            "app_init finished",
            "scan_start_req_handler",
            "Soft_AP_start",
            "hal_machw_enter_monitor_mode",
            "wifi_stat:1",
        ],
        "unexpected_strings": ["assert_param", "lld.c:404", "ASSERTION FAILED"],
        "pcap_expect": ["beacon", "probe request", "probe response", "authentication",
                        "tuya_mdev_test1", "filter"],
    },
    {
        # Factory-state one-channel CB3S switch (board marking 72-087).  Like
        # the preceding SDK 2.x image it needs both XVR busy-bit clearing and
        # the BLE controller model.  Its old endpoint was only the scan request;
        # putting the factory SSID on the emulated air makes the real firmware
        # measure it and enter product-test mode.
        "name": "BK7231N Tuya 72-087 CB3S switch finds factory network and enters product test",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7231N_Tuya_72-087.bin"),
        "args": ["--only-uart", "-key", "TUYA", "--xvr-selfclear", "--ble-core",
                 "--wifi-peer", "auto", "--wifi-ssid", "tuya_mdev_test1",
                 "--pcap", "{pcap}"],
        "timeout": 90,
        "tags": ["BLE", "BLE core", "FIQ"],
        "expected_strings": [
            "< TUYA IOT SDK V:2.3.3 BS:40.00_PT:2.2_LAN:3.4_CAD:1.0.5_CD:1.0.0 >",
            "oem_bk7231n_dctrl_switch:1.3.5",
            ("gapm_cmp_evt:GAPM_GEN_RAND_NB", 2),
            "gapm_cmp_evt:BLE_STACK_OK",
            "CREATE DB OK",
            "[ble_appm_start_advertising]",
            "mf_init succ",
            "module:CB3S",
            "channel init success",
            "app_init finished",
            "current product ssid name:tuya_mdev_test1",
            "scan_start_req_handler",
            "The current rssi is -45dB",
            "product test mode",
        ],
        "unexpected_strings": ["assert_param"],
        "pcap_expect": ["beacon", "probe request", "probe response", "tuya_mdev_test1"],
    },
    {
        # Plaintext BK7238 dump from a GBDQA protection switch.  It carries a
        # bistable relay (separate on/off coils) and metering configuration.
        # The old case stopped at app_init immediately before the paired unit's
        # network/BLE/application path.  The 5.2 controller and virtual network
        # now take it through HCI sync, metering start, its stored-network join,
        # authenticated TLS-PSK and the first encrypted MQTT cloud exchange.
        "name": "BK7238 Tuya GBDQA protection switch starts metering, BLE and cloud TLS",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7238_GBDQA_63V_16A.bin"),
        "args": ["--only-uart", "-chip", "BK7238", "--ble-core",
                 "--wifi-peer", "auto"],
        "timeout": 240,
        "tags": ["BLE", "BLE core", "FIQ"],
        "expected_strings": [
            "device_id=0x21128000",
            "< TuyaOS V:3.11.16 BS:40.00_PT:2.3_LAN:3.5_CAD:1.0.5_CD:1.0.0 >",
            "oem_t1_cutout_self_switch:1.1.0",
            ",module: is T1-U",
            ",over_cur: is 63000",
            ",rl_on1_pin: is 8",
            ",rl_off1_pin: is 6",
            ",bt1_pin: is 17",
            ",led1_pin: is 21",
            "product have measure , chip is 3",
            "key_addr: 0x1f5000",
            "calibration_main over",
            "app_init finished",
            "mf_init succ",
            "channel 0 Init ok",
            "tuya_ele_hw_init success",
            "initial BLE...",
            "rwip_heap_env addr:",
            "enter normal mode",
            "ty bt sdk init success finish",
            "dltj start success",
            "tuya_measure_chip ok",
            "connect to mqtt broker mqtts://m3.tuyacn.com:8886 port 8886",
            "pre-shared key identity: 49 bytes",
            "handshake authenticated; serving encrypted MQTT offline",
            "connected to the offline broker",
            "MQTT SUBSCRIBE smart/device/in/",
            "MQTT PUBLISH smart/device/out/",
        ],
        "unexpected_strings": [
            "mbedtls_ssl_handshake returned 0x7280",
            "client Finished authentication failed",
        ],
    },
    {
        # Plaintext BK7238 dump from a three-channel PCIe remote-boot card.
        # The old endpoint stopped after the board map.  With the 5.2
        # controller and a station on the emulated air, the real firmware also
        # starts its Tuya BLE service and open pairing AP, leases an address and
        # answers the station's ping.
        "name": "BK7238 Tuya T1-U PCIe card serves its BLE/Wi-Fi pairing interfaces",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7238_Tuya_T1U_PCIE_RemoteBoot.bin"),
        "args": ["--only-uart", "-chip", "BK7238", "--ble-core",
                 "--wifi-peer", "auto", "--pcap", "{pcap}"],
        "timeout": 180,
        "tags": ["BLE", "BLE core", "FIQ"],
        "expected_strings": [
            "device_id=0x21128000",
            "< TuyaOS V:3.11.16 BS:40.00_PT:2.3_LAN:3.5_CAD:1.0.5_CD:1.0.0 >",
            "name:t1_kaijika:1.0.4",
            ",module: is CBU",
            ",ch_num: is 3",
            ",zero_io_pin: is 24",
            ",bt1_pin: is 21",
            ",bt2_pin: is 8",
            "mf_init successfully",
            "channel 0 Init ok",
            "channel 1 Init ok",
            "channel 2 Init ok",
            "wifi soc init. pid:jlicasd8h9mzckrz firmwarekey:NULL ver:1.0.4",
            "enter success_proc",
            "device_init ok",
            "ssid:SmartLife-D1BF, key:, channel: 6",
            "BLE HCI Version = 11",
            "Stack sync",
            "[BLE_ADV]   Service Data",
            "jlicasd8h9mzckrz  uuid=0xFD50",
            "ble adv updated",
            "[WIFI] associated with 'SmartLife-D1BF' (AID 1)",
            "[NET] DHCP: we got 192.168.176.100/255.255.255.0 from the device (router 192.168.176.1)",
            "[NET] ping reply from 192.168.176.1 (seq 1)",
        ],
        "pcap_expect": ["beacon", "authentication", "association request",
                        "association response", "data", "SmartLife-D1BF"],
    },
    {
        # Full 2 MiB BK7252N camera flash.  This third-party RT-Thread image
        # brings up its Xiongmai camera application, restores device config,
        # publishes its local status and finally takes the battery sleep path.
        "name": "BK7252N Xiongmai camera restores config and enters deep sleep",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7252N_Camera_7252N.bin"),
        "args": ["--only-uart", "-chip", "BK7252N"],
        "timeout": 60,
        "expected_strings": [
            "RT-Thread Flash Abstraction Layer (V0.4.0) initialize success.",
            "ROMFS File System initialized!",
            "Enter normal mode...",
            "bk_wlan_app_init finished",
            "rf_thread_init ok",
            "xc_main, sw_ver=3.00.21.01_250723!",
            "button node have Button1",
            "button node have Button2",
            "---adc channel:0---",
            "return xdev_opt, magic=11223344",
            "IOTSDK Ver = 3.00.42.01_241230",
            "power status is off, wait to power on",
            "wake up time out to enter deep sleep",
            "---enter_deep sleep",
            "---enter deep sleep :wake up with gpio 0~31 ps: 0x2020 0x20",
        ],
    },
    {
        # Stock three-gang WB3S wall-switch image.  Besides exercising the
        # older Tuya 1.0.x BLE path, this verifies every relay, button and LED
        # in a multi-channel board map before the application begins scanning.
        "name": "BK7231T Tuya three-gang WB3S switch initializes every channel",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7231T_Tuya_3CH_Light_Switch_WB3S_1.0.7.bin"),
        "args": ["--only-uart", "-key", "TUYA"],
        "timeout": 90,
        "expected_strings": [
            "< TUYA IOT SDK V:1.0.2 BS:40.00_PT:2.2_LAN:3.3_CAD:1.0.2_CD:1.0.0 >",
            "oem_bk7231s_rnd_zero_dctrl_switch:1.0.7",
            "STACK INIT OK",
            "CREATE DB SUCCESS",
            "appm start advertising",
            "mf_init succ",
            "module:WB3S",
            "ch_num:3",
            "IO - relay[0]:\tpin-6\ttype high",
            "IO - button[0]:\tpin-24\ttype low",
            "IO - led[0]:\tpin-14\ttype high",
            "IO - relay[1]:\tpin-9\ttype high",
            "IO - button[1]:\tpin-10\ttype low",
            "IO - led[1]:\tpin-26\ttype high",
            "IO - relay[2]:\tpin-7\ttype high",
            "IO - button[2]:\tpin-8\ttype low",
            "IO - led[2]:\tpin-23\ttype high",
            "device_init ok",
            "scan_start_req_handler",
        ],
    },
    {
        # Plaintext BL2028N firmware from a Dogness D07 water fountain.  The
        # BLE core is required for its host stack to publish the product name;
        # the app then restores the pump, level and RGB controller state.
        "name": "BL2028N Dogness D07 water fountain starts BLE and water controls",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BL2028N_Dogness_D07_WaterFountain_5.0.38.bin"),
        "args": ["--only-uart", "--ble-core"],
        "timeout": 60,
        "tags": ["BLE", "BLE core", "FIQ"],
        "expected_strings": [
            "SDK Rev: 3.0.46",
            "chip id=7231a device id=18520001",
            "rwble_hl_init ok",
            "gapm_cmp_evt:BLE_STACK_OK",
            "[BLE_ADV]   Complete Name",
            "DNS_D07_410B8E",
            "5.0.38",
            "ap_ssid:DNS_D07_410B8E",
            "water_hight     1",
            "water_mode      1",
            "RGB mode        2",
            "RGB color       00FF00",
            "myconfig.water.water_sw  1",
            "JK_GET_WATER_VALUE 0",
        ],
        "unexpected_strings": ["lld.c:404"],
    },
    {
        # Plaintext BK7231M dump from a VeSync/Aubess BSD33 (BSDOG02) EU smart
        # plug on SDK 3.0.56.  Checks SCTRL chip/device ID (requires BK7231N
        # silicon identity) and uses SARADC_ADC_CTRL_CFG bit 8 during
        # calibration_main before bringing up FlashDB, BLE 5.1 advertising,
        # pwm_new metering capture and Wi-Fi scanning.
        "name": "BK7231M VeSync Aubess BSD33 BSDOG02 plug (SDK 3.0.56) calibrates, advertises BLE and scans",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7231M_VeSync_Aubess_BSD33_BSDOG02_Plug_1.0.01.bin"),
        "args": ["--only-uart", "-chip", "BK7231N"],
        "timeout": 90,
        "tags": ["BLE", "BLE core", "FIQ"],
        "expected_strings": [
            "FlashDB V1.1.2 is initialize success",
            "chip id=7231c device id=20521023",
            "calibration_main over",
            "rwble_hl_init ok",
            ("gapm_cmp_evt:GAPM_GEN_RAND_NB", 2),
            "gapm_cmp_evt:BLE_STACK_OK",
            "[THD]ves_ble",
            "company=0x06D0",
            "ff d0 06 01 d3 cf b8 4a 58 fc",
            "Device model: BSDOG02",
            "Vesync SDK version: v1.2.1-1d1881fe",
            "Firmware version: 1.0.01",
            "REG_PWM_GROUP_CTRL= 0x1c14",
            "[THD]ves_netcfg",
            "wpa rx E SCAN_STARTED",
        ],
        "unexpected_strings": ["Unsupported chip or dev", "lld.c:404"],
    },
    {
        # Newer 1.0.02 revision of the VeSync GreenSun WHOGPLUG EU smart plug
        # on SDK 3.0.56 (afc66d1).  Unlike 1.0.01 (SDK 3.0.35), 1.0.02 enforces
        # bk_check_chip_id for BK7231N silicon and runs the 0x802c08 SARADC
        # trigger path in calibration_main.
        "name": "BK7231M VeSync WHOGPLUG Plug 1.0.02 (SDK 3.0.56) calibrates, advertises BLE and scans",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7231M_VeSync_GreenSun_WHOGPLUG_Plug_1.0.02.bin"),
        "args": ["--only-uart", "-chip", "BK7231N"],
        "timeout": 90,
        "tags": ["BLE", "BLE core", "FIQ"],
        "expected_strings": [
            "SDK Rev: 3.0.56 afc66d1",
            "FlashDB V1.1.2 is initialize success",
            "chip id=7231c device id=20521023",
            "calibration_main over",
            "ble mac:fc-58-4a-a1-d3-c3",
            "rwble_hl_init ok",
            ("gapm_cmp_evt:GAPM_GEN_RAND_NB", 2),
            "gapm_cmp_evt:BLE_STACK_OK",
            "company=0x06D0",
            "ff d0 06 01 c3 d3 a1 4a 58 fc",
            "Device model: WHOGPLUG",
            "Vesync SDK version: v1.2.1-03528e63",
            "Firmware version: 1.0.02",
            "REG_PWM_GROUP_CTRL= 0x1c14",
            "[THD]ves_netcfg",
            "wpa rx E SCAN_STARTED",
        ],
        "unexpected_strings": ["Unsupported chip or dev", "lld.c:404"],
    },
    {
        # Plaintext BK7231M dump from a China Mobile HeJiaQin (HuiJu X1S) CB2S
        # smart metering plug on SDK 3.0.56.  Enforces bk_check_chip_id for
        # BK7231N, initializes BLE 5.1, loads its factory burninfo (CMEI/SN/MAC)
        # and looks for the factory calibration AP 'JX-ChanCe-M2'; with that
        # SSID on the emulated air it associates, gets DHCP and starts BL0937B
        # energy-meter calibration.
        "name": "BK7231M ChinaMobile HeJiaQin X1S plug joins JX-ChanCe-M2 and starts BL0937 calibration",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7231M_ChinaMobile_HeJiaQin_HuiJu_X1S_Plug_CB2S_1.0.4.bin"),
        "args": ["--only-uart", "-chip", "BK7231N",
                 "--wifi-peer", "auto", "--wifi-ssid", "JX-ChanCe-M2"],
        "timeout": 90,
        "tags": ["BLE", "BLE core", "FIQ", "WiFi MAC"],
        "expected_strings": [
            "SDK Rev: 3.0.56 fa12785",
            "chip id=7231c device id=20521023",
            "calibration_main over",
            "rwble_hl_init ok",
            ("gapm_cmp_evt:GAPM_GEN_RAND_NB", 2),
            "gapm_cmp_evt:BLE_STACK_OK",
            "======>> INIT COMPLETE",
            "Prod: 1, Version: 1.0.4",
            "burninfo.cmei: 111092600037589",
            "burninfo.sn: 1716059038400037588",
            "burninfo.wifimac: bc-fd-0c-8e-3f-78",
            "[WIFI] station bc:fd:0c:8e:3f:78 associated to 'JX-ChanCe-M2'",
            "[NET] DHCP: device bc:fd:0c:8e:3f:78 got 192.168.1.100",
            "connect JX-ChanCe-M2 success",
            "find m2 ssid",
            "do m2 cali",
            "cali, p_ref: 870, v_ref: 2200, i_ref: 395, e_ref: 272",
            "bl0937 cali start",
        ],
        "unexpected_strings": ["Unsupported chip or dev", "lld.c:404"],
    },
    {
        # 4 MB BK7231M dump from a SparkleIoT XH-CB2S Mini Switch running a
        # Quectel Matter application (svc_matter_main.cpp) on Beken SDK 3.0.56.
        # Emits its Beken OS boot banner on UART1 and its Matter stack startup
        # lines on UART2.
        "name": "BK7231M SparkleIoT XH-CB2S Matter mini switch boots into Matter init",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7231M_SparkleIoT_XH-CB2S_Mini_Switch.bin"),
        "args": ["--only-uart", "-chip", "BK7231N"],
        "timeout": 60,
        "expected_strings": [
            "SDK Rev: 3.0.56 ee14ae7f",
            "OSK Rev: F-3.0.35 ee14ae7f",
            "going Chip",
            "chip[DL][svc_matter_main.cpp:403] ql_NetCfgInitAndCalcuate",
            "chip[DL][svc_matter_main.cpp:406] ql_LoadLogFlag",
            "init success",
        ],
        "unexpected_strings": ["Unsupported chip or dev"],
    },
    {
        # Paired BK7231Q MOES 1-gang relay dump captured with fsw_cnt_key=2 in
        # flash.  On first boot it increments to 3, triggers Tuya's 3x power-
        # cycle factory reset via watchdog reboot, clears fsw_cnt_key to 0 in
        # flash, and on the second boot completes SARADC temperature calibration
        # (requiring SOC_BK7231 FIFO_EMPTY bit 28), starts its beacon/EZ monitor
        # mode and reaches wifi_stat:1.
        "name": "BK7231Q Tuya MOES Relay 1.1.3-2 completes 3x-reset WDT reboot and enters EZ pairing",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7231Q_Tuya_MOES_Relay_WA2_1.1.3-2.bin"),
        "args": ["--only-uart", "--wifi-peer", "auto"],
        "timeout": 90,
        "tags": ["WDT", "WiFi MAC"],
        "expected_strings": [
            "oem_bk7321_bk_1_switch:1.1.3",
            'read uf buf {"fsw_cnt_key":2}',
            "rst_cnt:[3]",
            "onoff cnt is ok! reset device..",
            "[WDT] watchdog expired; device reset #1",
            'read uf buf {"fsw_cnt_key":0}',
            "rst_cnt:[1]",
            "serial_no:68572db62696",
            "gw_cntl.gw_if.product_key:keys83qyuhuqrdn7, input:keys83qyuhuqrdn7",
            "device_init ok",
            "temp in flash is:315",
            "scan_start_req_handler",
            "hal_machw_enter_monitor_mode",
            "wifi_stat:1",
        ],
    },
    {
        # Factory-state BK7231Q MOES WA2 1-gang relay on Tuya IoT SDK 2.0.0
        # (firmware 1.0.6).  With SOC_BK7231 SARADC FIFO_EMPTY (bit 28) served
        # on 0x802c00 and the factory SSID on the virtual air, it clears
        # saradc_flush(), scans tuya_mdev_test1, measures RSSI -45 dB and enters
        # product test mode.
        "name": "BK7231Q Tuya MOES Relay 1.0.6 (SDK 2.0.0) scans factory SSID and enters product test",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7231Q_Tuya_MOES_Relay_WA2_1.0.6.bin"),
        "args": ["--only-uart", "--wifi-peer", "auto",
                 "--wifi-ssid", "tuya_mdev_test1"],
        "timeout": 90,
        "tags": ["WiFi MAC"],
        "expected_strings": [
            "< TUYA IOT SDK V:2.0.0 BS:30.01_PT:2.2_LAN:3.3_CAD:1.0.1_CD:1.0.0 >",
            "oem_bk7321_bk_1_switch:1.0.6",
            "IO - relay[0]:\tpin-18\ttype high",
            "IO - button[0]:\tpin-6\ttype low",
            "IO - wifi_stat:\tpin-7\ttype low",
            "hw_set_channel :1,channel_no :0",
            "temp in flash is:315",
            "[FUNC]func_init_extended OVER!!!",
            "mf_init succ",
            "current product ssid name:tuya_mdev_test1",
            "scan_start_req_handler",
            "flag:1,rssi:-45",
            "product test mode",
            "wifi_stat:1",
        ],
    },
    {
        # Factory-state BK7231Q MOES WA2 1-gang relay on Tuya IoT SDK 2.0.0
        # (firmware 1.0.4, rst_cnt:1).  Configures P18 (relay high), P6 (button
        # pull-up input) and P7 (Wi-Fi status LED), completes SARADC
        # calibration and enters product test mode on tuya_mdev_test1.
        "name": "BK7231Q Tuya MOES Relay 1.0.4 (SDK 2.0.0) maps GPIO and enters product test",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7231Q_Tuya_MOES_Relay_WA2_1.0.4.bin"),
        "args": ["--only-uart", "--wifi-peer", "auto",
                 "--wifi-ssid", "tuya_mdev_test1"],
        "timeout": 90,
        "tags": ["WiFi MAC"],
        "expected_strings": [
            "< TUYA IOT SDK V:2.0.0 BS:30.01_PT:2.2_LAN:3.3_CAD:1.0.1_CD:1.0.0 >",
            "oem_bk7321_bk_1_switch:1.0.4",
            "IO - relay[0]:\tpin-18\ttype high",
            "IO - button[0]:\tpin-6\ttype low",
            "IO - wifi_stat:\tpin-7\ttype low",
            "hw_set_channel :1,channel_no :0",
            "temp in flash is:315",
            "rst_cnt:1",
            "current product ssid name:tuya_mdev_test1",
            "scan_start_req_handler",
            "flag:1,rssi:-45",
            "product test mode",
            "wifi_stat:1",
        ],
        "expected_pins": {6: 0x3C, 18: 0x00},
    },
    {
        # Second BK7231Q MOES WA2 1.0.4 unit dump (rst_cnt:2).  Without the
        # factory SSID on air, its default power-on state drives the relay P18
        # high (0x02), configures the button P6 as pull-up input (0x3C) and LED
        # P7 low (0x00), then starts scanning.
        "name": "BK7231Q Tuya MOES Relay 1.0.4-2 (rst_cnt:2) drives relay high and scans",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7231Q_Tuya_MOES_Relay_WA2_1.0.4-2.bin"),
        "args": ["--only-uart"],
        "timeout": 90,
        "expected_strings": [
            "< TUYA IOT SDK V:2.0.0 BS:30.01_PT:2.2_LAN:3.3_CAD:1.0.1_CD:1.0.0 >",
            "oem_bk7321_bk_1_switch:1.0.4",
            "IO - relay[0]:\tpin-18\ttype high",
            "IO - button[0]:\tpin-6\ttype low",
            "IO - wifi_stat:\tpin-7\ttype low",
            "relay_stat:1",
            "hw_set_channel :1,channel_no :0",
            "temp in flash is:315",
            "[FUNC]func_init_extended OVER!!!",
            "mf_init succ",
            "rst_cnt:2",
            "current product ssid name:tuya_mdev_test1",
            "scan_start_req_handler",
        ],
        "expected_pins": {6: 0x3C, 7: 0x00, 18: 0x02},
    },
    {
        # First BK7231U dump in the suite: a C-Chip C-8133U RGB+CCT LED driver
        # running AliOS Things.  Loads RGBLight_Param from flash, configures
        # five PWM pins (P7/P8/P9/P24/P26) in second-function mode (0x48),
        # starts a Wi-Fi scan and triggers its 3x power-cycle reset handler.
        "name": "BK7231U C-Chip C-8133U AliOS LED driver (reset_counts:3) configures 5ch PWM and activates reset",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7231U_C-Chip_C-8133U_LED_Driver_AliOS_Reset3.bin"),
        "args": ["--only-uart", "-chip", "BK7231U"],
        "timeout": 90,
        "expected_strings": [
            "Welcome to AliOS Things",
            "reset_counts get from flash:3",
            "RGBLight_Param.LightSwitch:1",
            "RGBLight_Param.Brightness:50",
            "RGBLight_Param.ColorTemperature:4500",
            "RGBLight_Param.Red:255",
            "init pwm pin !",
            "scan_start_req_handler",
            "[ResetCounts] reset_counts:3",
            "[ResetCounts] active the reset function",
        ],
        "expected_pins": {7: 0x48, 8: 0x48, 9: 0x48, 24: 0x48, 26: 0x48},
    },
    {
        # Companion BK7231U C-Chip C-8133U AliOS Things dump captured at
        # reset_counts:1.  Configures all five PWM pins (P7/P8/P9/P24/P26) and
        # sets PWM0 to 500 Hz (period 0xCB20 = 52000 ticks at 26 MHz), starts
        # scanning and keeps normal state without triggering the 3x reset.
        "name": "BK7231U C-Chip C-8133U AliOS LED driver (reset_counts:1) configures 500 Hz PWM without resetting",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7231U_C-Chip_C-8133U_LED_Driver_AliOS_Reset1.bin"),
        "args": ["--only-uart", "-chip", "BK7231U"],
        "timeout": 90,
        "expected_strings": [
            "Welcome to AliOS Things",
            "reset_counts get from flash:1",
            "RGBLight_Param.LightSwitch:1",
            "RGBLight_Param.Brightness:50",
            "RGBLight_Param.ColorTemperature:4500",
            "init pwm pin !",
            "scan_start_req_handler",
            "[ResetCounts] reset_counts:1",
        ],
        "unexpected_strings": ["[ResetCounts] active the reset function"],
        "expected_pins": {7: 0x48, 8: 0x48, 9: 0x48, 24: 0x48, 26: 0x48},
        "expected_pwm": {0: (0xCB20, 0x0)},
    },
    {
        # Alternate BK7231U dump from the C-Chip C-8133U LED driver running
        # RT-Thread 3.1.0 instead of AliOS Things.  Calibrates RF, initializes
        # lwIP 2.0.2, registers station and soft-AP WLAN devices, and mounts
        # the FAL partition table (bootloader/app/download/param1..4).
        "name": "BK7231U C-Chip C-8133U RT-Thread 3.1.0 firmware mounts FAL partitions",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7231U_C-Chip_C-8133U_LED_Driver_RTThread.bin"),
        "args": ["--only-uart", "-chip", "BK7231U"],
        "timeout": 90,
        "expected_strings": [
            "Thread Operating System",
            "3.1.0 build Apr  8 2021",
            "lwIP-2.0.2 initialized!",
            "register station wlan device sucess!",
            "register soft-ap wlan device sucess!",
            "beken wlan hw init",
            "| bootloader | beken_onchip_crc | 0x00000000 | 0x00010000 |",
            "| app        | beken_onchip_crc | 0x00010000 | 0x00110000 |",
            "RT-Thread Flash Abstraction Layer (V0.4.0) initialize success.",
        ],
    },
    {
        # Plaintext BK7231U dump from a C-Chip CC8000 / Hi-Link HLK-B30
        # development board running RT-Thread 3.1.0.  Completes RF calibration
        # from flash (temp 280, xtal 48, pwr_gain 12), initializes lwIP 2.0.2,
        # registers station and soft-AP WLAN devices, and mounts its FAL
        # partition table.
        "name": "BK7231U C-Chip CC8000 HLK-B30 dev board boots RT-Thread 3.1.0 and FAL partitions",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7231U_C-Chip_CC8000_HLK-B30_DevBoard.bin"),
        "args": ["--only-uart", "-chip", "BK7231U"],
        "timeout": 90,
        "expected_strings": [
            "Thread Operating System",
            "3.1.0 build Apr  8 2021",
            "read txpwr tab from flash success",
            "temp in flash is:280",
            "xtal inital:280, 70, 16",
            "lwIP-2.0.2 initialized!",
            "register station wlan device sucess!",
            "register soft-ap wlan device sucess!",
            "| download   | beken_onchip     | 0x00132000 | 0x000c4000 |",
            "RT-Thread Flash Abstraction Layer (V0.4.0) initialize success.",
        ],
    },
    {
        # Plaintext BK7231U dump from an Arnoo AiDot RGBW A19 (2nd Gen,
        # light.A001126) smart bulb built on a Leedarson CC8000 module
        # (firmware V2.3.72::V2.04.51).  Opens the Wi-Fi MAC RX filter
        # (0x7fffffde) into monitor/sniffing mode, configures P2/P3/P20/P21
        # (0x3C) and brings up rw_main.
        "name": "BK7231U Arnoo AiDot RGBW A19 bulb (Leedarson CC8000 2.0.4.51) enters Wi-Fi monitor pairing",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7231U_Arnoo_AiDot_RGBW_A19_Leedarson_CC8000_2.0.4.51.bin"),
        "args": ["--only-uart", "-chip", "BK7231U", "--wifi"],
        "timeout": 90,
        "tags": ["WiFi MAC"],
        "expected_strings": [
            "[WIFI] device RX filter 0x7fffffde: monitor mode",
            "enter [cc8000::V2.3.72::V2.04.51] main ok ; tick=172",
            "-----rw_main task init----",
            "-----rw_main  start----",
        ],
        "expected_pins": {2: 0x3C, 3: 0x3C, 20: 0x3C, 21: 0x3C},
    },
    {
        # Earlier V2.2.18::V2.03.47 revision of the Arnoo Winees WS010078261
        # AiDot RGBW A60 (2nd Gen, light.A001139) smart bulb on Leedarson
        # CC8000 (BK7231U).  Brings up its pairing Soft-AP 'SmartLight-5783' on
        # channel 7 (192.168.10.1), hands out 192.168.10.2 over DHCP to the
        # emulated station and answers ICMP echo.
        "name": "BK7231U Arnoo Winees AiDot RGBW A60 bulb (Leedarson CC8000 2.0.3.47) hosts SmartLight-5783 AP and answers ping",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7231U_Arnoo_Winees_AiDot_RGBW_A60_Leedarson_CC8000_2.0.3.47.bin"),
        "args": ["--only-uart", "-chip", "BK7231U", "--wifi-peer", "auto"],
        "timeout": 90,
        "tags": ["WiFi MAC"],
        "expected_strings": [
            "enter [cc8000::V2.2.18::V2.03.47] main ok ; tick=173",
            "-----rw_main task init----",
            "-----rw_main  start----",
            "[WIFI] found BSS 'SmartLight-5783' 1c:d6:bd:d4:57:82 ch 7 (open)",
            "[WIFI] associated with 'SmartLight-5783' (AID 1)",
            "[NET] DHCP: we got 192.168.10.2/255.255.255.0 from the device (router 192.168.10.1)",
            "[NET] ARP: device 192.168.10.1 is at 1c:d6:bd:d4:57:82",
            "[NET] ping reply from 192.168.10.1 (seq 1)",
        ],
        "expected_pins": {2: 0x3C, 3: 0x3C, 20: 0x3C, 21: 0x3C, 23: 0x48},
    },
    {
        # Newer V2.3.72::V2.04.51 revision of the Arnoo Winees WS010078261
        # AiDot RGBW A60 (2nd Gen, light.A001139) smart bulb on Leedarson
        # CC8000 (BK7231U).  Unlike 2.0.3.47 (which switches to Soft-AP mode),
        # 2.0.4.51 stays in Wi-Fi monitor/sniffing mode (0x7fffffde) after
        # starting rw_main.
        "name": "BK7231U Arnoo Winees AiDot RGBW A60 bulb (Leedarson CC8000 2.0.4.51) stays in Wi-Fi monitor pairing",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7231U_Arnoo_Winees_AiDot_RGBW_A60_Leedarson_CC8000_2.0.4.51.bin"),
        "args": ["--only-uart", "-chip", "BK7231U", "--wifi-peer", "auto"],
        "timeout": 90,
        "tags": ["WiFi MAC"],
        "expected_strings": [
            "[WIFI] device RX filter 0x7fffffde: monitor mode",
            "enter [cc8000::V2.3.72::V2.04.51] main ok ; tick=172",
            "-----rw_main task init----",
            "-----rw_main  start----",
        ],
        "unexpected_strings": ["found BSS 'SmartLight"],
        "expected_pins": {2: 0x3C, 3: 0x3C, 20: 0x3C, 21: 0x3C},
    },
    {
        # Plaintext BK7252N dump from a CY365 Car IP camera
        # (EEE-249530-GDVHL, firmware 1.7.0).  During RF calibration it sets
        # bit 31 on 0x00900000 (XVR) at 0x0d4ba4 and waits for it to clear;
        # --xvr-selfclear walks it past that spin so it configures SDIO
        # P14..P19 (0x378), button P7 (0x3C) and output P5 (0x02), switches
        # the Wi-Fi MAC from monitor mode (0x7fffffde) to normal reception
        # (0x3503848c) and probes for networks as a station.
        "name": "BK7252N CY365 Car IP camera 1.7.0 (--xvr-selfclear) completes RF cal and probes as station",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7252N_CY365_Car_IP_EEE-249530-GDVHL_1.7.0.bin"),
        "args": ["--only-uart", "-chip", "BK7252N", "--xvr-selfclear",
                 "--wifi-peer", "auto"],
        "timeout": 90,
        "tags": ["WiFi MAC", "XVR"],
        "expected_strings": [
            "[WIFI] device RX filter 0x7fffffde: monitor mode",
            "[WIFI] device RX filter 0x3503848c: back to normal reception",
            "[WIFI] device acts as a station (it probes for networks): peer takes the access point role",
        ],
        "expected_pins": {
            5: 0x02, 7: 0x3C, 9: 0x00,
            14: 0x378, 15: 0x378, 16: 0x378, 17: 0x378, 18: 0x378, 19: 0x378,
        },
    },
    {
        # Plaintext BK7252N dump from a square INO-A13N-V2.2 battery camera
        # running xciot sw_ver=3.00.21.01_250512 (IOTSDK 3.00.42.01_241230).
        # Contrasts with BK7252N_Camera_7252N (sw_ver=3.00.21.01_250723): boots
        # FAL/ROMFS, initializes Button1/Button2 and SARADC channel 0
        # (battary_status:139), reads xdev_opt (magic=11223344), sets local time
        # to 2025-3-1 8:1:50, and enters deep sleep with GPIO wake mask 0x2020.
        "name": "BK7252N INO-A13N-V2.2 square camera (3.00.21.01_250512) restores RTC 2025-3-1 and enters deep sleep",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7252N_Cam_INO-A13N-V2.2_Square_3.00.21.01.bin"),
        "args": ["--only-uart", "-chip", "BK7252N"],
        "timeout": 60,
        "expected_strings": [
            "RT-Thread Flash Abstraction Layer (V0.4.0) initialize success.",
            "ROMFS File System initialized!",
            "Enter normal mode...",
            "bk_wlan_app_init finished",
            "rf_thread_init ok",
            "xc_main, sw_ver=3.00.21.01_250512!",
            "--- battary_status :139 ---",
            "return xdev_opt, magic=11223344",
            "IOTSDK Ver = 3.00.42.01_241230",
            "date: 2025-3-1, time: 8:1:50",
            "power status is off, wait to power on",
            "wake up time out to enter deep sleep",
            "---enter deep sleep :wake up with gpio 0~31 ps: 0x2020 0x20",
        ],
        "expected_pins": {
            2: 0x00, 4: 0x3F, 5: 0x3F, 9: 0x00, 12: 0x3C, 13: 0x3C,
            14: 0x378, 15: 0x378, 16: 0x378, 17: 0x378, 18: 0x378, 19: 0x378,
            22: 0x00, 23: 0x00,
        },
    },
    {
        # Plaintext BK7252N dump from a Pinmei A9 battery camera (captured via
        # BKFIL).  Unlike the powered-off INO-A13N-V2.2 and 250723 dumps, this
        # unit has power status on: it starts xled_start, reads its [iot]
        # device credentials (did PPH0A906615F0207B0, scode 760441,
        # prod.glbs.xcthings.com, MAC c8:47:8c:4c:81:2c), initializes AVSDK
        # 3.00.42.01_241230 and OSAL 1.00.01.64, spawns vtask_wor/unpack/xrpc
        # threads and registers the dp_save periodic task.
        "name": "BK7252N Pinmei A9 camera (BKFIL, power on) loads xciot identity and spawns xrpc threads",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7252N_Pinmei_A9_Camera_BKFIL_3.00.42.01.bin"),
        "args": ["--only-uart", "-chip", "BK7252N"],
        "timeout": 60,
        "expected_strings": [
            "RT-Thread Flash Abstraction Layer (V0.4.0) initialize success.",
            "ROMFS File System initialized!",
            "Enter normal mode...",
            "bk_wlan_app_init finished",
            "rf_thread_init ok",
            "power status is on",
            "-------->xled_start",
            "did = PPH0A906615F0207B0",
            "signkey = CRfaBC8kuPZ05YMIJHOzEw==",
            "scode = 760441",
            "gdomain = prod.glbs.xcthings.com",
            "wifi mac_s address is c8:47:8c:4c:81:2c -- s",
            "AVSDK VER: 3.00.42.01_241230, build:Dec 30 2024-17:39:37",
            "OSAL_VER:1.00.01.64_240206",
            "osal_plat_thr_init,name=vtask_wor,stack_size=4096",
            "osal_plat_thr_init,name=unpack,stack_size=4096",
            "osal_plat_thr_init,name=xrpc.r,stack_size=4096",
            "osal_plat_thr_init,name=xrpc.w,stack_size=4096",
            "vtask_add_work ok",
            "read wifi error,rc:0",
        ],
        "expected_pins": {
            2: 0x00, 4: 0x3F, 5: 0x3F, 9: 0x00, 12: 0x3C, 13: 0x3C,
            14: 0x378, 15: 0x378, 16: 0x378, 17: 0x378, 18: 0x378, 19: 0x378,
            22: 0x02, 23: 0x02,
        },
    },
    {
        # External SPI programmer read (NeoProgrammer EB6015) of the Pinmei A9
        # BK7252N camera after a later RTC state (2025-3-1 8:3:05 vs 8:2:30 in
        # the BKFIL dump).  Boots FAL/ROMFS, keeps power status on (P22/P23 =
        # 0x02), loads the [iot] credentials and spawns the AVSDK/OSAL/xrpc
        # worker threads.
        "name": "BK7252N Pinmei A9 camera (SPI NeoProgrammer dump, RTC 8:3:05) loads xciot identity and spawns xrpc threads",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7252N_Pinmei_A9_Camera_SPI_3.00.42.01.bin"),
        "args": ["--only-uart", "-chip", "BK7252N"],
        "timeout": 60,
        "expected_strings": [
            "RT-Thread Flash Abstraction Layer (V0.4.0) initialize success.",
            "ROMFS File System initialized!",
            "Enter normal mode...",
            "bk_wlan_app_init finished",
            "rf_thread_init ok",
            "2025-03-01 00:03:05",
            "power status is on",
            "-------->xled_start",
            "did = PPH0A906615F0207B0",
            "scode = 760441",
            "gdomain = prod.glbs.xcthings.com",
            "wifi mac_s address is c8:47:8c:4c:81:2c -- s",
            "AVSDK VER: 3.00.42.01_241230",
            "OSAL_VER:1.00.01.64_240206",
            "osal_plat_thr_init,name=vtask_wor,stack_size=4096",
            "osal_plat_thr_init,name=xrpc.r,stack_size=4096",
            "osal_plat_thr_init,name=xrpc.w,stack_size=4096",
            "vtask_add_work ok",
            "read wifi error,rc:0",
        ],
        "expected_pins": {
            2: 0x00, 4: 0x3F, 5: 0x3F, 9: 0x00, 12: 0x3C, 13: 0x3C,
            14: 0x378, 15: 0x378, 16: 0x378, 17: 0x378, 18: 0x378, 19: 0x378,
            22: 0x02, 23: 0x02,
        },
    },
    {
        # Plaintext BK7252N dump from an XCThings Linklemo A9 camera on the
        # INO-IPC-V2.3 (Blue PCB) revision.  Boots FAL/ROMFS, keeps power status
        # on (P22/P23 = 0x02), loads its distinct [iot] identity (did
        # PPH0A90E1F42CF3350, signkey V9Ja2Pz1D9mssDt5zBov9A==, scode 574818,
        # MAC c8:47:8c:70:a1:b2) and brings up AVSDK 3.00.42.01_241230 and the
        # vtask_wor thread.
        "name": "BK7252N XCThings Linklemo A9 INO-IPC-V2.3 camera loads PPH0A90E1F42CF3350 identity and starts vtask",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7252N_XCThings_Linklemo_A9_INO-IPC-V2.3.bin"),
        "args": ["--only-uart", "-chip", "BK7252N"],
        "timeout": 60,
        "expected_strings": [
            "RT-Thread Flash Abstraction Layer (V0.4.0) initialize success.",
            "ROMFS File System initialized!",
            "Enter normal mode...",
            "bk_wlan_app_init finished",
            "rf_thread_init ok",
            "did = PPH0A90E1F42CF3350",
            "signkey = V9Ja2Pz1D9mssDt5zBov9A==",
            "scode = 574818",
            "gdomain = prod.glbs.xcthings.com",
            "wifi mac_s address is c8:47:8c:70:a1:b2 -- s",
            "AVSDK VER: 3.00.42.01_241230, build:Dec 30 2024-17:39:37",
            "OSAL_VER:1.00.01.64_240206, Dec  3 2024-19:10:18",
            "osal_plat_thr_init,name=vtask_wor,stack_size=4096",
            "wait vtask start!",
        ],
        "expected_pins": {
            2: 0x00, 4: 0x3F, 5: 0x3F, 9: 0x00, 12: 0x3C, 13: 0x3C,
            14: 0x378, 15: 0x378, 16: 0x378, 17: 0x378, 18: 0x378, 19: 0x378,
            22: 0x02, 23: 0x02,
        },
    },
    {
        # Plaintext BK7252 (QFN48) dump from a Linklemo XCThings HA15 camera
        # on the IN-A15-V1.0 (INNOA15 Blue PCB) board, firmware 12.12.16
        # (RT-Thread 3.1.0 built Apr 1 2024).  Calibrates RF (tssi_th b-145,
        # g-130, BLE ch0/19/39 txpwr table, default LPF 61/61), mounts the FAL
        # partition table (app 0x10000, download 0x154000) and ROMFS, creates
        # the jpegdec thread, runs vbat_detect_test on SARADC channel 0, and
        # initializes tc_entity_init.
        "name": "BK7252 Linklemo XCThings HA15 IN-A15-V1.0 camera (12.12.16) boots FAL/ROMFS, jpegdec and vbat_detect_test",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7252_Linklemo_XCThings_HA15_IN-A15-V1.0_12.12.16.bin"),
        "args": ["--only-uart", "-chip", "BK7252"],
        "timeout": 60,
        "expected_strings": [
            "Thread Operating System",
            "3.1.0 build Apr  1 2024",
            "get rfcali_mode:0",
            "tssi_th:b-145, g-130",
            "txpwr table for ble ch0/19/39 inused",
            "NO TXID found in flash, use lpf i&q:61, 61",
            "RT-Thread Flash Abstraction Layer (V0.4.0) initialize success.",
            "creat jpegdec thread!",
            "------------------------->vbat_detect_test",
            "[ut_xciot.c      /01155] : ---adc channel:0---",
            "tc_entity_init",
            "ROMFS File System initialized!",
            "FLASH File System initialzation failed!",
            "msh />",
        ],
        "expected_pins": {
            0: 0x78, 1: 0x78, 9: 0x00,
        },
    },
    {
        # Plaintext BK7252 (QFN48) dump from a Naxclow MiniCam (A11-A9 B-V2.7
        # 240426 board, RT-Thread 3.1.0 built Aug 7 2024).  Calibrates RF
        # (tssi_th b-130, g-123, default LPF 61/61), initializes lwIP 2.0.2,
        # registers station and soft-AP WLAN devices, configures DAC/ADC audio
        # volumes (65/80), and starts com.naxclow.Camera_v0-1.1.20 for
        # DevType A9_V2.7_ALERT_A_48P.
        "name": "BK7252 Naxclow MiniCam A11-A9 B-V2.7 (1.1.20, 48P) boots audio, WLAN and Naxclow camera app",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7252_Naxclow_MiniCam_A11-A9_B-V2.7_240426.bin"),
        "args": ["--only-uart", "-chip", "BK7252"],
        "timeout": 60,
        "expected_strings": [
            "Thread Operating System",
            "3.1.0 build Aug  7 2024",
            "test_flash_read_for_mac",
            "tssi_th:b-130, g-123",
            "NO TXID_LPFCAP found in flash, use def 61, 61",
            "register station wlan device sucess!",
            "register soft-ap wlan device sucess!",
            "beken wlan hw init",
            "drivers  set dac vol:65 - indx:11,dig:30,ana:1a",
            "set adc vol: 80 - 80",
            "drv_pm_init",
            "com.naxclow.Camera_v0-1.1.20",
            "DevType (A9_V2.7_ALERT_A_48P",
            "msh />",
        ],
        "expected_pins": {
            0: 0x78, 1: 0x78, 9: 0x00, 10: 0x78, 11: 0x78,
        },
    },
    {
        # Plaintext BK7252 (QFN68) dump from a 2021-11-24 A9 camera running
        # Beken SDK 3.0.33 (OSK R-3.0.22, IP W4-3.0.33-P0, RT-Thread 3.1.0
        # built May 14 2022) and logging on UART1 instead of UART2.  Calibrates
        # RF (tssi b-125, g-115, xtal_cali:25), completes func_init_extended,
        # initializes lwIP 2.0.2, registers station and soft-AP WLAN devices,
        # and reaches the msh /> shell prompt.
        "name": "BK7252 A9 camera 20211124 (QFN68, SDK 3.0.33, UART1 log) calibrates RF and initializes WLAN",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7252_A9_Camera_20211124_SDK_3.0.33.bin"),
        "args": ["--only-uart", "-chip", "BK7252"],
        "timeout": 60,
        "expected_strings": [
            "Thread Operating System",
            "3.1.0 build May 14 2022",
            "OSK Rev: R-3.0.22",
            "SDK Rev: 3.0.33",
            "IP Rev: W4-3.0.33-P0",
            "rfcali_mode:0",
            "tssi:b-125, g-115",
            "xtal_cali:25",
            "[FUNC]func_init_extended OVER!!!",
            "start_type:0",
            "lwIP-2.0.2 initialized!",
            ("igmp_mac_filter add 224.0.0.1 01:00:5E:00:00:01", 2),
            "beken wlan hw init",
            "drv_pm_init",
            "Partition table NOT found on flash: beken_onchip_crc (len: 24) from offset: 0x0000f000.",
            "RT-Thread Flash Abstraction Layer (V0.4.0) initialize failed.",
            "msh />",
        ],
        "expected_pins": {
            0: 0x78, 1: 0x78, 10: 0x78, 11: 0x78,
        },
    },
    {
        # Plaintext BK7252 (QFN68) dump from a Naxclow A9 camera (RT-Thread
        # 3.1.0 built May 9 2023, com.naxclow.Camera_v0-1.1.20).  Contrasts
        # with the Aug 2024 QFN48 MiniCam build: logs 'No TLV header found in
        # flash', calibrates RF with tssi_th b-120/g-115, starts its watchdog
        # device, initializes tc_entity_init, and reads device ID 0800C0140548
        # from flash via sys_test_flash_read.
        "name": "BK7252 Naxclow A9 QFN68 camera (20230509, 1.1.20) boots audio, WDT and reads device ID 0800C0140548",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7252_Naxclow_A9_Camera_Redacted_1.1.20_20230509.bin"),
        "args": ["--only-uart", "-chip", "BK7252"],
        "timeout": 60,
        "expected_strings": [
            "Thread Operating System",
            "3.1.0 build May  9 2023",
            "test_flash_read_for_mac",
            "No TLV header found in flash",
            "tssi_th:b-120, g-115",
            "[sys_wdtDeviceInitStart - 38]-Debue: start wdt device",
            "register station wlan device sucess!",
            "register soft-ap wlan device sucess!",
            "beken wlan hw init",
            "drivers  set dac vol:65 - indx:11,dig:30,ana:1a",
            "set adc vol: 80 - 80",
            "drv_pm_init",
            "com.naxclow.Camera_v0-1.1.20",
            "tc_entity_init",
            "sign_netConfSignInit_n = (NULL)",
            "[0]202305091627",
            "sys_test_flash_read [0800C0140548]",
            "sign_readFlashDEVICE_id state 0",
            "[1]0800C0140548",
        ],
        "expected_pins": {
            0: 0x78, 1: 0x78, 9: 0x00, 10: 0x78, 11: 0x78,
        },
    },
    {
        # Plaintext BK7252 (QFN48) dump from a T99 MiniCam with GC0311 sensor
        # (CYCAM SDK).  Release build suppresses UART console output via
        # rt_console_set_device("no_uart"), probes SDIO on P14..P19 via the
        # SDCard controller (0x00802d00..0x00802d34), checks SCTRL_USB_PLUG_WAKEUP
        # (0x00800084 bit 21) and SARADC temperature/battery, and brings up the
        # 'EEE-196629-TLESS' Soft-AP at 192.168.169.100, handing out
        # 192.168.169.101 over DHCP and answering ARP + ICMP echo.
        "name": "BK7252 T99 MiniCam (QFN48, GC0311) probes SDIO and hosts EEE-196629-TLESS AP",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7252_T99_MiniCam_GC0311.bin"),
        "args": ["--only-uart", "-chip", "BK7252", "--wifi-peer", "sta"],
        "timeout": 60,
        "expected_strings": [
            "[WIFI] device RX filter 0x7fffffde: monitor mode, every frame on air accepted (sniffing, as in EZ pairing)",
            "[WIFI] device RX filter 0x3503848c: back to normal reception",
            "[WIFI] found BSS 'EEE-196629-TLESS' 00:00:00:12:56:77 ch 11 (open)",
            "[WIFI] authenticated with 00:00:00:12:56:77",
            "[WIFI] associated with 'EEE-196629-TLESS' (AID 1)",
            "[NET] DHCP: we got 192.168.169.101/255.255.255.0 from the device (router 192.168.169.100)",
            "[NET] ARP: device 192.168.169.100 is at 00:00:00:12:56:77",
            "[NET] ping reply from 192.168.169.100 (seq 1)",
        ],
        # P5 (config 0x00) is not asserted: with the real 1 kHz timer tick the app
        # drives it ~0.07 s of device time AFTER the last marker (ping reply), and the
        # pins are frozen at the first boundary once every marker is in.
        "expected_pins": {
            0: 0x78, 1: 0x78, 4: 0x3C, 7: 0x3C,
            10: 0x78, 11: 0x78, 12: 0x3C,
            14: 0x78, 15: 0x78, 16: 0x78, 17: 0x78, 18: 0x78, 19: 0x78,
        },
    },
    {
        # Plaintext BK7252 (QFN68) dump from an A10 CYCAM camera (LV-MW102P-V1.3
        # board, VM101P_v85_0729).  Probes SDIO on P14..P19, checks USB plug
        # status (0x00800084 bit 21) and SARADC, drives P24 low (0x00), and
        # brings up the 'PIXA-077260-DNGUV' Soft-AP (90:91:64:08:54:80) at
        # 192.168.169.100, handing out 192.168.169.101 over DHCP and answering
        # ARP + ICMP echo.
        "name": "BK7252 A10 CYCAM (QFN68, VM101P_v85_0729) probes SDIO and hosts PIXA-077260-DNGUV AP",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7252_A10_CYCAM_VM101P_v85_0729.bin"),
        "args": ["--only-uart", "-chip", "BK7252", "--wifi-peer", "sta"],
        "timeout": 60,
        "expected_strings": [
            "[WIFI] device RX filter 0x7fffffde: monitor mode, every frame on air accepted (sniffing, as in EZ pairing)",
            "[WIFI] device RX filter 0x3503848c: back to normal reception",
            "[WIFI] found BSS 'PIXA-077260-DNGUV' 90:91:64:08:54:80 ch 11 (open)",
            "[WIFI] authenticated with 90:91:64:08:54:80",
            "[WIFI] associated with 'PIXA-077260-DNGUV' (AID 1)",
            "[NET] DHCP: we got 192.168.169.101/255.255.255.0 from the device (router 192.168.169.100)",
            "[NET] ARP: device 192.168.169.100 is at 90:91:64:08:54:80",
            "[NET] ping reply from 192.168.169.100 (seq 1)",
        ],
        "expected_pins": {
            0: 0x78, 1: 0x78, 4: 0x3C, 7: 0x3C,
            10: 0x78, 11: 0x78, 12: 0x3C,
            14: 0x78, 15: 0x78, 16: 0x78, 17: 0x78, 18: 0x78, 19: 0x78,
        },
    },
    {
        # Plaintext BK7252 (QFN48) dump from a Pix-Link A9 camera
        # (CYCAM_RW101P_v19_1010, PIXA-180431-XLPNL).  Probes SDIO on P14..P19,
        # configures P7 as pull-up input (0x3C), and brings up the
        # 'PIXA-180431-XLPNL' Soft-AP (90:91:64:0b:56:77) at 192.168.169.100,
        # handing out 192.168.169.101 over DHCP and answering ARP + ICMP echo.
        "name": "BK7252 Pix-Link A9 camera (QFN48, RW101P_v19_1010) probes SDIO and hosts PIXA-180431-XLPNL AP",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7252_A9_Pix-Link_CYCAM_RW101P_v19_1010.bin"),
        "args": ["--only-uart", "-chip", "BK7252", "--wifi-peer", "sta"],
        "timeout": 60,
        "expected_strings": [
            "[WIFI] device RX filter 0x7fffffde: monitor mode, every frame on air accepted (sniffing, as in EZ pairing)",
            "[WIFI] device RX filter 0x3503848c: back to normal reception",
            "[WIFI] found BSS 'PIXA-180431-XLPNL' 90:91:64:0b:56:77 ch 11 (open)",
            "[WIFI] authenticated with 90:91:64:0b:56:77",
            "[WIFI] associated with 'PIXA-180431-XLPNL' (AID 1)",
            "[NET] DHCP: we got 192.168.169.101/255.255.255.0 from the device (router 192.168.169.100)",
            "[NET] ARP: device 192.168.169.100 is at 90:91:64:0b:56:77",
            "[NET] ping reply from 192.168.169.100 (seq 1)",
        ],
        "expected_pins": {
            0: 0x78, 1: 0x78, 7: 0x3C,
            14: 0x78, 15: 0x78, 16: 0x78, 17: 0x78, 18: 0x78, 19: 0x78,
        },
    },
    {
        # Plaintext BL2028N (PAI-053 module) dump from a Dreo WH517S wall
        # heater running firmware 3.4.3 (Oct 23 2024, SDK 3.0.46).  Boots the
        # RW BLE 5.x stack (gapm_cmp_evt:BLE_STACK_OK), prints its DREOsh17
        # banner and license SN, sends 55 AA MCU heartbeat/query frames on
        # UART1, and starts scanning + temperature/XTAL calibration.
        "name": "BL2028N Dreo WH517S wall heater (PAI-053, 3.4.3) boots BLE stack and polls MCU",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BL2028N_Dreo_Wall_Heater_WH517S_PAI-053_3.4.3.bin"),
        "args": ["--only-uart", "--ble-core", "--uart1-hex"],
        "timeout": 60,
        "tags": ["BLE", "BLE core", "FIQ", "TuyaMCU"],
        "expected_strings": [
            "SDK Rev: 3.0.46_20220921_d9dce354c70f",
            "chip id=7231a device id=18520001",
            "calibration_main over",
            "rwble_hl_init ok",
            "BLE Rev: B5-3.0.46-P0",
            "gapm_cmp_evt:BLE_STACK_OK",
            "firware ver:3.4.3",
            "Date: Oct 23 2024 14:47:15",
            "MAC address: 00:1c:c2:a4:d9:1c",
            "ble_name: DREOsh17",
            "info checkout: success",
            "license.sn:1769616359658942465-fda17af2af19fd07:001:0000000000b",
            "[UART1/MCU] 55 aa 00 00 00 00 00 00 ff 55 aa 00 01 01 00 00 00 01",
            "[UART1/MCU] 55 aa 00 02 03 00 00 02 01 00 07",
            "wpa S: DISCONNECTED -> SCANNING",
        ],
        "expected_pins": {
            0: 0x78, 1: 0x78, 10: 0x78, 11: 0x78,
        },
    },
    {
        # Newer firmware revision (3.8.7, Oct 13 2025) from the same Dreo
        # WH517S BL2028N PAI-053 wall heater family (MAC 00:1c:c2:a6:a8:07,
        # XTAL=86).  Boots the RW BLE 5.x stack, prints its updated DREOsh17
        # banner and license SN, exchanges 55 AA frames on UART1, and starts
        # Wi-Fi station scanning.
        "name": "BL2028N Dreo WH517S wall heater (PAI-053, 3.8.7) boots BLE stack and polls MCU",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BL2028N_Dreo_Wall_Heater_WH517S_PAI-053_3.8.7.bin"),
        "args": ["--only-uart", "--ble-core", "--uart1-hex"],
        "timeout": 60,
        "tags": ["BLE", "BLE core", "FIQ", "TuyaMCU"],
        "expected_strings": [
            "SDK Rev: 3.0.46_20220921_d9dce354c70f",
            "chip id=7231a device id=18520001",
            "calibration_main over",
            "rwble_hl_init ok",
            "BLE Rev: B5-3.0.46-P0",
            "gapm_cmp_evt:BLE_STACK_OK",
            "firware ver:3.8.7",
            "Date: Oct 13 2025 10:46:14",
            "MAC address: 00:1c:c2:a6:a8:07",
            "ble_name: DREOsh17",
            "info checkout: success",
            "license.sn:1769616359658942465-010f809255100431:001:0000000000b",
            "[UART1/MCU] 55 aa 00 00 00 00 00 00 ff 55 aa 00 01 01 00 00 00 01",
            "[UART1/MCU] 55 aa 00 02 03 00 00 02 01 00 07",
            "wpa S: DISCONNECTED -> SCANNING",
        ],
        "expected_pins": {
            0: 0x78, 1: 0x78, 10: 0x78, 11: 0x78,
        },
    },
    {
        # Plaintext BL2028N (BK7231N identity, SDK 3.0.65 / AliOS Things
        # AOS-R-1.3.4 / LinkKit 2.3.0_FY_1.6.0) dump from a XiaoMei WP8621N
        # automatic fish feeder (WIFI002_V20 board, APP: WP8621NK_03).
        # Exercises the _patch_ldm_caret_stubs normalization of AliOS Things's
        # `ldmfd sp!, {r0, r1}^` (0xe8fd0003) in boot_handlers.S, brings up the
        # RW BLE 5.x stack (gapm_cmp_evt:BLE_STACK_OK), initializes LinkKit
        # (product_key a1c3mFpE63X), and enters Soft-AP / monitor pairing mode.
        "name": "BL2028N XiaoMei WP8621N fish feeder (AliOS Things, WP8621NK_03) boots BLE and LinkKit",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BL2028N_XiaoMei_WP8621N_Fish_Feeder_WP8621NK_03.bin"),
        "args": ["--only-uart", "-chip", "BK7231N"],
        "timeout": 60,
        "tags": ["BLE", "BLE core", "FIQ"],
        "expected_strings": [
            "OSK Rev: A-3.0.30",
            "SDK Rev: 3.0.65",
            "chip id=7231c device id=20521023",
            "calibration_main over",
            "rwble_hl_init ok",
            "BLE Rev: B5-3.0.65-P0",
            "gapm_cmp_evt:BLE_STACK_OK",
            "-Welcome to AliOS Things-",
            "Kernel: AOS-R-1.3.4",
            "LinkKit: 2.3.0_FY_1.6.0",
            "APP: WP8621NK_03",
            "VSON product_key[a1c3mFpE63X]",
            "product_secret[HZQ5AC8ME8Vv5klX]",
            "device_name[86:21:02:FC:22:41]",
            "ota init success, ver:WP8621NK_03 type:0",
            "scan factory ap, set led ON",
            "struct_calibration.calibration = 1365",
            "verson = WP8621NK_03",
        ],
        "expected_pins": {
            0: 0x00, 1: 0x00, 7: 0x3C, 9: 0x3C,
            10: 0x78, 11: 0x78, 15: 0x00, 17: 0x00,
            22: 0x08, 23: 0x00, 28: 0x3C,
        },
    },
    {
        # Plaintext BK7238 QIO dump from a Gledopto GL-SPI-206W addressable SPI
        # LED strip controller running TuyaOS 3.11.11 (built Mar 5 2025 for T1).
        # Runs tuya_upgrade_main [255], configures SPI pads P14..P17 (0x348 /
        # 0x34A) for the addressable strip output, loads MAC 80:64:7c:99:dd:7f
        # and attempts fast-connect to stored BSSID 28:87:ba:a0:f5:6d
        # (DLINK_FastNet).
        "name": "BK7238 Gledopto GL-SPI-206W SPI LED controller (TuyaOS 3.11.11) configures SPI pads and fast-connects",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7238_Gledopto_GL-SPI-206W_LED_Controller.bin"),
        "args": ["--only-uart", "-chip", "BK7238",
                 "--wifi-peer", "ap", "--wifi-ssid", "DLINK_FastNet",
                 "--wifi-pass", "F4STN3T@W*f*PL1234567890"],
        "timeout": 90,
        "expected_strings": [
            "**********tuya_upgrade_main need upgrade? [255]**********",
            "device_id=0x21128000",
            "< TuyaOS V:3.11.11 BS:40.00_PT:2.3_LAN:3.5_CAD:1.0.5_CD:1.0.0 >",
            "< BUILD AT:2025_03_05_20_01_36 BY ci_manage FOR tuyaos-iot AT T1 >",
            "t1_common_user_config_ty sw ver:3.0.22",
            "calibration_main over",
            "kv has ccode PL",
            "mac 80:64:7c:99:dd:7f",
            "bssid 28:87:ba:a0:f5:6d, cap_info 0x1811, BI 100, ssid DLINK_FastNet",
            "writed fci to flash ssid=DLINK_FastNet",
            "[NET] DHCP: device 80:64:7c:99:dd:7f got 192.168.1.100 (gateway/DNS 192.168.1.1)",
            "handshake authenticated; serving encrypted MQTT offline",
            "[NET] MQTT SUBSCRIBE smart/device/in/bf70c4ef9a3806556brng4",
        ],
        "expected_pins": {
            0: 0x78, 1: 0x78, 10: 0x78, 11: 0x78,
            14: 0x348, 15: 0x34A, 16: 0x348, 17: 0x348,
        },
    },
    {
        # Plaintext BK7238 dump from a BrizLabs Fairy RGB LED strip
        # (sxebObqQRfXmgU, firmware 1.0.1 on Beken SDK 3.0.65 / OSK F-3.0.40).
        # Completes RF calibration (lpf_i/q 74/76, xtal 23), configures SPI
        # pads P14..P17 (0x48), button P6 (0x3C), outputs P22 (0x00) and P28
        # (0x08), loads factory test SSID 'tuyatest' (MAC 44:4a:d6:0c:9e:14)
        # and computes its PSKC before scanning.
        "name": "BK7238 BrizLabs Fairy LED RGB (SDK 3.0.65, 1.0.1) configures SPI/GPIO and scans for tuyatest",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7238_BrizLabs_Fairy_LED_RGB_1.0.1.bin"),
        "args": ["--only-uart", "-chip", "BK7238"],
        "timeout": 60,
        "tags": ["BLE", "BLE core", "FIQ"],
        "expected_strings": [
            "OSK Rev: F-3.0.40",
            "SDK Rev: 3.0.65",
            "chip id=7238 device id=21128000",
            "calibration_main over",
            "lpf_i & q in flash is:74, 76",
            "xtal in flash is:23",
            "gapm_cmp_evt:BLE_STACK_OK",
            "BLE_5_CREATE_DB",
            "ssid:tuyatest key:fakepassword",
            "mac 44:4a:d6: c:9e:14",
            "PSKC: ssid tuyatest, passphrase fakepassword",
            "wpa S: DISCONNECTED -> SCANNING",
            "[BLE_ADV]   16-bit UUIDs (incomplete)  01 a1                                   ..  uuid=0xA101",
            "sxebObqQRfXmgU  uuid=0xA101",
        ],
        "expected_pins": {
            0: 0x78, 1: 0x78, 6: 0x3C, 10: 0x78, 11: 0x78,
            14: 0x48, 15: 0x48, 16: 0x48, 17: 0x48,
            22: 0x00, 28: 0x08,
        },
    },
    {
        # Plaintext BK7238 dump from a DX-Smart DX-WF24A AT-command Wi-Fi + BLE
        # combo module.  Suppresses Beken SDK boot chatter, prints 'power on' on
        # UART, configures P6 and P26 low (0x00), brings up the BLE 5.2
        # controller and stages its 'WF24-BLE' Complete Name advertisement in
        # exchange memory at 0x0040e480 +0x0a54.
        "name": "BK7238 DX-Smart DX-WF24A AT module prints 'power on' and advertises 'WF24-BLE'",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7238_DX-Smart_DX-WF24A_AT.bin"),
        "args": ["--only-uart", "-chip", "BK7238"],
        "timeout": 60,
        "tags": ["BLE", "BLE core", "FIQ"],
        "expected_strings": [
            "power on",
            "[BLE_ADV] EM 0x0040e480 +0x0a54 (13 bytes): 02 01 06 09 09 57 46 32 34 2d 42 4c 45",
            "[BLE_ADV]   Complete Name",
            "WF24-BLE",
        ],
        "expected_pins": {
            0: 0x78, 1: 0x78, 6: 0x00, 10: 0x78, 11: 0x78, 26: 0x00,
        },
    },
    {
        # Plaintext BK7238 dump from a Zengge AK001-ZJ21412 RGBCCT bulb
        # (35_243_20240425_ZG-BK38-BP101 on Beken SDK 3.0.70 c063598, OS 8.43.0)
        # driving a BP5758D I2C 5-channel current-regulator LED driver on P24
        # and P26 (both driven high 0x02 at idle).  Prints its Zengge banner,
        # BP5758D current settings (8/8/8/30/30 mA), increments PWRRCD store
        # index 23, completes RF calibration, and advertises IOTWF46D over BLE 5.2.
        "name": "BK7238 Zengge ZJ21412 BP101 RGBCCT bulb (BP5758D I2C, 20240425) boots OS 8.43.0 and calibrates",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7238_Zengge_ZJ21412_BP101_RGBCCT_Bulb.bin"),
        "args": ["--only-uart", "-chip", "BK7238"],
        "timeout": 60,
        "tags": ["BLE", "BLE core", "FIQ"],
        "expected_strings": [
            "SDK Rev: 3.0.70 c063598",
            "chip id=7238 device id=21128000",
            "bulid time 20240425",
            "light version 4.16.0(2)",
            "|MID-> AK001-ZJ21412",
            "--------BP5758D rgbwc_ele 8 8 8 30 30",
            "|OS->8.43.0(4)",
            "|FW->35_243_20240425_ZG-BK38-BP101",
            "I(APP:PWRRCD) store index 23",
            "calibration_main over",
            "I(PORT) adv_name_get IOTWF46D",
            "[BLE_ADV]   Service Data               00 5a 4c 07 00 00 00 00 00 00 00 35 ..  .ZL........5....  uuid=0x5A00",
        ],
        "expected_pins": {
            0: 0x78, 1: 0x78, 10: 0x78, 11: 0x78, 24: 0x02, 26: 0x02,
        },
    },
    {
        # Plaintext BK7238 dump from a Zengge AK001-ZJ21413 Symphony USB SPI
        # addressable LED controller (77_24_20240423_ZG-BK on Beken SDK 3.0.70
        # ecb1fb4, Zengge OS 8.42.4).  Configures SPI pads P14..P17 (0x48) and
        # inputs P6/P8 (0x3C), brings up the BLE 5.2 controller, stages its
        # 'IOTWF8A9' Complete Name advertisement in exchange memory, and starts
        # the BRMT remote task.
        "name": "BK7238 Zengge ZJ21413 Symphony USB SPI LED (OS 8.42.4, 20240423) configures SPI and advertises IOTWF8A9",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7238_Zengge_ZJ21413_Symphony_USB_SPI_LED.bin"),
        "args": ["--only-uart", "-chip", "BK7238"],
        "timeout": 60,
        "tags": ["BLE", "BLE core", "FIQ"],
        "expected_strings": [
            "SDK Rev: 3.0.70 ecb1fb4",
            "chip id=7238 device id=21128000",
            "bulid time 20240423",
            "light version 1.1.2(2)",
            "|MID-> AK001-ZJ21413",
            "|OS->8.42.4(1)",
            "|FW->77_24_20240423_ZG-BK",
            "calibration_main over",
            "I(PORT) adv_name_get IOTWF8A9",
            "[BLE_ADV]   Complete Name",
            "IOTWF8A9",
            "D(BRMT) (2408) task init success",
        ],
        "expected_pins": {
            0: 0x78, 1: 0x08, 6: 0x3C, 8: 0x3C,
            10: 0x78, 11: 0x78, 14: 0x48, 15: 0x48, 16: 0x48, 17: 0x48,
        },
    },
    {
        # Plaintext BK7238 dump from a Zengge W43-M5 V1.2 ceiling light
        # (AK001-ZJ21412, 3D_23_20250724_ZIL-BK38-M5-12 on Beken SDK 3.0.70
        # 17e8b67, Zengge OS 9.7.1).  Configures PWM pads P6/P8 (0x48) and SPI
        # pads P14..P17 (0x48), brings up BLE 5.2 advertising ('IOTWF615'), and
        # checks the FOTA version record.
        "name": "BK7238 Zengge W43-M5 V1.2 ceiling light (OS 9.7.1, 20250724) configures PWM/SPI and advertises IOTWF615",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7238_Zengge_Ceiling_Light_W43-M5_V1.2.bin"),
        "args": ["--only-uart", "-chip", "BK7238"],
        "timeout": 60,
        "tags": ["BLE", "BLE core", "FIQ"],
        "expected_strings": [
            "SDK Rev: 3.0.70 17e8b67",
            "chip id=7238 device id=21128000",
            "bulid time 20250724",
            "light version 0.1.2(8)",
            "|MID-> AK001-ZJ21412",
            "|sdk version -> 2.10.1(6)",
            "|OS->9.7.1(3)",
            "|FW->3D_23_20250724_ZIL-BK38-M5-12",
            "calibration_main over",
            "I(PORT) adv_name_get IOTWF615",
            "[BLE_ADV]   Complete Name",
            "IOTWF615",
            "I(FOTA) read dev version (old :  ) ( now : 3D_23_20250724_ZIL-BK38-M5-12 ) (target :  )",
        ],
        "expected_pins": {
            0: 0x78, 1: 0x78, 6: 0x48, 8: 0x48,
            10: 0x78, 11: 0x78, 14: 0x48, 15: 0x48, 16: 0x48, 17: 0x48,
        },
    },
    {
        # Plaintext BK7238 dump from an eWeLink (CoolKit) door/window sensor
        # (CK-BK7238-W105SE0X-01-DOOR-HB, version 1.0.4 built 20250516 on Beken
        # SDK 3.0.70 / OSK F-3.0.44).  Configures reed-switch/button inputs P7,
        # P22, P23 as pull-up inputs (0x3C) and LED P21 low (0x00), prints its
        # CoolKit banner and DeviceMac d02704434298, completes RF calibration,
        # and starts station scanning.
        "name": "BK7238 eWeLink W105SE0X door/window sensor (1.0.4, SDK 3.0.70) configures reed/button GPIOs and scans",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7238_eWeLink_DoorWindow_W105SE0X_1.0.4.bin"),
        "args": ["--only-uart", "-chip", "BK7238"],
        "timeout": 60,
        "expected_strings": [
            "OSK Rev: F-3.0.44",
            "SDK Rev: 3.0.70",
            "***CK-BK7238-W105SE0X-01-DOOR-HB---Ver:1.0.4,Btim:20250516.09531208.015***",
            "chip id=7238 device id=21128000",
            "sta_mac:d0:27:04:43:42:98",
            "DeviceMac:d02704434298",
            "calibration_main over",
            "wpa S: DISCONNECTED -> SCANNING",
        ],
        "expected_pins": {
            0: 0x78, 1: 0x78, 7: 0x3C, 10: 0x78, 11: 0x78,
            21: 0x00, 22: 0x3C, 23: 0x3C,
        },
    },
    {
        # Plaintext BK7238 dump from a Zengge Ledytech XGD-NZ-002 ceiling light
        # on the WB43-M5 V1.1 board (AK001-ZJ21412, 3D_22_20250107_ZIL-BK38 on
        # Beken SDK 3.0.70 434b394, Zengge SDK 2.9.0 / OS 9.7.1).  Configures
        # PWM pads P6/P8 (0x48) and SPI pads P14..P17 (0x48), brings up BLE 5.2
        # advertising ('IOTWF829'), and initializes the BRMT remote task.
        "name": "BK7238 Zengge Ledytech XGD-NZ-002 WB43-M5 V1.1 (OS 9.7.1, 20250107) configures PWM/SPI and advertises IOTWF829",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7238_Zengge_Ledytech_XGD-NZ-002_WB43-M5_V1.1.bin"),
        "args": ["--only-uart", "-chip", "BK7238"],
        "timeout": 60,
        "tags": ["BLE", "BLE core", "FIQ"],
        "expected_strings": [
            "SDK Rev: 3.0.70 434b394",
            "chip id=7238 device id=21128000",
            "bulid time 20250107",
            "light version 0.1.2(8)",
            "|MID-> AK001-ZJ21412",
            "|sdk version -> 2.9.0(1)",
            "|OS->9.7.1(3)",
            "|FW->3D_22_20250107_ZIL-BK38",
            "calibration_main over",
            "I(PORT) adv_name_get IOTWF829",
            "[BLE_ADV]   Complete Name",
            "IOTWF829",
            "D(BRMT) (2588) task init success",
        ],
        "expected_pins": {
            0: 0x78, 1: 0x78, 6: 0x48, 8: 0x48,
            10: 0x78, 11: 0x78, 14: 0x48, 15: 0x48, 16: 0x48, 17: 0x48,
        },
    },
    {
        # Stock BK7231T dump from an Arlec / Tuya SSWF01G-WIFI ceiling fan
        # speed controller (oem_bk7231s_fan_control_arlec:1.0.5 on TUYA IOT SDK
        # 2.0.0 BS:30.06).  Configures its multi-relay fan speed outputs
        # (P6/P7/P9/P10 high 0x02, P26 low 0x00) and button inputs (P1/P8/P14
        # pull-up 0x3C), updates its bound BLE state and localkey, and brings
        # up GAPM.
        "name": "BK7231T Tuya Arlec SSWF01G fan speed controller (1.0.5, SDK 2.0.0) configures fan relays and BLE",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7231T_Tuya_Fan_Speed_Controller_SSWF01G_1.0.5.bin"),
        "args": ["--only-uart", "-key", "TUYA",
                 "--wifi-peer", "ap", "--wifi-ssid", "HomeNET", "--wifi-pass", "746025625"],
        "timeout": 90,
        "expected_strings": [
            "< TUYA IOT SDK V:2.0.0 BS:30.06_PT:2.2_LAN:3.3_CAD:1.0.2_CD:1.0.0 >",
            "oem_bk7231s_fan_control_arlec:1.0.5",
            "**********[oem_bk7231s_fan_control_arlec] [1.0.5] compiled at Jul  7 2020 11:27:29**********",
            "update bound state 1",
            "device_init ok  free_mem_size:81808",
            "ty bt update localkey",
            "55 bc ce cb 9b 56 ac 21 ba 29 28 8a 6e 47 2c f0",
            "ble adv && resp changed",
            "gapm_cmp_evt_handler operation = 0x1, status = 0x0",
            "ssid:HomeNET, 1",
            "[NET] DHCP: device 68:57:2d:f9:7c:48 got 192.168.1.100 (gateway/DNS 192.168.1.1)",
            "[NET] DNS m2.tuyaeu.com -> 10.200.0.1 (offline sinkhole)",
            "TLS_PSK_WITH_AES_128_CBC_SHA256",
        ],
        "expected_pins": {
            0: 0x78, 1: 0x3C, 6: 0x02, 7: 0x02, 8: 0x3C, 9: 0x02,
            10: 0x02, 11: 0x78, 12: 0x78, 13: 0x78, 14: 0x3C, 26: 0x00,
        },
    },
    {
        # Stock BK7231T dump from a Mirabella Genio Nebular Star Projector
        # (I003323, oem_bk7231t_light3_laser_nanxin:1.1.2 on TUYA IOT SDK 1.0.2).
        # Drives 5 hardware PWM channels at 1000 Hz (0x6590 = 26000 ticks:
        # P6/P8/P9/P24/P26 = PWM0/2/3/4/5) for the nebula LEDs + motor, plus
        # P7 low (0x00) and button P14 pull-up (0x3C), and brings up BLE GATT
        # advertising (STACK INIT OK, CREATE DB SUCCESS, appm start advertising).
        "name": "BK7231T Tuya Genio Nebular Projector I003323 (5ch 1000 Hz PWM + laser, 1.1.2) boots",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7231T_Tuya_Genio_Nebular_Projector_I003323_1.1.2.bin"),
        "args": ["--only-uart", "-key", "TUYA"],
        "timeout": 60,
        "expected_strings": [
            "goto first bright up!",
            "**********[oem_bk7231t_light3_laser_nanxin] [1.1.2] compiled at Jan 27 2021 10:29:48**********",
            "< TUYA IOT SDK V:1.0.2 BS:40.00_PT:2.2_LAN:3.3_CAD:1.0.2_CD:1.0.0 >",
            "oem_bk7231t_light3_laser_nanxin:1.1.2",
            "have actived over 15 min, not enter mf_init",
            "mf_init succ",
            "button init",
            "device config data already load! Don't load again!!",
            "Drive init already init ok",
            "STACK INIT OK",
            "CREATE DB SUCCESS",
            "ble adv && resp changed",
            "appm start advertising",
        ],
        "expected_pins": {
            0: 0x78, 1: 0x78, 6: 0x48, 7: 0x00, 8: 0x48, 9: 0x48,
            10: 0x78, 11: 0x78, 12: 0x78, 13: 0x78, 14: 0x3C,
            24: 0x48, 26: 0x48,
        },
        "expected_pwm": {
            0: (0x6590, 0x32C8),
            2: (0x6590, 0x6590),
            3: (0x6590, 0x6590),
            4: (0x6590, 0x6590),
            5: (0x6590, None),
        },
    },
    {
        # Stock BK7231T dump from a MOES MS-102 smart garage door controller
        # (oem_bk7231s_garage_door:1.1.2 on TUYA IOT SDK 1.0.2, WB2S module).
        # Configures door-contact reed input P26 (pull-down 0x2C), button P6
        # (pull-up 0x3C), relay/LED outputs P7 (0x02) and P24 (0x00), updates
        # its BLE advertisement, and reconnects to its pre-backup WPA2 network
        # (FASTWEB-casaFW), obtaining a DHCP lease and emitting Tuya LAN 3.3
        # discovery broadcasts.
        "name": "BK7231T Tuya MOES MS-102 garage door opener (WB2S, 1.1.2) configures contact/relay GPIOs and BLE",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7231T_Tuya_MOES_GarageDoor_MS-102_WB2S_1.1.2.bin"),
        "args": ["--only-uart", "-key", "TUYA",
                 "--wifi-peer", "ap", "--wifi-ssid", "FASTWEB-casaFW",
                 "--wifi-pass", "Giuseppe2707&Marta0909"],
        "timeout": 90,
        "expected_strings": [
            "< TUYA IOT SDK V:1.0.2 BS:40.00_PT:2.2_LAN:3.3_CAD:1.0.2_CD:1.0.0 >",
            "oem_bk7231s_garage_door:1.1.2",
            "**********[oem_bk7231s_garage_door] [1.1.2] compiled at Nov 23 2020 20:07:02**********",
            "device_ctrl_init start!",
            "device_ctrl_init end!",
            "device_init ok  free_mem_size:74552",
            "ty bt update localkey",
            "ble adv && resp changed",
            "ssid:FASTWEB-casaFW, 1",
            "[NET] DHCP: device b8:06:0d:ca:44:35 got 192.168.1.100 (gateway/DNS 192.168.1.1)",
            "[NET] DNS m2.tuyaeu.com -> 10.200.0.1 (offline sinkhole)",
            '"gwId":"bf5758e46fc5d63663qc5f","active":2,"ablilty":0,"encrypt":true,"productKey":"keymxkctp9ukgcvg"',
        ],
        "expected_pins": {
            0: 0x78, 1: 0x78, 6: 0x3C, 7: 0x02,
            10: 0x78, 11: 0x78, 12: 0x78, 13: 0x78,
            24: 0x00, 26: 0x2C,
        },
    },
    {
        # Stock BK7231T dump from a Tuya qcsc400w-v5t motorized smart curtain
        # switch (oem_bk7231s_curtain_switch:1.6.0 on TUYA IOT SDK 1.0.2, WB3S
        # module).  Recovers its protected user_param_key, parses and prints
        # its 3-sub-channel curtain relay/button table (rly P7/P6, bt P8/P10/P24,
        # intertime 200 ms, drvtime 200 ms), completes curtain_proc_dev_init,
        # and brings up BLE GATT advertising (STACK INIT OK, CREATE DB SUCCESS,
        # appm start advertising).
        "name": "BK7231T Tuya qcsc400w-v5t smart curtain switch (WB3S, 1.6.0) loads 3-sub-channel curtain config",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7231T_Tuya_Smart_Curtain_qcsc400w-v5t_WB3S_1.6.0.bin"),
        "args": ["--only-uart", "-key", "TUYA"],
        "timeout": 60,
        "expected_strings": [
            "< TUYA IOT SDK V:1.0.2 BS:40.00_PT:2.2_LAN:3.3_CAD:1.0.2_CD:1.0.0 >",
            "oem_bk7231s_curtain_switch:1.6.0",
            "**********[oem_bk7231s_curtain_switch] [1.6.0] compiled at May 31 2021 16:53:20**********",
            "CH[0]:dpid,[1-2-3-10-8-12-{dryEn(true)-intertime200(ms)-drvtime:200(ms)}]",
            "Sub ch[0]:led[255-  2],rly[  7-  1],bt[  8-  0 - longKey-5000(ms)]",
            "Sub ch[1]:led[255-  2],rly[255-  2],bt[ 10-  0 - longKey-5000(ms)]",
            "Sub ch[2]:led[255-  2],rly[  6-  1],bt[ 24-  0 - longKey-5000(ms)]",
            "curtain_json_init init end!",
            "dev_event_json_init init end!",
            "have actived over 15 min, not enter mf_init",
            "STACK INIT OK",
            "CREATE DB SUCCESS",
            "ble adv && resp changed",
            "appm start advertising",
        ],
        "expected_pins": {
            0: 0x78, 1: 0x00, 6: 0x00, 7: 0x00, 8: 0x3C,
            10: 0x3C, 11: 0x78, 12: 0x78, 13: 0x78, 24: 0x3C,
        },
    },
    {
        # Stock BK7231T dump from a Tuya MCB238 / DDSU238 DIN-rail circuit
        # breaker (oem_bk7231s_rnd_switch:1.1.8 on TUYA IOT SDK 1.0.2, WB3S
        # module).  Brings up the BLE GATT database and advertising (STACK INIT
        # OK, CREATE DB SUCCESS, appm start advertising), completes mf_init,
        # and prints its recovered bistable-relay pConfig (rl_on1_pin:9,
        # rl_off1_pin:8, bt1_pin:26, led1_pin:6, netled_pin:24).
        "name": "BK7231T Tuya MCB238 DDSU238 DIN-rail breaker (WB3S, 1.1.8) starts BLE advertising and loads bistable pConfig",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7231T_Tuya_MCB238_Breaker_DDSU238_WB3S_1.1.8.bin"),
        "args": ["--only-uart", "-key", "TUYA"],
        "timeout": 60,
        "expected_strings": [
            "< TUYA IOT SDK V:1.0.2 BS:40.00_PT:2.2_LAN:3.3_CAD:1.0.2_CD:1.0.0 >",
            "oem_bk7231s_rnd_switch:1.1.8",
            "**********[oem_bk7231s_rnd_switch] [1.1.8] compiled at Jul  9 2021 19:30:24**********",
            "STACK INIT OK",
            "CREATE DB SUCCESS",
            "ble adv && resp changed",
            "appm start advertising",
            "mf_init succ",
            "pConfig = {netled_lv:1,bt_type:0,bt1_pin:26,mxcl_led_m:0,led1_pin:6,rand_dpid:43,net_trig:2,rl_on1_pin:9,led_dp:40,jv:1.1.1,lock_dp:41,nety_led:1,rl_off1_pin:8,netled_pin:24,total_stat:0,rl_type:1,bt1_lv:0,",
        ],
        "expected_pins": {
            0: 0x78, 1: 0x78, 10: 0x78, 11: 0x78, 12: 0x78, 13: 0x78,
        },
    },
    {
        # Stock BK7231T dump from a TreatLife SS02 3-way wall switch
        # (oem_bk7231s_double_switch_rmd:1.1.3 on TUYA IOT SDK 1.0.8).
        # Configures traveler/button inputs P6, P26, P28 (pull-up 0x3C) and
        # relay/LED outputs P8, P9 and P24, brings up the BLE GATT database
        # (STACK INIT OK, CREATE DB SUCCESS), starts advertising, and reconnects
        # to its pre-backup WPA2 network (DLINK_FastNet), obtaining a DHCP lease
        # and emitting Tuya LAN 3.3 discovery broadcasts.
        "name": "BK7231T Tuya TreatLife SS02 3-way switch (1.1.3, SDK 1.0.8) configures 3-way GPIOs and BLE",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7231T_Tuya_TreatLife_SS02_3-Way_Switch_1.1.3.bin"),
        "args": ["--only-uart", "-key", "TUYA",
                 "--wifi-peer", "ap", "--wifi-ssid", "DLINK_FastNet",
                 "--wifi-pass", "F4STN3T@W*f*PL1234567890"],
        "timeout": 90,
        "expected_strings": [
            "< TUYA IOT SDK V:1.0.8 BS:40.00_PT:2.2_LAN:3.3_CAD:1.0.2_CD:1.0.0 >",
            "oem_bk7231s_double_switch_rmd:1.1.3",
            "**********[oem_bk7231s_double_switch_rmd] [1.1.3] compiled at May 12 2021 16:12:47**********",
            "STACK INIT OK",
            "CREATE DB SUCCESS",
            "ble adv && resp changed",
            "appm start advertising",
            "[NET] DHCP: device 38:1f:8d:f3:bd:35 got 192.168.1.100 (gateway/DNS 192.168.1.1)",
            '"gwId":"bf9014e5bafa4fa172qfyf","active":2,"ablilty":0,"encrypt":true,"productKey":"keyu5sjrcfhjsvd8"',
        ],
        "expected_pins": {
            0: 0x78, 1: 0x78, 6: 0x3C, 9: 0x00,
            10: 0x78, 11: 0x78, 12: 0x78, 13: 0x78,
            24: 0x00, 26: 0x3C, 28: 0x3C,
        },
    },
    {
        "name": "BL2028N Zemismart Matter ZML2 RGBCT bulb (UAM026, 1.21) prints Matter SetupQRCode and drives pwm_new",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "Zemismart_Matter_ZML2_RGBCT_CSA23291MAT40804-24_UAM026_1.21.bin"),
        "args": ["--only-uart", "-chip", "BK7231N", "--ble-core"],
        "timeout": 60,
        "expected_strings": [
            "SetupQRCode: [MT:--VJ0-IC005WSL02810]",
            "Copy/paste the below URL in a browser to see the QR Code:https://project-chip.github.io/connectedhomeip/qrcode.html?data=MT%3A--VJ",
        ],
    },
    {
        "name": "BK7238 Tuya Arlec PC191HA-S4 plug (T1-2S-NL, 1.0.2) calibrates and fast-connects to stored AP",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "Tuya_Arlec_PC191HA-S4_Plug_(schemaID-e1mtsiag)_keykmm3rjyqy5r8p_74xo1nolwa6xkjeu_T1-2S-NL_1.0.2.bin"),
        "args": ["--only-uart", "-chip", "BK7238", "--ble-core"],
        "timeout": 60,
        "expected_strings": [
            "prvHeapInit-start addr:0x415098, size:175976",
            "**********tuya_upgrade_main need upgrade? [255]**********",
            "< BUILD AT:2025_01_02_15_55_27 BY ci_manage FOR tuyaos-iot AT T1 >",
            "name:oem_t1_plug",
            "tuya_tls_rand_init ok!",
            "calibration_main over",
            "!! tkl_wifi_station_fast_connect",
            "found scan rst rssi -34 > -50",
        ],
    },
    {
        "name": "BK7238 Tuya BSEED M1 metering socket (T1-2S-NL, 1.1.4) boots TuyaOS plug app and scans",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "Tuya_BSEED_M1_Measure_Socket_(schemaID-e1nbktiw)_keykmm3rjyqy5r8p_vl7svb0a4o9gogeb_T1-2S-NL_1.1.4.bin"),
        "args": ["--only-uart", "-chip", "BK7238", "--ble-core"],
        "timeout": 60,
        "expected_strings": [
            "prvHeapInit-start addr:0x415118, size:175848",
            "**********tuya_upgrade_main need upgrade? [255]**********",
            "< BUILD AT:2025_01_02_15_55_27 BY ci_manage FOR tuyaos-iot AT T1 >",
            "name:oem_t1_plug",
            "tuya_tls_rand_init ok!",
            "calibration_main over",
            "mf_init succ",
            "scan_start_req_handler",
        ],
    },
    {
        "name": "BK7238 Tuya QS-WIFI-S10-C04 curtain module (T1-M, 1.0.6) raises SmartLife-BF55 AP and BLE central",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "Tuya_Curtain_QS-WIFI-S10-C04_(schemaID-e1n257is)_keyafhmcfuursnuw_ocz3mqv0v4ntkzva_ocz3mqv0v4ntkzva_T1-M_1.0.6.bin"),
        "args": ["--only-uart", "-chip", "BK7238", "--ble-core"],
        "timeout": 60,
        "expected_strings": [
            "prvHeapInit-start addr:0x414e60, size:176544",
            "sdk_info:< TuyaOS V:3.11.11 BS:40.00_PT:2.3_LAN:3.5_CAD:1.0.5_CD:1.0.0 >",
            "wifi soc init. pid:ocz3mqv0v4ntkzva firmwarekey:NULL ver:1.0.6",
            "serial_no:80647ce3bf55",
            "start ap success:SmartLife-BF55",
            "ty bt sdk init success finish",
            "ble central init success",
            "app_key_task start!!!",
        ],
    },
    {
        "name": "BK7238 Tuya AZIot 4-gang switch (T1-3S, 1.1.9) calibrates and starts Wi-Fi scan",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "Tuya_AZIot_4GSwitch_(schemaID-e1mzybmo)_keyfxuj3d5wg53wx_yiijiejtycigal7g_T1-3S_1.1.9.bin"),
        "args": ["--only-uart", "-chip", "BK7238", "--ble-core"],
        "timeout": 60,
        "expected_strings": [
            "prvHeapInit-start addr:0x415120, size:175840",
            "**********tuya_upgrade_main need upgrade? [255]**********",
            "< BUILD AT:2025_01_08_15_12_50 BY ci_manage FOR tuyaos-iot AT T1 >",
            "tuya_tls_rand_init ok!",
            "calibration_main over",
            "gw_cntl->gw_wsm.stat:1",
            "mf_init succ",
            "scan_start_req_handler",
        ],
    },
    {
        "name": "BK7238 Tuya TOMZN 63A SmartMeterPro breaker (T1-U-HL, 1.1.0) loads 63A config and fast-connects",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "Tuya_TOMZN_63A_Breaker_SmartMeterPro_(schemaID-e1n27y7k)_key7mapd9sdpyyf9_suznd0ma6xr4caz0_T1-U-HL_1.1.0.bin"),
        "args": ["--only-uart", "-chip", "BK7238", "--ble-core"],
        "timeout": 60,
        "expected_strings": [
            "[MEM DBG] heap init-------size:199672 addr:0x40f408---------",
            "< TuyaOS V:3.11.16 BS:40.00_PT:2.3_LAN:3.5_CAD:1.0.5_CD:1.0.0 >",
            ",over_cur: is 63000",
            "tuya_tls_rand_init ok!",
            "!! tkl_wifi_station_fast_connect",
            "found scan rst rssi -37 > -50",
        ],
    },
    {
        "name": "BK7231N Tuya Milfra 3-gang TB11 switch (SDK 2.3.3) initializes BLE 5.1 stack and GAPM config",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7231N_Milfra_3g_TB11_QIO_2024-20-8-15-32-47.bin"),
        "args": ["--only-uart", "-chip", "BK7231N", "-key", "TUYA", "--ble-core"],
        "timeout": 60,
        "expected_strings": [
            "prvHeapInit-start addr:0x410958, size:128680",
            "ble mac:50-8b-b9-6e-c3-ba",
            "ty bt sdk init success finish",
            "< TUYA IOT SDK V:2.3.3 BS:40.00_PT:2.2_LAN:3.4_CAD:1.0.5_CD:1.0.0 >",
            "< BUILD AT:2022_12_22_14_55_08 BY ci_manage FOR ty_iot_sdk AT bk7231n >",
            "gapm_cmp_evt:GAPM_SET_DEV_CONFIG",
        ],
    },
    {
        "name": "BK7231N Tuya A228903 Plafon ceiling light initializes BLE 5.1 GATT DB and advertising",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7231N_QIO_A228903_Plafon_2024-13-1-19-42-24.bin"),
        "args": ["--only-uart", "-chip", "BK7231N", "-key", "TUYA", "--ble-core"],
        "timeout": 60,
        "expected_strings": [
            "prvHeapInit-start addr:0x411930, size:124624",
            "ble mac:18-de-50-ea-4c-c4",
            "ty bt sdk init success finish",
            "gapm_cmp_evt:BLE_STACK_OK",
            "[PLATFORM NOTICE]STACK INIT OK",
            "[PLATFORM NOTICE]CREATE DB SUCCESS",
        ],
    },
    {
        "name": "BK7231N Tuya LSC SmartPanel (CBU, 1.3.1) boots TuyaOS and scans for tuya_mdev_test2",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "Tuya_LSC_SmartPanel_(schemaID-e1mu25o0)_keyj3w8cmutjmwk5_duxmr5beo0hrowle_CBU_1.3.1.bin"),
        "args": ["--only-uart", "-chip", "BK7231N", "-key", "TUYA", "--ble-core"],
        "timeout": 60,
        "expected_strings": [
            "[MEM DBG] heap init-------size:153512 addr:0x40a858---------",
            "tuya_tls_rand_init ok!",
            "mf_init succ",
            "< BUILD AT:2024_12_12_17_45_32 BY ci_manage FOR tuyaos-iot AT bk7231n >",
            "calibration_main over",
            "current product ssid name:tuya_mdev_test2",
            "scan_start_req_handler",
        ],
    },
    {
        "name": "BK7231N Tuya Sprycle SPL11-528 water valve (CB3S, 1.0.2) starts BLE advertising and ADC channel 3",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "Tuya_Sprycle_SPL11-528_Water_Valve_(SchemaID-000003w2yl)_s2vw3kf1575apfyc_CB3S_1.0.2.bin"),
        "args": ["--only-uart", "-chip", "BK7231N", "-key", "TUYA", "--ble-core"],
        "timeout": 60,
        "expected_strings": [
            "prvHeapInit-start addr:0x4114a0, size:125792",
            "< TUYA IOT SDK V:2.3.1 BS:40.00_PT:2.2_LAN:3.3_CAD:1.0.3_CD:1.0.0 >",
            "fj_wifi_ble_controller_moto:1.0.2",
            "[ble_appm_start_advertising]",
            "mf_init succ",
            "tuya_hard_init is success",
            "gw_cntl.gw_if.product_key:s2vw3kf1575apfyc, input:s2vw3kf1575apfyc",
            "scan_start_req_handler",
        ],
    },
    {
        "name": "BK7231N Tuya Eurodomest HDMI Sync Box (CBU, 1.0.7) starts BLE stack and lighting effect task",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "Tuya_Eurodomest_HDMI_Sync_Box_(6204000100_schemaID-er8l48)_aceyfgaokqwk4nzj_CBU_1.0.7.bin"),
        "args": ["--only-uart", "-chip", "BK7231N", "-key", "TUYA", "--ble-core"],
        "timeout": 60,
        "expected_strings": [
            "prvHeapInit-start addr:0x412040, size:122816",
            "gapm_cmp_evt:BLE_STACK_OK",
            "[ble_appm_start_advertising]",
            "calibration_main over",
            "app lighting is enable now",
            "Succeed to malloc space for app_light_cfg.seg_list",
            "lighting effect main task in running",
        ],
    },
    {
        "name": "BK7231N Tuya ATLO-RC-IR universal IR remote (SDK 2.3.1) loads IR pin map and starts BLE adv",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7231N_QIO_ATLO-RC-IR-TUYA_2025-16-1-16-44-50.bin"),
        "args": ["--only-uart", "-chip", "BK7231N", "-key", "TUYA", "--ble-core"],
        "timeout": 60,
        "expected_strings": [
            "prvHeapInit-start addr:0x411550, size:125616",
            "< TUYA IOT SDK V:2.3.1 BS:40.00_PT:2.2_LAN:3.3_CAD:1.0.3_CD:1.0.0 >",
            "gapm_cmp_evt:BLE_STACK_OK",
            "[ble_appm_start_advertising]",
            "mf_init succ",
            "key:infre:-----val:7----len:1",
            "key:infrr:-----val:8----len:1",
            "current product ssid name:tuya_mdev_test1",
        ],
    },
    {
        "name": "BK7231T Tuya W-105N motorized ball valve (WB3S, 1.1.5) loads pConfig and starts BLE adv",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "Tuya_BallValve_W-105N_(WIFI&BLE-_schemaID-0000043vc2)_key7axydcvmea3x9_hjnivuviqdko7mzd_WB3S_1.1.5.bin"),
        "args": ["--only-uart", "-key", "TUYA"],
        "timeout": 60,
        "expected_strings": [
            "< TUYA IOT SDK V:1.0.6 BS:40.00_PT:2.2_LAN:3.3_CAD:1.0.2_CD:1.0.0 >",
            "**********[oem_bk7231s_rnd_zero_dctrl_switch] [1.1.5] compiled at Jun 22 2021",
            "mf_init succ",
            "gw_cntl.gw_if.product_key:key7axydcvmea3x9, input:key7axydcvmea3x9",
            "device_init ok  free_mem_size:67688",
            "appm start advertising",
        ],
    },
    {
        "name": "BK7231T Tuya AHRISE AHR-083 power strip (WB3S, 1.1.0) loads multi-relay pConfig and advertises BLE",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "Tuya_AHRISE_AHR-083_PowerStrip_(schemaID-000002qqwe)_keya4gvchmtapm8n_fn2jtjtvkegj2oll_WB3S_1.1.0.bin"),
        "args": ["--only-uart", "-key", "TUYA"],
        "timeout": 60,
        "expected_strings": [
            "< TUYA IOT SDK V:1.0.2 BS:40.00_PT:2.2_LAN:3.3_CAD:1.0.2_CD:1.0.0 >",
            "**********[oem_bk7231s_rnd_switch] [1.1.0] compiled at Oct  3 2020 15:09:14",
            "mf_init succ",
            "gw_cntl.gw_if.product_key:keya4gvchmtapm8n, input:keya4gvchmtapm8n",
            "IO - wifi_stat:\tpin-9\ttype low",
            "device_init ok  free_mem_size:66344",
            "appm start advertising",
        ],
    },
    {
        "name": "BK7231T Tuya MS-103 two-way TempHum switch (1.2.8, SDK 1.0.2) starts BLE advertising and scans",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "Tuya_MS-103-Two-Way_TempHum_Switch_(NH-YM0430_schemaID-0000037bpa)_keyxnxg34hf5xw9e_dlsdk5zvkuuhufpy_1.2.8.bin"),
        "args": ["--only-uart", "-key", "TUYA"],
        "timeout": 60,
        "expected_strings": [
            "< TUYA IOT SDK V:1.0.2 BS:40.00_PT:2.2_LAN:3.3_CAD:1.0.2_CD:1.0.0 >",
            "**********[oem_bk7231_2onoff_wetemp] [1.2.8] compiled at Jul 16 2021 14:33:15",
            "ble adv && resp changed",
            "appm start advertising",
            "mf_init succ",
            "scan_start_req_handler",
        ],
    },
    {
        "name": "BK7231T Tuya TreatLife ST30 TV LED backlight (1.0.2, SDK 1.0.2) initializes RGB frame, BLE and ADC",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "Tuya_TreatLife_ST30_TV_LED_Backlight_(ST30HSR_schemaID-000002fli4)_xhsx0clcbovz5xre_1.0.2.bin"),
        "args": ["--only-uart", "-key", "TUYA"],
        "timeout": 60,
        "expected_strings": [
            "--------------> PID:xhsx0clcbovz5xre",
            "**********[HYS_03_010_RGB_xhsx0clcbovz5xre] [1.0.2] compiled at Dec 21 2020 16:13:43",
            "appm start advertising",
            "mf_init succ",
            "gw_cntl.gw_if.product_key:xhsx0clcbovz5xre, input:xhsx0clcbovz5xre",
            "frame init ok!",
            "==== tuya_hal_adc_init enter ==== channel:3",
        ],
    },
    {
        "name": "BK7231T Tuya BIM8774 16A 3500W metering socket (WB2S, 1.3.5) boots dltj energy app and BLE",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "Tuya_BIM8774_Socket_16A_3500W_(BOMSOC8774_schemaID-0000032ccw)_keym9qkuywghyrvs_jug4i7aunvvjfhq7_WB2S_1.3.5.bin"),
        "args": ["--only-uart", "-key", "TUYA"],
        "timeout": 60,
        "expected_strings": [
            "< TUYA IOT SDK V:1.0.2 BS:40.00_PT:2.2_LAN:3.3_CAD:1.0.2_CD:1.0.0 >",
            "**********[oem_bk7231s_dltj] [1.3.5] compiled at Dec 25 2020 10:09:10**********",
            "mf_init succ",
            "gw_cntl.gw_if.product_key:keym9qkuywghyrvs, input:keym9qkuywghyrvs",
            "dltj_run_int",
            "device_init ok  free_mem_size:67376",
            "appm start advertising",
        ],
    },
    {
        "name": "BK7231T Tuya 120V A19 9W smart bulb (WB2L, 2.9.6) raises SmartLife-8811 AP and auto-blinks",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "Tuya_120V_A19_9W_SMART_BULB_(schemaID-000002p9xg)_keytg5kq8gvkv9dh_mlgrefimabrept8a_WB2L_2.9.6.bin"),
        "args": ["--only-uart", "-key", "TUYA"],
        "timeout": 60,
        "expected_strings": [
            "**********[oem_bk7231s_light_ty] [2.9.6] compiled at Oct 29 2020 14:38:00**********",
            "mf_init succ",
            "gw_cntl.gw_if.product_key:keytg5kq8gvkv9dh, input:keytg5kq8gvkv9dh",
            "frame init ok!",
            "ssid:SmartLife-8811\tkey:  channnel: 6",
            "appm start advertising",
            "start AP config auto blink",
        ],
    },
    {
        "name": "BK7231T Tuya 120V A60 9W smart bulb (WB2L, 2.9.25, SDK 2.3.2) loads 5ch light config and boots",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "Tuya_120V_A60_9W_SMART_BULB_(schemaID-000004kvcq)_keytg5kq8gvkv9dh_uuyc3x6l7gusczhb_WB2L_2.9.25.bin"),
        "args": ["--only-uart", "-key", "TUYA"],
        "timeout": 60,
        "expected_strings": [
            "prvHeapInit-start addr:0x421b18, size:124136",
            "crc_r[ca] crc_g[00] crc_b[67]",
            "goto first bright up!",
            "ty bt sdk init success finish",
            "mf_init succ",
            "< TUYA IOT SDK V:2.3.2 BS:40.00_PT:2.2_LAN:3.3_CAD:1.0.4_CD:1.0.0 >",
            "< BUILD AT:2022_02_25_14_49_05 BY ci_manage FOR ty_iot_sdk AT bk7231t >",
        ],
    },
    {
        "name": "BK7231T Tuya 1CH WB01 switch (WB3S, 1.1.5, SDK 1.0.6) loads pConfig and starts BLE adv",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "Tuya_1CH_WB01_Switch_(schemaID-00000489v0)_key7axydcvmea3x9_etehelyd9guknfyi_WB3S_1.1.5.bin"),
        "args": ["--only-uart", "-key", "TUYA"],
        "timeout": 60,
        "expected_strings": [
            "< TUYA IOT SDK V:1.0.6 BS:40.00_PT:2.2_LAN:3.3_CAD:1.0.2_CD:1.0.0 >",
            "**********[oem_bk7231s_rnd_zero_dctrl_switch] [1.1.5] compiled at Jun 22 2021",
            "appm start advertising",
            "mf_init succ",
            "pConfig = {rl1_lv:1,bt_type:0,bt1_pin:6,mxcl_led_m:0,led1_pin:11,rand_dpid:18",
            "gw_cntl.gw_if.product_key:key7axydcvmea3x9, input:key7axydcvmea3x9",
            "device_init ok  free_mem_size:68040",
        ],
    },
    {
        "name": "OpenBeken DeepSleep 10 enters SCTRL deep sleep and wakes via RTC timer",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "OpenBK7231T_QIO_1.18.300_mathDemo_obkStartupCommand_uartConsole.bin"),
        "args": ["--only-uart", "-key", "TUYA",
                 "--uart1-rx", "DeepSleep 10",
                 "--deep-sleep-wake", "rtc"],
        "timeout": 60,
        "tags": ["deep sleep"],
        "expected_strings": [
            "Warn:CFG:CFG_InitAndLoad: Correct config has been loaded with 0 changes count.",
            "[SLEEP] entered deep sleep (RTC=10s)",
            "[SLEEP] waking from deep sleep (RTC timer); device reset #1",
            "Info:CMD:StartupBeforeConsole",
        ],
    },
    {
        "name": "OpenBeken PinDeepSleep enters SCTRL deep sleep and wakes via GPIO P10",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "OpenBK7231T_QIO_1.18.300_mathDemo_obkStartupCommand_uartConsole.bin"),
        "args": ["--only-uart", "-key", "TUYA",
                 "--uart1-rx", "SetPinRole 10 Btn\\nPinDeepSleep",
                 "--deep-sleep-wake", "gpio:10"],
        "timeout": 60,
        "tags": ["deep sleep"],
        "expected_strings": [
            "Warn:CFG:CFG_InitAndLoad: Correct config has been loaded with 0 changes count.",
            "[SLEEP] entered deep sleep (GPIO=[P10:LOW])",
            "[SLEEP] waking from deep sleep (GPIO P10=0); device reset #1",
            "Warn:CFG:CFG_InitAndLoad: Correct config has been loaded with 1 changes count.",
        ],
    },
    {
        "name": "BK7231N Tuya Generic Motion Sensor (CBU, 1.0.6) enters deep sleep and wakes on PIR GPIO P16",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7231N_Tuya_Generic_Motion_Sensor_CBU_1.0.6.bin"),
        "args": ["--only-uart", "-chip", "BK7231N", "-key", "TUYA",
                 "--wifi", "--deep-sleep-wake", "gpio:16"],
        "timeout": 90,
        "tags": ["deep sleep"],
        "expected_strings": [
            "upload_by_wake_event,cur_wake_type:0",
            "enter sleep mode,sleep_timer is 28800",
            "[SLEEP] entered deep sleep (RTC=28800s, GPIO=[P16:HIGH, P20:HIGH])",
            "[SLEEP] waking from deep sleep (GPIO P16=1); device reset #1",
            "upload_by_wake_event,cur_wake_type:3",
        ],
    },
    {
        "name": "BK7231N Tuya Rain Sensor (CBU, 1.1.9) enters deep sleep and wakes on water-probe GPIO P16",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7231N_Tuya_Rain_Sensor_CBU_1.1.9.bin"),
        "args": ["--only-uart", "-chip", "BK7231N", "-key", "TUYA",
                 "--gpio-input", "20:1", "--deep-sleep-wake", "gpio:16"],
        "timeout": 90,
        "tags": ["deep sleep"],
        "expected_strings": [
            "oem_bk7231n_water_sensor_plus:1.1.9",
            "power_on wake up",
            "enter sleep mode,sleep_timer is 28800",
            "[SLEEP] entered deep sleep (RTC=28800s, GPIO=[P16:HIGH, P20:LOW])",
            "[SLEEP] waking from deep sleep (GPIO P16=1); device reset #1",
            "IO wake up",
            "g_dev_ctl.wake_event is :2",
        ],
    },
    {
        "name": "BK7231N Tuya TH01 TempHum Sensor (CBU, 1.1.17) enters 3600s deep sleep and wakes via RTC timer",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7231N_Tuya_TH01_TempHum_CBU_1.1.17.bin"),
        "args": ["--only-uart", "-chip", "BK7231N", "-key", "TUYA",
                 "--gpio-input", "20:1", "--deep-sleep-wake", "rtc"],
        "timeout": 90,
        "tags": ["deep sleep"],
        "expected_strings": [
            "oem_bk7231n_temp_hum_sensor:1.1.17",
            "Wakeup_GetEvent is:0",
            "sleep sec is:3600",
            "[SLEEP] entered deep sleep (RTC=3600s, GPIO=[P16:HIGH, P20:LOW])",
            "[SLEEP] waking from deep sleep (RTC timer); device reset #1",
            "Wakeup_GetEvent is:4",
        ],
    },
    {
        "name": "BK7238 Tuya SRW-TH01 TempHum Sensor (T1-AHL, 1.0.1) enters 240s deep sleep and wakes via RTC timer",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7238_Tuya_SRW-TH01_TempHum_T1-AHL_1.0.1.bin"),
        "args": ["--only-uart", "-chip", "BK7238",
                 "--wifi", "--deep-sleep-wake", "rtc"],
        "timeout": 90,
        "tags": ["deep sleep"],
        "expected_strings": [
            "start_type:0",
            "---enter deep sleep :wake up with gpio 0~31 ps: 0x300 0x100",
            "---enter deep sleep :wake up with   rosc  ps :240 s",
            "[SLEEP] entered deep sleep (RTC=240s, GPIO=[P8:LOW, P9:HIGH])",
            "[SLEEP] waking from deep sleep (RTC timer); device reset #1",
            "start_type:4",
        ],
    },
    {
        "name": "BK7252N Pinmei A9 Camera (SDK 3.00.42.01) probes GC0328C over I2C1 and captures JPEG frame via EJPEG+GDMA",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7252N_Pinmei_A9_Camera_BKFIL_3.00.42.01.bin"),
        "args": ["--only-uart", "-chip", "BK7252N",
                 "--uart1-rx", "video_buffer open\\nvideo_buffer read 4096\\n"],
        "timeout": 90,
        "tags": ["camera", "EJPEG"],
        "expected_strings": [
            "0328c id_data:9d",
            "GC0328C init finish",
            "jpeg opened",
            "vbuf opened",
            "[CAMERA] GC0328C streaming 640x480 JPEG (1180 bytes/frame",
            "get frame ret: 0, len:1185",
            "ff,d8,ff,e0,00,10,4a,46,49,46,",
        ],
    },
    {
        "name": "BK7252 Linklemo XCThings HA15 Camera (IN-A15-V1.0) probes GC0328C over I2C1 and streams EJPEG+GDMA frames",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7252_Linklemo_XCThings_HA15_IN-A15-V1.0_12.12.16.bin"),
        "args": ["--only-uart", "-chip", "BK7252",
                 "--uart1-rx", "video open\\n"],
        "timeout": 90,
        "tags": ["camera", "EJPEG"],
        "expected_strings": [
            "video_transfer_main entry",
            "open I2C1",
            "0328c id_data:9d",
            "GC0328C init finish",
            "[CAMERA] GC0328C streaming 640x480 JPEG (1180 bytes/frame",
        ],
    },
    {
        "name": "BK7252 Tuya A9 Cam SC-10024 (1.0.4, SDK 3.0.29) raises SmartLife-2640 Soft-AP and serves DHCP",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7252_Tuya_A9_Cam_SC-10024_1.0.4.bin"),
        "args": ["--only-uart", "-chip", "BK7252", "--wifi-peer", "auto"],
        "timeout": 90,
        "tags": ["camera"],
        "expected_strings": [
            "SDK Rev: 3.0.29",
            "SC008-WB00:1.0.4",
            "wifi soc init. pid:keyph8vwhadwsx3a",
            "serial_no:381f8da92640",
            "[WIFI] associated with 'SmartLife-2640' (AID 1)",
            "[NET] DHCP: we got 192.168.175.100/255.255.255.0 from the device (router 192.168.175.1)",
            "[NET] ping reply from 192.168.175.1 (seq 1)",
        ],
    },
    {
        "name": "BK7252 Tuya Doorbell IP06 V6.3 (SC028_WB30_ALK, 2.2.4) boots BLE, SDRAM heap and reads 4MB flash key",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7252_Tuya_Doorbell_IP06_V6.3_2.2.4.bin"),
        "args": ["--only-uart", "-chip", "BK7252", "-key", "TUYA"],
        "timeout": 90,
        "tags": ["camera"],
        "expected_strings": [
            "prvHeapInit-start addr:0x420cb0, size:127824",
            "[MEM DBG] heap init-------size:229376 addr:0x908000---------",
            "SDK Rev: 3.0.29",
            "ty bt sdk init success finish",
            "firmware compiled at [Sep 13 2024 14:31:56]",
            "key_addr: 0x397000   block_sz 4096",
            "0xc3 0x3d 0x40 0x5b 0x90 0xfa 0x59 0x81 0x20 0x31 0x54 0xd8 0x80 0xa9 0x8f 0xc",
        ],
    },
    {
        "name": "BK7252 Tuya ONENUO ML1T-B Doorbell (SC028_WB30_ALK, 2.2.4) boots BLE, SDRAM heap and reads 4MB flash key",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7252_Tuya_ONENUO_ML1T-B_Doorbell_2.2.4.bin"),
        "args": ["--only-uart", "-chip", "BK7252", "-key", "TUYA"],
        "timeout": 90,
        "tags": ["camera"],
        "expected_strings": [
            "prvHeapInit-start addr:0x420cb8, size:127816",
            "[MEM DBG] heap init-------size:229376 addr:0x908000---------",
            "SDK Rev: 3.0.29",
            "ty bt sdk init success finish",
            "SC028_WB30_ALK:2.2.4",
            "firmware compiled at [Jul 20 2023 14:41:22]",
            "key_addr: 0x397000   block_sz 4096",
            "0x89 0x33 0xfa 0xd1 0x58 0xfb 0x7f 0x3b 0xe7 0x24 0x97 0xd2 0x15 0xb2 0x6f 0xbd",
        ],
    }
]

# Report-facing prose, one per test name. Kept separate from TEST_CASES so the
# (working, exactly-asserted) case definitions stay untouched; the inline
# comments there document specific assertion strings for maintainers, while
# these summarise each test for the HTML report's expandable description.
DESCRIPTIONS = {
    "OpenBeken DeepSleep 10 enters SCTRL deep sleep and wakes via RTC timer":
        "Exercises OpenBeken's `DeepSleep 10` command on BK7231T: the firmware counts down, "
        "programs the SCTRL 32 kHz ROSC timer (`0x0080011C`/`0x00800140`) for 10 seconds, and "
        "writes `0xADC1` to `SCTRL_SLEEP` (`0x00800104`). With `--deep-sleep-wake rtc`, the "
        "emulator wakes the chip via warm SCTRL reset with `ROSC_TIMER_INT_STATUS` set.",
    "OpenBeken PinDeepSleep enters SCTRL deep sleep and wakes via GPIO P10":
        "Exercises OpenBeken's `PinDeepSleep` door-sensor sleep mode on BK7231T: after configuring "
        "P10 as a button (`SetPinRole 10 Btn`), `PinDeepSleep` arms `SCTRL_GPIO_WAKEUP_EN` "
        "(`0x00800120`) and `SCTRL_GPIO_WAKEUP_TYPE` (`0x00800124`) for active-low wake on P10, "
        "enters deep sleep via `0x00800104`, and wakes via `--deep-sleep-wake gpio:10`.",
    "BK7231N Tuya Generic Motion Sensor (CBU, 1.0.6) enters deep sleep and wakes on PIR GPIO P16":
        "A stock battery-powered Tuya PIR motion sensor on BK7231N (CBU module, firmware 1.0.6). "
        "On cold boot it reports `cur_wake_type:0`, arms a 28800s RTC timer plus GPIO wake on P16 "
        "(PIR trigger, active-high) and P20 (button), and enters SCTRL deep sleep (`0x00800104`). "
        "When woken via `--deep-sleep-wake gpio:16`, the firmware reads the retained SCTRL/AON GPIO "
        "wake status registers (`0x00800128`/`0x00808004`) and reports `cur_wake_type:3` (PIR alarm).",
    "BK7231N Tuya Rain Sensor (CBU, 1.1.9) enters deep sleep and wakes on water-probe GPIO P16":
        "A stock battery-powered Tuya water/rain leak sensor on BK7231N (`oem_bk7231n_water_sensor_plus:1.1.9`, "
        "CBU module). On cold boot it logs `power_on wake up`, arms a 28800s RTC timer plus GPIO wake on "
        "P16 (water probe, active-high) and P20 (reset button, active-low), and enters SCTRL deep sleep. "
        "Waking on P16 (`--deep-sleep-wake gpio:16`) reboots the chip with GPIO wake status set so the "
        "firmware logs `IO wake up` and `g_dev_ctl.wake_event is :2`.",
    "BK7231N Tuya TH01 TempHum Sensor (CBU, 1.1.17) enters 3600s deep sleep and wakes via RTC timer":
        "A stock battery-powered Tuya TH01 temperature and humidity sensor on BK7231N "
        "(`oem_bk7231n_temp_hum_sensor:1.1.17`, CBU module). On cold boot it logs `Wakeup_GetEvent is:0`, "
        "arms a 3600s SCTRL ROSC timer plus GPIO wake on P16/P20, and enters deep sleep. When woken via "
        "`--deep-sleep-wake rtc`, the warm reset sets `ROSC_TIMER_INT_STATUS` (`0x0080011C` bit 8) and "
        "the firmware recognizes an RTC wake (`Wakeup_GetEvent is:4`).",
    "BK7238 Tuya SRW-TH01 TempHum Sensor (T1-AHL, 1.0.1) enters 240s deep sleep and wakes via RTC timer":
        "A stock battery-powered Tuya SRW-TH01 temperature and humidity sensor on BK7238 (T1-AHL module, "
        "firmware 1.0.1). On cold boot (`start_type:0`) it scans for its AP, then arms GPIO wake on P8/P9 "
        "and a 240s ROSC timer before writing `0xADC1` to `SCTRL_SLEEP` (`0x00800104`). Waking via "
        "`--deep-sleep-wake rtc` resets the chip with `ROSC_TIMER_INT_STATUS` set, which `bk_misc_init_start_type` "
        "reports as `start_type:4` (`RESET_SOURCE_DEEPPS_RTC`).",
    "BK7252N Pinmei A9 Camera (SDK 3.00.42.01) probes GC0328C over I2C1 and captures JPEG frame via EJPEG+GDMA":
        "End-to-end DVP camera sensor and hardware JPEG capture test on BK7252N (`BK7252N_Pinmei_A9_Camera_BKFIL_3.00.42.01.bin`). "
        "Typing `video_buffer open` and `video_buffer read 4096` into the RT-Thread MSH shell initializes Timer 5 "
        "(`0x00802A4C`), probes the emulated GalaxyCore GC0328C sensor at I2C1 slave `0x21` (chip ID `0x9D` at reg `0xF0`), "
        "writes all 374 sensor configuration registers, enables `YUV_BUF` (`0x00A04000`) and `EJPEG` (`0x00A03000`) with "
        "GDMA channel 4 circular capture (`0x00809140`), and reads back a complete 1185-byte JFIF JPEG frame (`ff,d8,ff,e0,...`).",
    "BK7252 Linklemo XCThings HA15 Camera (IN-A15-V1.0) probes GC0328C over I2C1 and streams EJPEG+GDMA frames":
        "End-to-end DVP camera sensor and EJPEG streaming test on BK7252 (`BK7252_Linklemo_XCThings_HA15_IN-A15-V1.0_12.12.16.bin`). "
        "Typing `video open` into the RT-Thread MSH shell starts `video_transfer_main`, opens hardware `I2C1` (`0x00802300`), "
        "identifies the emulated GC0328C sensor (`id_data:9d`), writes 391 sensor configuration registers, and streams "
        "640x480 JPEG frames through the BK7252 EJPEG encoder (`0x0080A000`) and GDMA channel 4 (`0x00809080`).",
    "BK7252 Tuya A9 Cam SC-10024 (1.0.4, SDK 3.0.29) raises SmartLife-2640 Soft-AP and serves DHCP":
        "A 4 MiB plaintext CRC-framed BK7252 dump from a Tuya SC-10024 A9 VGA Wi-Fi camera (`SC008-WB00:1.0.4` on "
        "SDK 3.0.29, PID `keyph8vwhadwsx3a`). It initializes its 256 KiB external SDRAM heap at `0x00900000`, raises its "
        "`SmartLife-2640` pairing Soft-AP, and hands our virtual Wi-Fi station a `192.168.175.100` DHCP lease and ICMP ping reply.",
    "BK7252 Tuya Doorbell IP06 V6.3 (SC028_WB30_ALK, 2.2.4) boots BLE, SDRAM heap and reads 4MB flash key":
        "A 4 MiB TUYA-encrypted BK7252 dump from an IP06 V6.3 smart video doorbell (`SC028_WB30_ALK:2.2.4` on SDK 3.0.29). "
        "It initializes both internal SRAM and 224 KiB external SDRAM heaps, brings up the BLE stack (`ty bt sdk init success finish`), "
        "completes RF calibration, and reads its protected flash key from `0x397000` in upper 4 MiB SPI flash.",
    "BK7252 Tuya ONENUO ML1T-B Doorbell (SC028_WB30_ALK, 2.2.4) boots BLE, SDRAM heap and reads 4MB flash key":
        "A 4 MiB TUYA-encrypted BK7252 dump from an ONENUO ML1T-B smart video doorbell (`SC028_WB30_ALK:2.2.4` on SDK 3.0.29, "
        "PID `keytqn8773d8kwuu`). It initializes internal SRAM and external SDRAM heaps, brings up the BLE stack, completes "
        "RF calibration, and reads its protected flash key from `0x397000`.",
    "BL2028N Zemismart Matter ZML2 RGBCT bulb (UAM026, 1.21) prints Matter SetupQRCode and drives pwm_new":
        "A plaintext BL2028N dump from a Zemismart ZML2 Matter over Wi-Fi RGBCT smart bulb "
        "(CSA23291MAT40804-24, Uascent UAM026 v1.21). It initializes the Matter commissioning "
        "stack, prints its SetupQRCode (MT:--VJ0-IC005WSL02810) and QR helper URL on UART, "
        "and drives the BK7231N/BL2028N pwm_new peripheral.",
    "BK7238 Tuya Arlec PC191HA-S4 plug (T1-2S-NL, 1.0.2) calibrates and fast-connects to stored AP":
        "A plaintext BK7238 dump from an Arlec Grid Connect PC191HA Series 4 smart plug "
        "(oem_t1_plug:1.0.2 on TuyaOS, T1-2S-NL module). It completes RF calibration and TLS "
        "RNG initialization, reads its protected key at 0x1f5000, and triggers Wi-Fi station "
        "fast-connect to its stored 'amc' network.",
    "BK7238 Tuya BSEED M1 metering socket (T1-2S-NL, 1.1.4) boots TuyaOS plug app and scans":
        "A plaintext BK7238 dump from a BSEED M1 energy-metering wall socket (oem_t1_plug:1.1.4 "
        "on TuyaOS, T1-2S-NL module). It completes RF calibration and TLS RNG initialization, "
        "recovers its protected flash key at 0x1f5000, finishes mf_init, and starts Wi-Fi scanning.",
    "BK7238 Tuya QS-WIFI-S10-C04 curtain module (T1-M, 1.0.6) raises SmartLife-BF55 AP and BLE central":
        "A plaintext BK7238 dump from a Lonsonho QS-WIFI-S10-C04 smart curtain switch module "
        "(tuyaos_curtain:1.0.6 on TuyaOS 3.11.11, T1-M module). It initializes the BLE 5.2 stack "
        "and BLE central remote-control listener, validates product ID ocz3mqv0v4ntkzva, raises "
        "its open SmartLife-BF55 pairing Soft-AP, and starts app_key_task.",
    "BK7238 Tuya AZIot 4-gang switch (T1-3S, 1.1.9) calibrates and starts Wi-Fi scan":
        "A plaintext BK7238 dump from an AZIot 4-gang smart wall switch (oem_t1_switch:1.1.9 on "
        "TuyaOS, T1-3S module). It completes RF calibration and TLS RNG initialization, recovers "
        "its protected flash key at 0x1f5000, finishes mf_init, and starts Wi-Fi scanning.",
    "BK7238 Tuya TOMZN 63A SmartMeterPro breaker (T1-U-HL, 1.1.0) loads 63A config and fast-connects":
        "A plaintext BK7238 dump from a TOMZN 63A DIN-rail smart circuit breaker and energy meter "
        "(TuyaOS 3.11.16, T1-U-HL module, 1.1.0). It loads its bistable relay and 63000 mA "
        "over-current configuration from flash, completes RF calibration, and initiates Wi-Fi "
        "fast-connect to its stored 'shuoda5' network.",
    "BK7231N Tuya Milfra 3-gang TB11 switch (SDK 2.3.3) initializes BLE 5.1 stack and GAPM config":
        "A stock BK7231N QIO dump from a Milfra 3-gang TB11 smart wall switch (TUYA IOT SDK "
        "2.3.3). With the BK7231N BLE 5.1 controller modeled, it initializes the RW BLE stack, "
        "logs BLE MAC 50-8b-b9-6e-c3-ba, and completes GAPM device configuration.",
    "BK7231N Tuya A228903 Plafon ceiling light initializes BLE 5.1 GATT DB and advertising":
        "A stock BK7231N QIO dump from an A228903 RGBCW LED ceiling plafon light. It initializes "
        "the RivieraWaves BLE 5.1 stack (BLE_STACK_OK, STACK INIT OK), creates its GATT database "
        "(CREATE DB SUCCESS), and starts BLE advertising.",
    "BK7231N Tuya LSC SmartPanel (CBU, 1.3.1) boots TuyaOS and scans for tuya_mdev_test2":
        "A stock BK7231N dump from an LSC Smart Connect LED SmartPanel (CBU module, firmware "
        "1.3.1 on TuyaOS). It recovers its protected key at 0x1ee000, initializes TLS RNG and "
        "mf_init, completes RF calibration, and scans for the tuya_mdev_test2 factory SSID.",
    "BK7231N Tuya Sprycle SPL11-528 water valve (CB3S, 1.0.2) starts BLE advertising and ADC channel 3":
        "A stock BK7231N dump from a Sprycle SPL11-528 smart irrigation water timer valve "
        "(fj_wifi_ble_controller_moto:1.0.2 on TUYA IOT SDK 2.3.1, CB3S module). It brings up "
        "BLE 5.1 advertising, validates product key s2vw3kf1575apfyc, initializes battery/valve "
        "SARADC channel 3, and starts Wi-Fi scanning.",
    "BK7231N Tuya Eurodomest HDMI Sync Box (CBU, 1.0.7) starts BLE stack and lighting effect task":
        "A stock BK7231N dump from a Eurodomest 6204000100 HDMI ambient-lighting TV sync box "
        "(CBU module, firmware 1.0.7). It brings up the BLE 5.1 stack and advertising, completes "
        "RF calibration, allocates its segmented lighting buffer, and launches the lighting "
        "effect main task.",
    "BK7231N Tuya ATLO-RC-IR universal IR remote (SDK 2.3.1) loads IR pin map and starts BLE adv":
        "A stock BK7231N QIO dump from an ATLO-RC-IR-TUYA Wi-Fi universal infrared remote blaster "
        "(TUYA IOT SDK 2.3.1). It brings up the BLE 5.1 stack and advertising, reads its protected "
        "configuration mapping IR transmit (P7), IR receive (P8), reset button (P9) and status LED "
        "(P24), and scans for tuya_mdev_test1.",
    "BK7231T Tuya W-105N motorized ball valve (WB3S, 1.1.5) loads pConfig and starts BLE adv":
        "A stock BK7231T dump from a W-105N Wi-Fi + BLE motorized water/gas ball valve controller "
        "(oem_bk7231s_rnd_zero_dctrl_switch:1.1.5 on TUYA IOT SDK 1.0.6, WB3S module). It loads "
        "its pin configuration (bt1_pin:26, led1_pin:24), validates product key key7axydcvmea3x9, "
        "completes device_init, and starts BLE advertising.",
    "BK7231T Tuya AHRISE AHR-083 power strip (WB3S, 1.1.0) loads multi-relay pConfig and advertises BLE":
        "A stock BK7231T dump from an AHRISE AHR-083 multi-socket smart power strip "
        "(oem_bk7231s_rnd_switch:1.1.0 on TUYA IOT SDK 1.0.2, WB3S module). It recovers its "
        "protected key at 0x1ee000, loads its multi-relay pin table (wifi_stat on P9), validates "
        "product key keya4gvchmtapm8n, and starts BLE advertising.",
    "BK7231T Tuya MS-103 two-way TempHum switch (1.2.8, SDK 1.0.2) starts BLE advertising and scans":
        "A stock BK7231T dump from an MS-103 two-way temperature and humidity thermostat switch "
        "(oem_bk7231_2onoff_wetemp:1.2.8 on TUYA IOT SDK 1.0.2). It completes RF calibration, "
        "brings up BLE GATT advertising, loads its relay/sensor pin configuration, and starts "
        "Wi-Fi scanning.",
    "BK7231T Tuya TreatLife ST30 TV LED backlight (1.0.2, SDK 1.0.2) initializes RGB frame, BLE and ADC":
        "A stock BK7231T dump from a TreatLife ST30 RGB TV ambient LED backlight strip "
        "(HYS_03_010_RGB_xhsx0clcbovz5xre:1.0.2 on TUYA IOT SDK 1.0.2). It initializes the light "
        "control frame in mode 1, brings up BLE advertising, validates PID xhsx0clcbovz5xre, "
        "initializes SARADC channel 3 for mic/music sensing, and starts Wi-Fi scanning.",
    "BK7231T Tuya BIM8774 16A 3500W metering socket (WB2S, 1.3.5) boots dltj energy app and BLE":
        "A stock BK7231T dump from a BIM8774 (BOMSOC8774) 16A 3500W energy-metering smart socket "
        "(oem_bk7231s_dltj:1.3.5 on TUYA IOT SDK 1.0.2, WB2S module). It recovers its protected "
        "key, validates product key keym9qkuywghyrvs, initializes the dltj energy accumulator, "
        "and starts BLE advertising.",
    "BK7231T Tuya 120V A19 9W smart bulb (WB2L, 2.9.6) raises SmartLife-8811 AP and auto-blinks":
        "A stock BK7231T dump from a 120V A19 9W RGBCW smart bulb (oem_bk7231s_light_ty:2.9.6 on "
        "TUYA IOT SDK 1.0.2, WB2L module). It initializes the 5-channel light frame, validates "
        "product key keytg5kq8gvkv9dh, raises its SmartLife-8811 pairing Soft-AP on channel 6, "
        "starts BLE advertising, and begins AP-config auto-blinking.",
    "BK7231T Tuya 120V A60 9W smart bulb (WB2L, 2.9.25, SDK 2.3.2) loads 5ch light config and boots":
        "A stock BK7231T dump from a 120V A60 9W RGBCW smart bulb (oem_bk7231s_light_ty:2.9.25 on "
        "TUYA IOT SDK 2.3.2, WB2L module). It loads its 5-channel PWM pin map (2 1 0 4 3) and "
        "color balance table (28 28 28 50 50), initializes the BLE stack, and recovers its "
        "protected flash key.",
    "BK7231T Tuya 1CH WB01 switch (WB3S, 1.1.5, SDK 1.0.6) loads pConfig and starts BLE adv":
        "A stock BK7231T dump from a 1-channel WB01 smart switch "
        "(oem_bk7231s_rnd_zero_dctrl_switch:1.1.5 on TUYA IOT SDK 1.0.6, WB3S module). It brings "
        "up BLE advertising, loads its relay/button/LED pConfig (bt1_pin:6, led1_pin:11), "
        "validates product key key7axydcvmea3x9, and reaches wifi_stat = 5.",
    "BK7231T Tuya TreatLife SS02 3-way switch (1.1.3, SDK 1.0.8) configures 3-way GPIOs and BLE":
        "A stock BK7231T dump from a TreatLife SS02 3-way smart wall switch "
        "(oem_bk7231s_double_switch_rmd:1.1.3 on TUYA IOT SDK 1.0.8). It configures 3-way "
        "traveler/button inputs P6, P26 and P28 with pull-ups (0x3C) and relay/LED outputs P8 "
        "(0x02), P9 (0x00) and P24 (0x00), brings up the BLE GATT database (STACK INIT OK, "
        "CREATE DB SUCCESS), and starts advertising.",
    "BK7231T Tuya MCB238 DDSU238 DIN-rail breaker (WB3S, 1.1.8) starts BLE advertising and loads bistable pConfig":
        "A stock BK7231T dump from a Tuya MCB238 / DDSU238 DIN-rail circuit breaker "
        "(oem_bk7231s_rnd_switch:1.1.8 on TUYA IOT SDK 1.0.2, WB3S module). It brings up the "
        "BLE GATT database and advertising (STACK INIT OK, CREATE DB SUCCESS, appm start "
        "advertising), completes mf_init, and logs its recovered bistable-relay pConfig "
        "(rl_on1_pin:9, rl_off1_pin:8, bt1_pin:26, led1_pin:6, netled_pin:24).",
    "BK7231T Tuya qcsc400w-v5t smart curtain switch (WB3S, 1.6.0) loads 3-sub-channel curtain config":
        "A stock BK7231T dump from a Tuya qcsc400w-v5t motorized smart curtain switch "
        "(oem_bk7231s_curtain_switch:1.6.0 on TUYA IOT SDK 1.0.2, WB3S module). It recovers "
        "its protected user_param_key, parses and logs its three-sub-channel curtain relay and "
        "button mapping (open/stop/close buttons on P8/P10/P24, motor relays on P7/P6 with "
        "200 ms interlock and drive times), and finishes curtain_json_init and dev_event_json_init.",
    "BK7231T Tuya MOES MS-102 garage door opener (WB2S, 1.1.2) configures contact/relay GPIOs and BLE":
        "A stock BK7231T dump from a MOES MS-102 smart garage door controller "
        "(oem_bk7231s_garage_door:1.1.2 on TUYA IOT SDK 1.0.2, WB2S module). It recovers its "
        "protected flash configuration, configures the door-contact sensor input P26 with a "
        "pull-down (0x2C), button P6 with a pull-up (0x3C) and relay/LED outputs P7 (0x02) and "
        "P24 (0x00), completes device_ctrl_init, and updates its BLE advertisement.",
    "BK7231T Tuya Genio Nebular Projector I003323 (5ch 1000 Hz PWM + laser, 1.1.2) boots":
        "A stock BK7231T dump from a Mirabella Genio Nebular Star Projector (I003323, "
        "oem_bk7231t_light3_laser_nanxin:1.1.2 on TUYA IOT SDK 1.0.2). It configures five "
        "1000 Hz hardware PWM channels (P6/P8/P9/P24/P26 = PWM0/2/3/4/5, period 0x6590 = 26000 "
        "ticks, with PWM0 at 50% duty 0x32C8 and PWM2..4 at 100% duty) alongside GPIO output P7 "
        "(0x00) and button input P14 (0x3C).",
    "BK7231T Tuya Arlec SSWF01G fan speed controller (1.0.5, SDK 2.0.0) configures fan relays and BLE":
        "A stock BK7231T dump from an Arlec / Tuya SSWF01G-WIFI ceiling fan speed controller "
        "(oem_bk7231s_fan_control_arlec:1.0.5 on TUYA IOT SDK 2.0.0 BS:30.06). It recovers its "
        "protected flash configuration, drives its fan-speed relay outputs (P6, P7, P9, P10 high "
        "0x02; P26 low 0x00) and button inputs (P1, P8, P14 pull-up 0x3C), updates its bound BLE "
        "localkey, and completes GAPM initialization.",
    "BK7238 Zengge Ledytech XGD-NZ-002 WB43-M5 V1.1 (OS 9.7.1, 20250107) configures PWM/SPI and advertises IOTWF829":
        "A plaintext BK7238 dump from a Zengge Ledytech XGD-NZ-002 ceiling light on the "
        "WB43-M5 V1.1 board (AK001-ZJ21412, 3D_22_20250107_ZIL-BK38 on Beken SDK 3.0.70 "
        "434b394, Zengge SDK 2.9.0 / OS 9.7.1). It completes RF calibration, configures PWM "
        "pads P6/P8 (0x48) and SPI pads P14..P17 (0x48), brings up the BLE 5.2 controller, "
        "stages its 'IOTWF829' Complete Name advertisement in exchange memory, and initializes "
        "the BRMT remote task.",
    "BK7238 eWeLink W105SE0X door/window sensor (1.0.4, SDK 3.0.70) configures reed/button GPIOs and scans":
        "A plaintext BK7238 dump from an eWeLink (CoolKit) door/window sensor "
        "(CK-BK7238-W105SE0X-01-DOOR-HB, firmware 1.0.4 built May 16 2025 on Beken SDK 3.0.70 / "
        "OSK F-3.0.44). It prints its CoolKit version banner and DeviceMac d02704434298, "
        "configures reed-switch and tamper/button inputs P7, P22 and P23 with pull-ups (0x3C) "
        "and drives status output P21 low (0x00), completes RF calibration, and enters Wi-Fi "
        "station scanning.",
    "BK7238 Zengge W43-M5 V1.2 ceiling light (OS 9.7.1, 20250724) configures PWM/SPI and advertises IOTWF615":
        "A plaintext BK7238 dump from a Zengge W43-M5 V1.2 ceiling light (AK001-ZJ21412, "
        "3D_23_20250724_ZIL-BK38-M5-12 on Beken SDK 3.0.70 17e8b67, Zengge OS 9.7.1). "
        "It completes RF calibration, configures PWM pads P6/P8 (0x48) and SPI pads P14..P17 "
        "(0x48), brings up the BLE 5.2 controller, stages its 'IOTWF615' Complete Name "
        "advertisement in exchange memory, and checks its FOTA firmware version record.",
    "BK7238 Zengge ZJ21413 Symphony USB SPI LED (OS 8.42.4, 20240423) configures SPI and advertises IOTWF8A9":
        "A plaintext BK7238 dump from a Zengge AK001-ZJ21413 Symphony USB SPI addressable LED "
        "controller (77_24_20240423_ZG-BK on Beken SDK 3.0.70 ecb1fb4, Zengge OS 8.42.4). "
        "It completes RF calibration, configures SPI pads P14..P17 (0x48) and inputs P6/P8 "
        "(0x3C), brings up the BLE 5.2 controller, stages its 'IOTWF8A9' Complete Name "
        "advertisement in exchange memory, and initializes the BRMT remote task.",
    "BK7238 Zengge ZJ21412 BP101 RGBCCT bulb (BP5758D I2C, 20240425) boots OS 8.43.0 and calibrates":
        "A plaintext BK7238 dump from a Zengge AK001-ZJ21412 RGBCCT bulb "
        "(35_243_20240425_ZG-BK38-BP101 on Beken SDK 3.0.70 c063598, Zengge OS 8.43.0) "
        "using a BP5758D 5-channel I2C constant-current LED driver on P24 and P26 (both 0x02). "
        "It prints its Zengge banner and BP5758D channel currents (8/8/8/30/30), updates "
        "PWRRCD store index 23, and completes RF calibration.",
    "BK7238 DX-Smart DX-WF24A AT module prints 'power on' and advertises 'WF24-BLE'":
        "A plaintext BK7238 dump from a DX-Smart DX-WF24A AT-command Wi-Fi + BLE combo module. "
        "It suppresses SDK boot messages, prints 'power on', drives P6 and P26 low (0x00), "
        "initializes the BLE 5.2 controller, and stages its 'WF24-BLE' Complete Name "
        "advertisement in exchange memory at 0x0040e480 +0x0a54.",
    "BK7238 BrizLabs Fairy LED RGB (SDK 3.0.65, 1.0.1) configures SPI/GPIO and scans for tuyatest":
        "A plaintext BK7238 dump from a BrizLabs Fairy RGB LED string light (sxebObqQRfXmgU, "
        "firmware 1.0.1 on Beken SDK 3.0.65 / OSK F-3.0.40). It completes RF calibration "
        "(lpf_i/q 74/76, xtal 23), configures SPI pads P14..P17 (0x48), button P6 (0x3C) and "
        "outputs P22/P28, loads its factory test credentials ('tuyatest' / 'fakepassword', "
        "MAC 44:4a:d6:0c:9e:14), computes its PSKC, and enters Wi-Fi station scanning.",
    "BK7238 Gledopto GL-SPI-206W SPI LED controller (TuyaOS 3.11.11) configures SPI pads and fast-connects":
        "A plaintext BK7238 QIO dump from a Gledopto GL-SPI-206W addressable SPI LED strip "
        "controller running TuyaOS 3.11.11 (built Mar 5 2025 for T1). It runs the Tuya OTA "
        "bootloader check (tuya_upgrade_main [255]), completes RF calibration, configures SPI "
        "pads P14..P17 (0x348/0x34A) for the addressable pixel strip, loads MAC 80:64:7c:99:dd:7f, "
        "and initiates a fast-connect scan for its stored 'DLINK_FastNet' network.",
    "BL2028N XiaoMei WP8621N fish feeder (AliOS Things, WP8621NK_03) boots BLE and LinkKit":
        "A plaintext BL2028N dump from a XiaoMei WP8621N automatic fish feeder (WIFI002_V20 "
        "board, APP: WP8621NK_03, built on SDK 3.0.65 with AliOS Things AOS-R-1.3.4 and LinkKit "
        "2.3.0_FY_1.6.0). It exercises the boot_handlers.S `ldmfd sp!, {r0, r1}^` (0xe8fd0003) "
        "exception entry normalization, completes RF calibration, initializes the RW BLE 5.x "
        "stack, loads its VSON LinkKit credentials (product_key a1c3mFpE63X, MAC 86:21:02:FC:22:41), "
        "turns on the status LED, and reports calibration = 1365.",
    "BL2028N Dreo WH517S wall heater (PAI-053, 3.8.7) boots BLE stack and polls MCU":
        "A plaintext BL2028N dump from a Dreo WH517S wall heater (PAI-053 module) running "
        "firmware 3.8.7 (built Oct 13 2025 on SDK 3.0.46, MAC 00:1c:c2:a6:a8:07, XTAL=86). "
        "It completes RF calibration, brings up the RW BLE 5.x stack (gapm_cmp_evt:BLE_STACK_OK), "
        "prints its DREOsh17 product banner and license serial number, transmits 55 AA protocol "
        "frames to the heater MCU over UART1, and enters Wi-Fi station scanning.",
    "BL2028N Dreo WH517S wall heater (PAI-053, 3.4.3) boots BLE stack and polls MCU":
        "A plaintext BL2028N dump from a Dreo WH517S wall heater (PAI-053 module, firmware "
        "3.4.3 built Oct 23 2024 on SDK 3.0.46). It completes RF calibration, brings up the "
        "RW BLE 5.x stack (gapm_cmp_evt:BLE_STACK_OK), prints its DREOsh17 product banner and "
        "license serial number, transmits 55 AA protocol frames to the heater MCU over UART1, "
        "and enters Wi-Fi station scanning.",
    "BK7252 Pix-Link A9 camera (QFN48, RW101P_v19_1010) probes SDIO and hosts PIXA-180431-XLPNL AP":
        "A BK7252 (QFN48) dump from a Pix-Link A9 mini camera (CYCAM_RW101P_v19_1010). "
        "It silences UART console output via rt_console_set_device(\"no_uart\"), probes for an "
        "SD card on P14..P19 via the 0x00802d00 SDCard controller, configures P7 as a pull-up "
        "input (0x3C), and brings up the open 'PIXA-180431-XLPNL' Soft-AP (90:91:64:0b:56:77) "
        "at 192.168.169.100, assigning 192.168.169.101 over DHCP and replying to ARP and ICMP ping.",
    "BK7252 A10 CYCAM (QFN68, VM101P_v85_0729) probes SDIO and hosts PIXA-077260-DNGUV AP":
        "A BK7252 (QFN68) dump from an A10 CYCAM mini camera (LV-MW102P-V1.3 board, "
        "VM101P_v85_0729). It silences UART console output via rt_console_set_device(\"no_uart\"), "
        "probes for an SD card on P14..P19 via the 0x00802d00 SDCard controller, checks "
        "SCTRL_USB_PLUG_WAKEUP (0x00800084 bit 21) and SARADC, and brings up the open "
        "'PIXA-077260-DNGUV' Soft-AP (90:91:64:08:54:80) at 192.168.169.100, assigning "
        "192.168.169.101 over DHCP and replying to ARP and ICMP ping.",
    "BK7252 T99 MiniCam (QFN48, GC0311) probes SDIO and hosts EEE-196629-TLESS AP":
        "A BK7252 (QFN48) dump from a T99 MiniCam with a GC0311 DVP sensor (CYCAM SDK). "
        "The release build silences UART console output via rt_console_set_device(\"no_uart\"), "
        "probes for an SD card on P14..P19 through the 0x00802d00 SDCard controller, checks "
        "SCTRL_USB_PLUG_WAKEUP (0x00800084 bit 21) and SARADC, and brings up the open "
        "'EEE-196629-TLESS' Soft-AP at 192.168.169.100, assigning 192.168.169.101 over DHCP "
        "and replying to ARP and ICMP ping.",
    "BK7252 Naxclow A9 QFN68 camera (20230509, 1.1.20) boots audio, WDT and reads device ID 0800C0140548":
        "A BK7252 (QFN68) dump from a Naxclow A9 camera (RT-Thread 3.1.0 built May 9 2023). "
        "Unlike the Aug 2024 QFN48 MiniCam build, this earlier QFN68 image reports 'No TLV header "
        "found in flash', calibrates RF with tssi_th b-120/g-115, starts its watchdog device, "
        "registers the station and soft-AP WLAN interfaces, configures audio DAC/ADC volumes "
        "(65/80), initializes tc_entity_init, and reads device ID 0800C0140548 from flash.",
    "BK7252 A9 camera 20211124 (QFN68, SDK 3.0.33, UART1 log) calibrates RF and initializes WLAN":
        "A BK7252 (QFN68) dump from an A9 camera on Beken SDK 3.0.33 (OSK R-3.0.22, IP "
        "W4-3.0.33-P0, RT-Thread 3.1.0 built May 14 2022) that routes its console log to UART1 "
        "instead of UART2. It completes RF calibration (tssi b-125/g-115, xtal_cali 25), "
        "initializes lwIP 2.0.2 and both WLAN interfaces, runs drv_pm_init, and reaches msh />.",
    "BK7252 Naxclow MiniCam A11-A9 B-V2.7 (1.1.20, 48P) boots audio, WLAN and Naxclow camera app":
        "A BK7252 (QFN48) dump from a Naxclow MiniCam on the A11-A9 B-V2.7 (240426) board "
        "(RT-Thread 3.1.0 built Aug 7 2024). It completes RF calibration (tssi_th b-130/g-123), "
        "initializes lwIP 2.0.2 and the station/soft-AP WLAN interfaces, configures audio DAC/ADC "
        "volumes (65/80) and board GPIOs, and launches com.naxclow.Camera_v0-1.1.20 for "
        "DevType A9_V2.7_ALERT_A_48P.",
    "BK7252 Linklemo XCThings HA15 IN-A15-V1.0 camera (12.12.16) boots FAL/ROMFS, jpegdec and vbat_detect_test":
        "A BK7252 (QFN48) dump from a Linklemo XCThings HA15 camera on the IN-A15-V1.0 (INNOA15 "
        "Blue PCB) board, firmware 12.12.16 (RT-Thread 3.1.0 built Apr 1 2024). It completes RF "
        "calibration (tssi_th b-145/g-130, BLE ch0/19/39 txpwr table), mounts the FAL partition "
        "table and ROMFS, creates the jpegdec thread, runs vbat_detect_test on SARADC channel 0, "
        "and initializes tc_entity_init.",
    "BK7252N XCThings Linklemo A9 INO-IPC-V2.3 camera loads PPH0A90E1F42CF3350 identity and starts vtask":
        "A BK7252N dump from an XCThings Linklemo A9 camera built on the INO-IPC-V2.3 (Blue PCB) "
        "board. It initializes FAL and ROMFS, configures SDIO pads P14..P19 (0x378), buttons "
        "P12/P13 (0x3C) and outputs P22/P23 (0x02), reads its distinct [iot] device credentials "
        "(did PPH0A90E1F42CF3350, scode 574818, MAC c8:47:8c:70:a1:b2) from flash, and starts "
        "AVSDK 3.00.42.01, OSAL 1.00.01.64 and the vtask_wor worker thread.",
    "BK7252N Pinmei A9 camera (SPI NeoProgrammer dump, RTC 8:3:05) loads xciot identity and spawns xrpc threads":
        "An external SPI programmer dump (NeoProgrammer EB6015) of the Pinmei A9 BK7252N camera "
        "captured at a later flash RTC timestamp (2025-03-01 00:03:05 UTC). It initializes FAL "
        "and ROMFS, restores its persisted [iot] credentials and MAC c8:47:8c:4c:81:2c, keeps P22 "
        "and P23 driven high (0x02), and brings up AVSDK 3.00.42.01 and the xrpc worker threads.",
    "BK7252N Pinmei A9 camera (BKFIL, power on) loads xciot identity and spawns xrpc threads":
        "A BK7252N dump from a Pinmei A9 camera captured via BKFIL with power status on in flash. "
        "Unlike the powered-off camera dumps (which enter deep sleep), this unit keeps P22 and P23 "
        "driven high (0x02), starts xled_start, reads its [iot] device credentials (did "
        "PPH0A906615F0207B0, scode 760441, prod.glbs.xcthings.com, MAC c8:47:8c:4c:81:2c), "
        "initializes AVSDK 3.00.42.01 and OSAL 1.00.01.64, and spawns the vtask_wor, unpack, "
        "xrpc.r and xrpc.w worker threads.",
    "BK7252N INO-A13N-V2.2 square camera (3.00.21.01_250512) restores RTC 2025-3-1 and enters deep sleep":
        "A BK7252N dump from an INO-A13N-V2.2 square battery camera running an earlier xciot "
        "build (sw_ver=3.00.21.01_250512 vs 250723). It initializes FAL and ROMFS, configures "
        "SDIO pads P14..P19 (0x378), buttons P12/P13 (0x3C) and drives P22/P23 low (0x00) on "
        "sleep entry after reading SARADC channel 0 (battary_status:139), xdev_opt "
        "(magic=11223344) and setting the local clock to 2025-3-1 8:1:50.",
    "BK7252N CY365 Car IP camera 1.7.0 (--xvr-selfclear) completes RF cal and probes as station":
        "A BK7252N dump from a CY365 Car IP camera (EEE-249530-GDVHL, firmware 1.7.0). Without "
        "--xvr-selfclear it spins at 0x0d4ba4 waiting for bit 31 of XVR register 0x00900000 to "
        "self-clear. With --xvr-selfclear it completes RF calibration, configures SDIO pads "
        "P14..P19 (0x378), button P7 (0x3C) and output P5 (0x02), transitions the Wi-Fi MAC RX "
        "filter from monitor mode (0x7fffffde) to normal reception (0x3503848c), and begins active "
        "station probing.",
    "BK7231U Arnoo Winees AiDot RGBW A60 bulb (Leedarson CC8000 2.0.4.51) stays in Wi-Fi monitor pairing":
        "The newer V2.3.72::V2.04.51 firmware dump from the Arnoo Winees WS010078261 AiDot RGBW "
        "A60 (2nd Gen, light.A001139) smart bulb on a Leedarson CC8000 BK7231U module. Unlike the "
        "earlier 2.0.3.47 build (which immediately switches from monitor mode to its SmartLight "
        "Soft-AP), 2.0.4.51 remains in Wi-Fi MAC monitor/sniffing mode (RX filter 0x7fffffde) "
        "after bringing up rw_main and configuring P2, P3, P20 and P21.",
    "BK7231U Arnoo Winees AiDot RGBW A60 bulb (Leedarson CC8000 2.0.3.47) hosts SmartLight-5783 AP and answers ping":
        "An earlier V2.2.18::V2.03.47 firmware dump from an Arnoo Winees WS010078261 AiDot RGBW "
        "A60 (2nd Gen, light.A001139) smart bulb on a Leedarson CC8000 BK7231U module. It starts "
        "its open pairing Soft-AP 'SmartLight-5783' (1c:d6:bd:d4:57:82, channel 7), assigns "
        "192.168.10.2/24 to the emulated station from router 192.168.10.1 via DHCP, and replies "
        "to ARP and ICMP ping while configuring P2, P3, P20, P21 and P23.",
    "BK7231U Arnoo AiDot RGBW A19 bulb (Leedarson CC8000 2.0.4.51) enters Wi-Fi monitor pairing":
        "A BK7231U dump from an Arnoo AiDot RGBW A19 (2nd Gen, light.A001126) smart bulb on a "
        "Leedarson CC8000 module (V2.3.72::V2.04.51). It opens the Wi-Fi MAC RX filter "
        "(0x7fffffde) into monitor/sniffing mode, prints its CC8000 version banner, configures "
        "P2, P3, P20 and P21 as pull-up inputs, and starts the RivieraWaves rw_main task.",
    "BK7231U C-Chip CC8000 HLK-B30 dev board boots RT-Thread 3.1.0 and FAL partitions":
        "A BK7231U dump from a C-Chip CC8000 / Hi-Link HLK-B30 development board running RT-Thread "
        "3.1.0. It reads its factory RF calibration parameters from flash (temp 280, crystal offset "
        "48), initializes lwIP 2.0.2, registers station and soft-AP WLAN interfaces, and mounts the "
        "RT-Thread FAL V0.4.0 partition table.",
    "BK7231U C-Chip C-8133U RT-Thread 3.1.0 firmware mounts FAL partitions":
        "An RT-Thread 3.1.0 build from the C-Chip C-8133U BK7231U directory (contrasting with the "
        "AliOS Things dumps from the same board). It completes RF calibration, brings up lwIP 2.0.2, "
        "registers station and soft-AP WLAN interfaces, and initializes the RT-Thread Flash "
        "Abstraction Layer (FAL V0.4.0) partition table.",
    "BK7231U C-Chip C-8133U AliOS LED driver (reset_counts:1) configures 500 Hz PWM without resetting":
        "A companion BK7231U C-Chip C-8133U AliOS Things dump captured with reset_counts:1 in flash. "
        "It configures all five PWM pads (P7, P8, P9, P24, P26), programs PWM0 for a 500 Hz period "
        "(0xCB20 ticks at 26 MHz), starts its Wi-Fi scan, and confirms the 3x reset handler is not "
        "triggered when reset_counts is below 3.",
    "BK7231U C-Chip C-8133U AliOS LED driver (reset_counts:3) configures 5ch PWM and activates reset":
        "The first BK7231U dump in the suite: a C-Chip C-8133U 5-channel RGB+CCT LED driver "
        "running AliOS Things. It restores its persisted light parameters (50% brightness, "
        "4500 K, Red=255), configures all five PWM pads (P7, P8, P9, P24, P26) into second-"
        "function mode, initiates a Wi-Fi scan, and fires its three-cycle reset handler.",
    "BK7231Q Tuya MOES Relay 1.0.4-2 (rst_cnt:2) drives relay high and scans":
        "A second BK7231Q MOES WA2 one-gang relay unit on firmware 1.0.4 captured with rst_cnt:2. "
        "Before manufacturing-test override, its default power-on state drives P18 high (relay "
        "closed), configures P6 as a pull-up button input and P7 low, and initiates a Wi-Fi scan.",
    "BK7231Q Tuya MOES Relay 1.0.4 (SDK 2.0.0) maps GPIO and enters product test":
        "The 1.0.4 factory firmware of the BK7231Q MOES WA2 one-gang relay on Tuya IoT SDK 2.0.0 "
        "(rst_cnt:1). It configures P18 as an active-high relay output and P6 as a pull-up button "
        "input, completes SARADC calibration, scans the virtual tuya_mdev_test1 AP, and enters "
        "product test mode.",
    "BK7231Q Tuya MOES Relay 1.0.6 (SDK 2.0.0) scans factory SSID and enters product test":
        "A factory-state BK7231Q MOES WA2 one-gang relay dump on Tuya IoT SDK 2.0.0 (version "
        "1.0.6). With SOC_BK7231 SARADC FIFO_EMPTY bit 28 reported when drained, it completes "
        "SARADC calibration and Wi-Fi scanning against the virtual tuya_mdev_test1 AP, measures "
        "RSSI -45 dB, and enters product test mode.",
    "BK7231Q Tuya MOES Relay 1.1.3-2 completes 3x-reset WDT reboot and enters EZ pairing":
        "A BK7231Q MOES WA2 one-gang relay dump captured after two fast power cycles "
        "(fsw_cnt_key=2). On first boot it reaches the three-cycle factory-reset threshold, "
        "clears the counter in flash and reboots through the hardware watchdog; on the second "
        "boot it reads back fsw_cnt_key=0, drains the SOC_BK7231 SARADC FIFO (bit 28), and "
        "brings up EZ-mode Wi-Fi pairing.",
    "BK7231M SparkleIoT XH-CB2S Matter mini switch boots into Matter init":
        "A 4 MB plaintext BK7231M dump from a SparkleIoT XH-CB2S mini switch built on Quectel's "
        "Matter SDK (svc_quec_matter) and Beken SDK 3.0.56. It boots the Beken RTOS on UART1, "
        "enters the Matter application ('going Chip') and runs its network-config and log-flag "
        "initialization on UART2.",
    "BK7231M ChinaMobile HeJiaQin X1S plug joins JX-ChanCe-M2 and starts BL0937 calibration":
        "A plaintext BK7231M China Mobile HeJiaQin (HuiJu X1S) CB2S metering plug on Beken SDK "
        "3.0.56. With the BK7231N chip identity and SARADC_ADC_CTRL_CFG trigger modeled, it "
        "completes RF calibration and BLE 5.1 startup, loads its factory CMEI/SN burninfo, "
        "associates with the virtual 'JX-ChanCe-M2' factory AP over DHCP, and enters BL0937B "
        "power-meter calibration.",
    "BK7231M VeSync WHOGPLUG Plug 1.0.02 (SDK 3.0.56) calibrates, advertises BLE and scans":
        "The 1.0.02 firmware revision of the VeSync/GreenSun WHOGPLUG EU smart plug on Beken SDK "
        "3.0.56. Unlike 1.0.01, this build enforces the BK7231N chip-ID check and exercises the "
        "SARADC_ADC_CTRL_CFG trigger during RF calibration before starting FlashDB, BLE 5.1 "
        "advertising, pwm_new pulse capture and Wi-Fi scanning.",
    "BK7231M VeSync Aubess BSD33 BSDOG02 plug (SDK 3.0.56) calibrates, advertises BLE and scans":
        "A plaintext BK7231M VeSync/Aubess BSD33 (BSDOG02) smart-plug dump on Beken SDK 3.0.56. "
        "It enforces the BK7231N chip-ID check and pulses SARADC_ADC_CTRL_CFG bit 8 during RF "
        "calibration before mounting its FlashDB KV partitions, bringing up BLE 5.1 advertising, "
        "configuring pwm_new metering capture and starting a Wi-Fi scan.",
    "BK7231T Tuya BN-LINK WB2S metering plug boots its relay application":
        "A newly added stock BN-LINK WB2S metering-plug dump on Tuya SDK 2.0.0. It recovers its "
        "protected configuration, validates the licensed product key, initializes the dltj energy-"
        "meter service and relay channel, and brings the BLE database up successfully.",
    "BK7231T Tuya one-gang WB3S-IPEX switch loads its hardware map":
        "A newly added stock one-gang wall-switch dump on Tuya SDK 1.0.1. Its recovered board "
        "configuration is consumed by the application and maps the relay, physical button and "
        "Wi-Fi status LED to the expected pins before BLE advertising starts.",
    "BK7231N Tuya LSC GLS A60 CBLC5 joins and authenticates cloud MQTT":
        "A paired LSC RGB+CCT bulb on TuyaOS 3.3.44 that previously spun after HCI callback "
        "registration. Its real BK7231N selection now brings BLE up automatically, joins the "
        "stored network and exercises Tuya's older mode-2 identity-as-PSK bootstrap. The offline "
        "peer authenticates the encrypted Finished before returning server Finished, CONNACK and "
        "SUBACK, and captures the bulb's first encrypted PUBLISH without logging its credential.",
    "BK7231N Tuya CB3S IR module reaches production-monitor endpoint":
        "A factory CB3S dump carrying Tuya's IR-box application. Its real BK7231N identity "
        "selects BLE hardware automatically, with no BLE/XVR workaround flags. The stock image "
        "completes GAPM and advertising, initializes its IR services, starts its SoftAP, enters "
        "the MAC's monitor mode and reaches the same wifi_stat:1 endpoint observed on hardware. "
        "Its capture proves the production beacon/probe exchange and the firmware's periodic raw "
        "authentication frame carrying the vendor payload 'filter'.",
    "BK7231N Tuya 72-087 CB3S switch finds factory network and enters product test":
        "A factory-state CB3S one-channel switch that previously stopped at its scan request. "
        "The emulated air now supplies the exact Tuya manufacturing SSID; the stock firmware "
        "measures its RSSI and takes the real product-test branch.",
    "BK7238 Tuya GBDQA protection switch starts metering, BLE and cloud TLS":
        "A paired BK7238 GBDQA protection switch that previously stopped at app_init. With its "
        "BLE 5.2 controller and network peer active, it synchronizes the host stack, starts the "
        "bistable relay and metering application, rejoins its stored network, authenticates with "
        "the dump's recovered PSK3, and completes MQTT CONNECT, SUBSCRIBE and its first QoS1 "
        "PUBLISH against the offline cloud broker.",
    "BK7238 Tuya T1-U PCIe card serves its BLE/Wi-Fi pairing interfaces":
        "A three-channel BK7238 PCIe remote-boot card that previously stopped after loading its "
        "board map. It now completes BLE 5.2 startup, publishes its Tuya service, raises the open "
        "SmartLife pairing AP, leases an address to the virtual station and answers its ping.",
    "BK7252N Xiongmai camera restores config and enters deep sleep":
        "A newly added full-flash BK7252N camera image running RT-Thread and Xiongmai's camera "
        "stack. It initializes flash and ROMFS, brings up WLAN/RF, restores persisted device "
        "configuration, starts both buttons and the battery ADC, then follows its timed deep-sleep path.",
    "BK7231T Tuya three-gang WB3S switch initializes every channel":
        "A newly added stock BK7231T three-gang WB3S wall-switch dump on Tuya SDK 1.0.2. It "
        "completes the classic BLE database and advertising path, restores its three-channel board "
        "map and verifies every relay, button and status LED before device and Wi-Fi initialization.",
    "BL2028N Dogness D07 water fountain starts BLE and water controls":
        "A newly added plaintext BL2028N Dogness D07 smart water-fountain dump. With the BLE core "
        "modeled, it completes controller and host initialization, advertises its product name, "
        "starts its local AP and restores the configured water-level, pump and RGB-control state.",
    "CLI: invalid -key is rejected":
        "A bad -key value must be rejected up front by parse_key(), with a message naming the "
        "accepted forms (a known key name, 32 hex chars, or base64 of 16 bytes), rather than "
        "failing deep inside emulation.",
    "CLI: invalid -chip is rejected":
        "An unknown -chip name must be rejected and the known chip identities listed, so a typo "
        "fails fast instead of emulating with the wrong SCTRL id registers.",
    "CLI: encrypted image without a key warns":
        "Running an encrypted image with no key must warn that the decrypted slice does not look "
        "like ARM code and suggest -key TUYA, instead of silently emulating garbage.",
    "CLI: malformed --adc is rejected":
        "A malformed --adc specification must be rejected up front by parse_adc_specs() with the "
        "expected CH:VALUE syntax, rather than starting emulation with unparsed ADC overrides.",
    "UART1 Command Console: SetPinRole P23 ADC reads --adc P23:2150 into channel 8":
        "End-to-end SARADC input test on BK7231T: --adc P23:2150 sets SARADC channel 3 (P23) to "
        "raw sample 2150, and 'backlog SetPinChannel 23 8; SetPinRole 23 ADC' over the UART1 "
        "console makes PIN_OnEverySecond() read HAL_ADC_Read(23) and publish 2150 to channel 8.",
    "OpenBK7231T_QIO_1.18.300 Boot to MQTT and 1s timers":
        "Full OpenBeken boot on the flagship BK7231T image, then ten Main_OnEverySecond ticks - "
        "proving the timer keeps firing, not just starts. Also proves the tick interrupt, the "
        "FreeRTOS timer daemon and MQTT registration are alive; with no WiFi in the emulator, "
        "MQTT stays disconnected (bWifi 0). Runs on to ~5 device-seconds, where the no-SSID path "
        "opens the fallback access point: the HAL logs its IP config and the MAC-derived SSID "
        "(OpenBK7231T_xxxx), then goes silent inside bk_wlan_start() - the exact edge of what the "
        "emulator models of the WiFi stack.",
    "MathDemo Boot and Float Verification":
        "A special OpenBeken build that runs integer and floating-point math at boot and logs the "
        "results. Verifies the CPU's software-float paths and that FreeRTOS software timers fire "
        "(quicktick); with no config in flash it takes the default-config path.",
    "MathDemo Startup Command: echo":
        "The mathDemo image with a hand-crafted OpenBeken config injected at flash 0x1e1000, whose "
        "startup command is 'echo Test...'. Proves the emulated flash controller serves a full "
        "32-byte page per read and that the config's signed-char Tiny_CRC8 is accepted, then the "
        "echo command runs.",
    "MathDemo Startup Command: uartSendHex on UART1":
        "Same config-injection trick, but the startup command drives OpenBeken's uartSendHex to "
        "place arbitrary bytes on UART1 (the MCU link). Exercises config load -> command "
        "registration -> HAL UART write -> the emulator's UART1 hex capture.",
    "MathDemo Startup Command: TuyaMCU full link, data points and channels":
        "The deepest TuyaMCU case here. The driver opens UART1 itself and talks first, "
        "unprompted: the first per-second tick emits a heartbeat (55 AA 00 00 00 00 FF). A "
        "simulated MCU answers, so OpenBeken completes the whole handshake and then reports one "
        "data point of every wire type - bool, value, string, enum, bitmap. The startup command "
        "binds those data points to channels, so the case checks both directions: a reported DP "
        "must land on its channel, and a channel moved by a change handler must go back out as a "
        "0x06 SET_DP frame. The MCU then speaks unprompted, including a frame with a deliberately "
        "wrong checksum and one behind four junk bytes, to check that bad input is discarded and "
        "the framer resynchronises instead of desyncing.",
    "MathDemo Startup Command: SetPinRole drives relay pin P9":
        "Startup command 'SetPinChannel 9 1; SetPinRole 9 Rel; SetChannel 1 0; SetChannel 1 1'. "
        "Orders OpenBeken to make pin 9 a relay output bound to channel 1, then switches that "
        "channel on. The only case where the expected GPIO register word is known independently "
        "- from the vendor SDK's own gpio_config()/gpio_output() - so it verifies the emulator's "
        "pin capture rather than merely displaying it.",
    "MathDemo Startup Command: SetPinRole drives relay pin P7":
        "The P9 case repeated on a different pin (7) and a different channel (2), with an "
        "assertion that pin 9 stays untouched. Two pins moving independently under command is "
        "what rules out a fixed write landing on a captured index by coincidence.",
    "MathDemo Startup Command: PWM 1400 Hz on pin 9":
        "Startup command 'PWMFrequency 1400; SetPinChannel 9 1; SetPinRole 9 PWM; SetChannel 1 50'. "
        "Asks for a deliberately uncommon 1400 Hz so the expected period (26 MHz / 1400 = 18571) is "
        "a value nothing else produces, then checks the captured PWM registers decode back to that "
        "frequency and to 50% duty - verifying the PWM decode, not just the capture.",
    "MathDemo Startup Command: PWM 2500 Hz on pin 7":
        "The 1400 Hz case with every variable moved: pin 7 instead of 9, PWM index 1 instead of 3, "
        "channel 3, 2500 Hz and 25% duty. Because PWM1's period register sits at a different offset "
        "under the stride-12 layout than it would under stride-8, this case pins down the register "
        "stride itself rather than just re-confirming one frequency.",
    "MathDemo Startup Command: PWM 10 kHz on pins 24 and 26":
        "Two PWM channels driven at once on the highest pads (P24/P26 = PWM4/PWM5) at 10 kHz, with "
        "different duties on each (10% and 75%). Exercises the top of the captured register window "
        "and the first multi-channel PWM_CTL value, where an off-by-one in the channel stride would "
        "push the registers out of range entirely.",
    "BK7231T Tuya Geeni BW223 Filament Bulb (single-colour CW PWM) boots":
        "A real white-only filament bulb on stock Tuya firmware. Its stored config declares "
        "pwmhz:3000, so the emulator's decoded 3000.2 Hz is checked against the device's own claim "
        "rather than against our arithmetic. Drives P24/P26 (PWM4/PWM5), the high channels no other "
        "real-firmware test covers. Its config maps c_pin:26/w_pin:24, so PWM5 is cold and PWM4 warm; "
        "the captured duties (100% and 0%) show the bulb at full cold white.",
    "OpenBK7231N Startup Command: PWM 3700 Hz on pin 8 (pwm_new)":
        "The synthetic ground-truth trick on the BK7231N image: OpenBeken is told to do 3700 Hz at "
        "40% duty on P8, which its HAL programs through the pwm_new block. Both values were "
        "predicted before the first run - period 0x1B73 and a 40% toggle at 0x0AFA - and the duty "
        "is reconstructed by replaying edge toggles, making this the sharpest single check the "
        "N-family decoder has.",
    "OpenBK7231N Startup Command: dual PWM 25%/70% on pins 7 and 9":
        "Two pwm_new channels commanded to different duties (25% and 70%) at 4500 Hz, both on the "
        "sub-channel-1 half of their groups - the slots every stock N dump leaves at 0%, so the "
        "toggle-replay duty decode for them had only ever been checked against zero. Distinct "
        "values on distinct groups also rule out cross-channel register mix-ups.",
    "MathDemo Startup Command: AlwaysHigh and AlwaysLow pins":
        "Startup command 'SetPinRole 14 AlwaysHigh; SetPinRole 15 AlwaysLow'. The two fixed-output "
        "roles drive a pin high and low with no channel involved, on plain GPIOs that have no "
        "second function. Asserting the low pin at 0x00 - a written value, not an absent one - is "
        "what separates 'driving low' from 'never configured'.",
    "BK7231T Tuya A60 RGBCW Bulb (5-channel PWM) boots":
        "A five-channel RGBCW bulb driving PWM0/1/2/4/5 at once, all at 1000 Hz as its stored "
        "config declares. It is the first real firmware to exercise PWM1. PWM2's duty is left "
        "unasserted on purpose: the bulb ramps it during light init, so it reads 100% when the "
        "harness stops and 54.5% in longer runs - a function of boot progress, not an invariant.",
    "BK7231T Tuya Ledvance WW/CW Bulb (PWM3) boots":
        "The only real firmware in the suite driving PWM3, which completes real-device coverage of "
        "all six PWM channels. Its period register sits at the same +0xAC offset the synthetic "
        "1400 Hz case pins down, so a shipped product confirms that offset independently of how we "
        "drive OpenBeken. 2000 Hz, matching its stored pwmhz.",
    "BK7231T Tuya ROOMLUX A60 RGBCW Bulb (600 Hz PWM) boots":
        "Chosen for its period arithmetic: 26 MHz / 600 truncates to 43333, which decodes back to "
        "600.0046 Hz rather than 600, so the case pins the integer truncation instead of a rounded "
        "frequency. Also the first five-channel PWM_CTL cross-check - 0x110111 names exactly the "
        "channels decoded from the period registers.",
    "BK7231T Tuya TreatLife RGB LED Strip (PWM3/4/5) boots":
        "A strip controller driving r/g/b through P9/P24/P26 = PWM3/4/5 - the only real device on "
        "the upper half of the channel map, where every bulb clusters on P6/P7/P8. 1000 Hz, "
        "matching its stored pwmhz. Its oem build (strip_ty 1.0.5, Apr 2020) is the oldest light "
        "firmware in the suite.",
    "BK7231N Tuya bulb (paired): joins its stored network and reaches the Tuya cloud":
        "The bulb above once the emulator stops it stalling: it turns out to be a paired device "
        "that joins the network name still in its flash and calls its cloud. It is the second "
        "device here to confirm how Tuya secures that link, with a different SDK and a longer "
        "cipher list but still no certificate and no server name, and it stops at the same point "
        "for the same reason, having no key of its own to present.",
    "BK7231N Tuya Feconn MR16 RGBCT Bulb (pwm_new ch3) boots into its app":
        "Completes N-family channel coverage: its cool channel sits on P9 = pwm_new ch3, the one "
        "channel the other three N lights leave idle. Group 1's CTRL enables only sub-channel 1 "
        "here, so it also proves the decoder honours per-sub enable bits. IOT SDK 2.3.3 - the "
        "oldest N-family light firmware in the suite.",
    "BK7231N Tuya Gleco Bulb (5ch pwm_new, 5 kHz) boots":
        "Third N-family light at a third frequency: 5 kHz, period 5200 matching its stored "
        "pwmhz:5000. Together with Arlec (1 kHz) and the Plafon (16 kHz), the pwm_new decode is "
        "corroborated across a 16x frequency span, with channel maps running in both directions.",
    "BK7231N Tuya LED RGB Plafon (5ch pwm_new, 16 kHz) boots":
        "Five BK7231N pwm_new channels at once at 16 kHz - the highest PWM frequency in the suite "
        "and the only case exercising all three pwm_new register groups simultaneously. Period "
        "1625 = 26 MHz / 16000 exactly, matching its stored pwmhz:16000.",
    "BK7238 Tuya Onforu 100W CCT Floodlight (pwm_new 2-toggle CW, 256B flash page write) boots":
        "The first BK7238 light in the suite that drives hardware PWM (TuyaOS 3.11.24, T1-U-HL "
        "1.0.43, plaintext). Its stored Tuya config maps warm to P6 (PWM0) and cold to P26 (PWM5) "
        "at 1000 Hz, and PWM0 uses two non-zero edge toggles (T1=0x171B, T2=0x4E75 -> 54.5% duty) "
        "- the first real firmware exercising two-edge toggle replay in pwm_new_channels(). Also "
        "guards BK7238's 256-byte flash page-write FIFO at 0x00803024 (REG_FLASH_PW_CONF), without "
        "which 4K sector writes fail verification and reboot, and asserts [BLE_ADV] exchange-memory "
        "extraction on the BLE 5.2 core (UUID 0xFD50 carrying keytfesxxsqewnqy).",
    "BK7238 Tuya S06Pro IR + TempHum (TuyaMCU + BLE 5.2 + Wi-Fi AP) handshakes and reports AP state":
        "The first BK7238 TuyaMCU firmware in the suite, and a new device class: an S06Pro IR "
        "blaster + temperature/humidity station (t1_common_user_config_ir_ty 1.0.1, TuyaOS 3.11.16, "
        "T1-3S, plaintext). Exercises all three emulated peripheral/radio models on BK7238 in one "
        "run - UART1 TuyaMCU (0x00/0x01/0x02 handshake with licensed id jbe3snv4tki8oo9c), the "
        "802.11 MAC + virtual station joining its SmartLife-C8F2 pairing AP (DHCP, ping, Tuya LAN "
        "3.5 broadcast), and the BLE 5.2 core staging its UUID 0xFD50 beacon - and shows the "
        "firmware report Wi-Fi state 0x01 (AP config mode, 55 aa 00 03 00 01 01 04) back to the MCU "
        "once both radios are up.",
    "BK7238 Tuya IHSENO HAZ-IR-V3 IR Blaster (paired): joins stored network, BLE 5.2 adv, TLS-PSK cloud handshake":
        "The first paired BK7238 dump that joins its stored network ('DOM-1') as a Wi-Fi station and "
        "reaches for the Tuya cloud (oem_t1_irbox_mol_ty:0.0.5 on TuyaOS 3.8.31, T1-3S, plaintext). "
        "Because this unit is already bound ('already bind', psk30_key stored in KV), it connects "
        "straight to its MQTT-over-TLS broker (m3-eu.lifeaiot.com:8883). The simulator recovers the "
        "dump's own PSK3 internally, verifies the encrypted TLS Finished, and serves its real MQTT "
        "CONNECT, SUBSCRIBE and first QoS1 PUBLISH from the offline broker. Also covers fast-connect "
        "fallback + FCI flash persistence, bound BLE 5.2 advertising (0x4B flag byte), and the "
        "post-connect IR/button/Wi-Fi-LED GPIO state.",
    "Berry Startup Command: print(\"Hello \" + str(5+2*2))":
        "Pulls OpenBeken's dedicated 1.18.302 Berry build and runs a one-line Berry script from the "
        "startup command: berry print(\"Hello \" + str(5+2*2)). Exercises the embedded VM end to "
        "end - arithmetic precedence (=9), str(), string concatenation and print() - which surfaces "
        "as Info:BERRY:Hello 9.",
    "UART1 Command Console: echo runs, unknown command rejected":
        "Bidirectional UART1 - the receive counterpart of the uartSendHex case. The injected config "
        "sets OBK flag 31 (OBK_FLAG_CMD_ACCEPT_UART_COMMANDS), starting a command console on UART1. "
        "--uart1-rx feeds two newline-separated lines into the receive FIFO once the console's RX "
        "callback is up: 'echo UartRxConsoleOK' then 'nosuchcmd_uarttest'. OBK runs the first "
        "(echoing the argument) and rejects the second as unknown (Error:CMD:cmd ... NOT found) - "
        "proof the full RX path works and a real command parser, not a byte reflector, is running.",
    "UART1 Command Console: setChannel arg 12+28 evaluated to 40":
        "Proves OBK evaluates a command argument as an expression, over UART1. --uart1-rx injects "
        "'setChannel 5 12+28'; OBK's tokenizer resolves 12+28 to 40 before the setChannel handler "
        "runs, so channel 5 is set to 40 - not rejected as non-numeric, nor truncated to 12. The "
        "'CHANNEL_Set channel 5 has changed to 40' log line carries the resolved value.",
    "UART1 Command Console: backlog runs two commands from one line":
        "Closes the multi-command loop over UART1. A plain console line is one command (OBK does "
        "not split on ';'), so multiple commands need the backlog prefix. --uart1-rx injects "
        "'backlog echo BacklogOne; echo BacklogTwo'; backlog splits on ';' and runs both echoes, "
        "so both arguments come back - proof the console parses and dispatches several commands "
        "from one received line.",
    "OpenBK7231U_QIO_1.18.300 Boot to 1s timers":
        "OpenBeken on the BK7231U variant from a plaintext (no-key) image, booting through to the "
        "per-second timer - confirms the shared BK7231 model and the no-decrypt path.",
    "OpenBK7238_QIO_1.18.300 Boot to 1s timers":
        "OpenBeken on the newer BK7238 silicon (-chip BK7238). Boots through the SPI flash mirror "
        "and the crypto/XVR peripherals to the per-second timer, checking BK7238 chip-identity gating.",
    "OpenBK7231N_QIO_1.18.300 Boot to 1s timers":
        "OpenBeken on BK7231N. This variant blocks in the per-second temperature read waiting on "
        "the SARADC interrupt (ICU bit 11), so reaching the 1s timer proves the emulated SARADC "
        "FIFO + interrupt model works.",
    "OpenBK7231M_QIO_1.18.300 Boot to 1s timers (no key)":
        "OpenBeken on BK7231M from a plaintext image (no key). Confirms the shared model and "
        "no-key path reach the per-second timer.",
    "OpenBK7252_QIO_1.18.300 Boot to 1s timers":
        "OpenBeken on BK7252 (BK7221U silicon, -chip BK7252). One of the heavier boots; reaches "
        "the per-second timer.",
    "OpenBK7252N_QIO_1.18.300 Boot to 1s timers":
        "OpenBeken on BK7252N (-chip BK7252N). The heaviest boot in the suite; reaches the "
        "per-second timer.",
    "BK7238 Sonoff 4MB Dump Boots (SPI mirror + XVR)":
        "A real 4MB Sonoff flash dump on BK7238. Exercises the SPI flash mirror and the XVR "
        "peripheral through the vendor SDK boot to calibration / normal-mode markers.",
    "BK7231Q Tuya MOES Relay Boot":
        "A Tuya BK7231Q relay dump that initializes its GPIO/datapoint map, returns from TCP/IP "
        "stack initialization and actually schedules the Wi-Fi supplicant and core tasks before "
        "waiting normally for external manufacturing/provisioning input.",
    "BL2028N Dreo fan clears BLE gate and serves its pairing AP":
        "A BL2028N (BK7231N-class) Dreo fan dump that used to stop at the BLE-controller "
        "handoff. With the BLE core model enabled, the stock firmware completes HCI setup, "
        "publishes its DREO BLE name, starts its open provisioning AP, leases an address to the "
        "virtual station and answers its ping.",
    "Tuya TMWF02 TuyaMCU Boots past crypto accel":
        "A stock Tuya fan-switch dump. Boots past the 0x810000 crypto-accelerator stall into the "
        "BLE host stack and network-config advertising.",
    "CLI: malformed --net-forward is rejected":
        "A port forward that is not LPORT:DPORT must be refused with a one-line message before "
        "anything boots.",
    "OpenBK7231T Wi-Fi AP: virtual station joins, DHCP, ping, HTTP command runs, panel on localhost":
        "The emulated Wi-Fi path end to end: the RivieraWaves MAC is modelled, a virtual station "
        "joins OpenBeken's open AP, takes a DHCP lease from it, pings it and sends an HTTP GET to "
        "its command API - the command runs in the firmware and its reply comes back over the air. "
        "Then the harness opens the device's web panel through a real port forward on 127.0.0.1, "
        "the way a browser does, and checks the page is the real panel.",
    "OpenBK7231T Wi-Fi station: joins virtual AP, DHCP, HTTP in, MQTT out captured":
        "OpenBeken configured for an open network joins the virtual access point, gets its address "
        "from the virtual network, answers an HTTP command coming in, and its outgoing MQTT "
        "connection is resolved to a sinkhole and decoded instead of leaving the machine.",
    "OpenBK7231T Wi-Fi station: joins a WPA2-protected network and uses it":
        "The emulated access point could only ever be an open network, so a device that had "
        "stored a protected one could see it and never join. Here it offers a protected network "
        "and the device joins it properly: both ends already hold the passphrase, the four-way "
        "handshake is how each proves that, and the link stays shut until it completes. The "
        "address, the clock and the outgoing request all happen on the far side of it.",
    "OpenBK7231T Wi-Fi station: outgoing NTP, HTTP GET and MQTT answered offline, pcap holds them":
        "Everything the device starts itself, answered without a real network: its NTP query gets "
        "the host clock, its repeating HTTP GET to a cloud-style name gets a 200 from the offline "
        "responder each time, and its MQTT client completes CONNECT, SUBSCRIBE and PUBLISH against "
        "the offline broker until the firmware reports the session up. The broker then pushes a "
        "command back to the device on a topic the firmware subscribed to itself, and the "
        "firmware runs it - the device driven over MQTT, not merely listened to. The 802.11 "
        "capture file written alongside is parsed back and must hold the same exchange in both "
        "directions.",
    "OpenBK7238 Wi-Fi AP: virtual station joins, DHCP, ping, HTTP command runs":
        "The same emulated Wi-Fi path on BK7238 silicon, with the MAC model unchanged: join, DHCP "
        "from the device, ping and an HTTP command executed by the firmware.",
    "Snapshot: resumed MathDemo run reproduces the original run":
        "Saves a snapshot mid-boot and resumes from it in a second run. The emulator is "
        "deterministic, so the resumed run must print the same firmware lines as the original "
        "after the snapshot point, with the instruction counter within a small tolerance (it is "
        "estimated per translated block, which a cold translation cache can split differently). "
        "Anything the snapshot forgets shows up as a difference.",
    "Snapshot: resumed OpenBK7231T Wi-Fi AP run reproduces the original run":
        "The same round trip with the Wi-Fi MAC modelled and OpenBeken's AP up - the path the "
        "web-panel launcher takes. The resumed device must associate, lease and answer ping "
        "exactly as the original run did.",
    "Woox Tuya Original Firmware Boot":
        "Original Tuya (non-OpenBeken) light firmware. Verifies the fix that serves the un-striped "
        "protected-key region at 0x1ee000: instead of inventing a key from blank flash, the "
        "firmware now retrieves the stored key. With nothing on air it must not reach production "
        "test, which is the control for the case below.",
    "Woox Tuya stock firmware: factory test network on air switches it to production test":
        "Stock Tuya firmware, not OpenBeken, changing course because of what the emulator put on "
        "air. Every Tuya device scans at boot for the factory test network baked into it; when the "
        "virtual access point advertises that name, this one stops its normal start-up and enters "
        "manufacturing test. It never joins the network - seeing it advertised is the whole "
        "trigger. The same firmware with nothing on air does not do this, which is the case above.",
    "BK7238 Tuya RGB controller: raises its SmartLife pairing AP, serves DHCP, answers ping":
        "The pairing mode where the device makes its own network for a phone to join. This stock "
        "dump raises it unprompted, so the emulator joins as a client, is given an address by the "
        "device, and gets an answer to a ping. The device then announces itself in Tuya's LAN "
        "protocol; this firmware's version of it is sealed with a key only that device holds, so "
        "the announcement is identified by its framing and its contents are left alone.",
    "BK7231N Tuya 3-phase meter (paired): joins its stored network and reaches for the Tuya cloud":
        "A stock Tuya device captured after it had been set up, so its flash still holds the "
        "owner's network name. It asks for that network, the emulator plays it, and the device "
        "joins, takes an address, announces its own cloud identity on the LAN in a form every "
        "Tuya device encrypts the same way, and opens TLS to its cloud twice - once at a fixed "
        "address and once by name. Nothing leaves the machine: the connections are accepted and "
        "decoded locally. The identity it announces matches the product key given on the command "
        "line, which is what confirms the decryption rather than assuming it. The cloud connection "
        "itself is answered far enough to learn how it is secured: Tuya devices authenticate with "
        "a shared secret rather than a certificate, and this one accepts the answer and then stops, "
        "because the dump holds no such secret to present.",
    "BK7231N Tuya thermostat: EZ-mode sniffing reported, then falls back to AP pairing":
        "The pairing mode that needs real hardware behaviour: the device opens its receiver to "
        "every frame on the channel and reads the credentials out of the pattern a phone sends. "
        "The emulated MAC reports that the firmware did this, which is what tells a sniffing "
        "device apart from a scanning one. The same run then shows it give up, close the receiver "
        "back down and fall back to making its own pairing network.",
    "BK7231N Tuya alarm (paired): a second device's cloud identity decodes with the same shared key":
        "A different paired device with a different stored network and a different identity, "
        "included to show the announcement is readable because every Tuya device encrypts it with "
        "the same key, not because of anything special about the meter. What it announces matches "
        "the product key passed on the command line, which rules out rubbish that merely parses.",
    "BK7231N Tuya TempHum Sensor accepts its MCU identity and starts BLE/Wi-Fi":
        "The stock Tuya temp/humidity sensor that formerly looped forever on product-info queries "
        "to its absent companion MCU. The simulated MCU supplies the exact product identity from "
        "the dump; firmware validates it, initializes the device and BLE service, then advances to "
        "its working-mode query while Wi-Fi MAC activity is present.",
    "BK7231N Tuya plug auto-models BLE and enters factory mode":
        "A stock five-channel Tuya plug on SDK 2.3.3. Selecting its real BK7231N silicon now "
        "provides BLE 5.1 hardware automatically, without BLE/XVR workaround flags; the firmware "
        "completes HCI, database and advertising setup, initializes every relay plus metering, "
        "sees the emulated factory network and takes its production-mode branch.",
    "BK7231N Tuya BEOK Thermostat (TuyaOS 3.11.12) sends TuyaMCU heartbeats":
        "A paired BEOK TOL47WIFI-WP-WF thermostat running TuyaOS 3.11.12 - a newer SDK line than "
        "the Ettroit case, and one that never prints \"have actived\" yet still skips manufacturing "
        "test. It opens the MCU link at 9600 baud and streams TuyaMCU heartbeats. Selected by the "
        "mirror-KV signal: paired units have written both KV copies, factory dumps leave the mirror "
        "erased.",
    "BK7231N Tuya PJ1103C Dual-Clamp Power Meter sends TuyaMCU heartbeats":
        "A paired PJ1103C dual-clamp power meter (TuyaOS 3.11.12). The MCU owns the current clamps "
        "and all metering while the Beken is only the radio, so this covers a different hardware "
        "class from the two thermostat cases. Its KV pair carries a third distinct rotation count "
        "(16), showing the physical flash-addressing model is not tied to one store state.",
    "MathDemo Startup Command: startDriver BL0942 drives the meter UART":
        "Runs OpenBeken's BL0942 energy-meter driver from an injected startup command. The driver "
        "opens UART1 at 4800 baud and speaks the BL0942 register protocol - two init writes "
        "(0xA8|addr) followed by a repeating full-packet read (0x58 0xAA) - which looks nothing like "
        "TuyaMCU framing. Proves the UART1 capture path is protocol-agnostic, not tuned to 55 AA.",
    "BK7231N Tuya NAS-PS10 Presence Sensor sends TuyaMCU heartbeats":
        "A paired presence/radar sensor on TuyaOS 3.8.18 - a third SDK line and hardware class. "
        "It is the flash-write guard: the device persists a 4K sector during start-up, and while "
        "page-program and sector-erase opcodes were ignored it failed its own read-back verify and "
        "rebooted in a loop. With writes implemented it reaches normal operation and drives its MCU.",
    "BK7231N Tuya Afyeev GD4301 EV Charger accepts product, advances to working-mode":
        "An extra TuyaOS 3.x TuyaMCU dump for breadth (from FlashDumps): an EV charger, a device "
        "class the suite did not previously cover, on SDK 3.1.17. With --tuyamcu-raw and its "
        "licensed id (dsmsam7xpb3ht7rl) the simulated MCU answers in the raw form this SDK line "
        "wants, so the device accepts the product record (stored key matches our input), updates "
        "its product id, and advances to the working-mode query (0x02) and Wi-Fi link setup.",
    "BK7231N Tuya ATORCH AT4P Meter (SDK 2.1.17) boots past BLE, accepts product":
        "The 2.x SDK generation, reachable only with --xvr-selfclear. This ATORCH energy meter "
        "(stock Tuya 2.1.17) wedges twice in RF/BLE init on the XVR busy-bit spins at 0x9000F8 and "
        "0x900000 (both 'set bit 31, wait for it to clear'). --xvr-selfclear models those bits as "
        "self-clearing, walking it past both spins to the TuyaMCU link, where it runs the raw-form "
        "handshake with its licensed id (tjtigg991kvoiiqi) and advances to the working-mode query "
        "(0x02). The flag is opt-in because other dumps use 0x9000F8 the opposite way.",
    "BK7231N Tuya PC321 3-Phase Meter (SDK 2.0.2) boots past BLE, accepts product":
        "A second 2.x dump, on SDK 2.0.2 (vs ATORCH's 2.1.17): the PC321 3-phase power meter "
        "(from FlashDumps). Like ATORCH it wedges on the XVR busy spins (0x9000F8, 0x900000); "
        "--xvr-selfclear walks it past both, through BLE init to the TuyaMCU link, where its "
        "raw-form product record (licensed id gqmmtjclqb7reg5p) is accepted and it advances to "
        "the working-mode query (0x02). Adds a distinct SDK sub-version and device class.",
    "BK7231N Tuya A03CB3S Alarm (SDK 2.1.17) boots past BLE, accepts product":
        "Device-class breadth on the flag-reachable 2.x line: a security alarm/siren (A03CB3S, "
        "stock Tuya 2.1.17, from FlashDumps). Like ATORCH/PC321 it wedges on the XVR busy spins; "
        "--xvr-selfclear walks it past both to the TuyaMCU link, where its raw-form product record "
        "(licensed id ztoh9ka787lzjkpy) is accepted and it advances to the working-mode query (0x02).",
    "BK7231N Tuya 10-Gang Wall Switch accepts product, advances to working-mode":
        "Another TuyaOS 3.x TuyaMCU dump for device-class breadth (from FlashDumps): a 10-gang wall "
        "switch on SDK 3.1.28, a multi-relay panel distinct from the meters, sensors and charger "
        "already covered. With --tuyamcu-raw and its licensed id (ni2ztksbndubd9rf) it accepts the "
        "raw product record (stored key matches our input), updates its product id, and advances to "
        "the working-mode query (0x02) and Wi-Fi link setup.",
    "BK7231N Tuya Arlec RGB Strip (PWM light, TuyaOS 3.3.44) boots":
        "A paired RGB LED strip driven by hardware PWM straight from the Beken - cool on P6, warm "
        "on P8 at 1000 Hz - rather than by an external MCU, so unlike the other stock-Tuya cases it "
        "produces no UART1 traffic by design. Adds a fourth Tuya SDK generation (TuyaOS 3.3.44) and "
        "reaches normal operation after reading its protected key.",
    "BK7238 Tuya RGB Controller TY-02-3CH (stock, unencrypted) boots its light stack":
        "The first stock-firmware dump on BK7238 - every other image on this silicon here is "
        "OpenBeken, so the chip identity had only ever been exercised by firmware we build "
        "ourselves. This one reads the SCTRL device-id register back and prints it, so the "
        "device confirms the identity -chip BK7238 hands it. It is also the only stock Tuya "
        "dump that is plaintext rather than TUYA-encrypted. It boots through RF calibration "
        "into the Tuya light stack - colour gamma tables, dimmer curve, startup state - "
        "reports its own product id and starts BLE, with no --xvr-selfclear needed.",
    "BK7238 Tuya RGB Controller with --ble-core: BLE host completes HCI, adv applied":
        "The same BK7238 dump with the RivieraWaves BLE core modelled (--ble-core). Without it "
        "the controller arms deep sleep and never wakes, so the NimBLE host times out on every "
        "HCI command and loops on assert. With the core's slot clock, sleep/wake, clock "
        "correction and slot-clock interrupt modelled - and FIQ delivery added to the emulator - "
        "the controller runs its real sleep/wake loop, acknowledges every interrupt, answers "
        "the host, and the Tuya BLE service reports the advertisement applied. No radio: "
        "nothing goes on air.",
    "BK7231M VeSync WHOGPLUG Plug (VeSync SDK, BLE 5.1 core) advertises and scans":
        "A VeSync (Etekcity) EU smart plug on BK7231M - the first firmware in the suite that "
        "is neither OpenBeken nor Tuya. It runs VeSync SDK v1.2.1 on the Beken SDK and needs "
        "the N-family BLE 5.1 core, which --ble-core now models alongside BK7238's 5.2: the "
        "slot clock at its 5.1 offset, the BLE block's version register the link layer "
        "asserts on, and the hardware AES engine the host needs for its random numbers. It "
        "also needed two pwm_new behaviours, since the plug captures its metering chip's "
        "pulses with PWM: the counter-read request self-clears, and interrupt status is "
        "write-1-to-clear. BLE comes up, the advertisement (Etekcity manufacturer data "
        "carrying the BLE MAC) is decoded straight out of exchange memory, and network "
        "config starts a Wi-Fi scan - the edge of what the emulator models.",
    "BK7231N Tuya Globe Ceiling Fan (BLE-gated, SDK 2.1.17) --ble-core: Tuya beacon, TuyaMCU to Wi-Fi state":
        "A paired Tuya ceiling fan from the 'BLE-gated' class - about 70% of the BK7231N dumps "
        "in FlashDumps - whose firmware brings BLE up at start-up and used to stall on the "
        "unmodelled BLE core. With --ble-core it runs real BLE init (random numbers from the "
        "modelled AES engine, the stack up, the full advertising state machine) and stages "
        "Tuya's pairing beacon: service UUID 0xA201 carrying the device's own product key, "
        "decoded from exchange memory. With the simulated MCU answering, it accepts its product "
        "record and goes one step further than the --xvr-selfclear dumps, reporting its Wi-Fi "
        "state to the MCU.",
    "BK7231T Tuya Breaker/Leakage Switch (TuyaMCU 1.1.80) accepts raw product":
        "A stock Tuya breaker / leakage switch, BK7231T, SDK 1.1.80. It pins down where the "
        "TuyaMCU product-info wire form changes: the other 1.1.x dump here (TMWF02, 1.1.71) "
        "accepts the JSON product record, while this one rejects it on length (prod len = 36) "
        "and needs the raw 16-byte id - so the switch happens inside the 1.1.x series, not at "
        "the 2.x boundary. It is also the first stock dump that brings its entire BLE stack up "
        "unaided (STACK INIT OK, GATT database, advertising) with no --xvr-selfclear, then "
        "completes the product handshake and moves on to the working-mode query.",
    "BK7231N Tuya Ettroit ETWF4301 (paired) sends TuyaMCU heartbeats":
        "A stock Tuya ETWF4301 thermostat (SDK 3.1.28) and the first non-OpenBeken image found "
        "that actually drives its MCU. Because this dump was taken after pairing, the SDK skips "
        "manufacturing test, reaches normal operation, and sends TuyaMCU heartbeats unprompted - "
        "the same 55 AA 00 00 00 00 FF frame OpenBeken emits. Also guards the physical "
        "flash-addressing model: both KV copies (current 0x1ed000, mirror 0x1cf000) must read "
        "valid with matching counts.",
    "BK7231N Tuya zmai90 RN8209C Energy Meter Boots into its app":
        "A stock Tuya energy meter (zmai90, RN8209C metering chip). A pre-pair (unactivated) dump: "
        "it reads its protected key but then parks in the mf_test thread, so it never polls the "
        "meter over UART1. Proves the boot + key read; the empty UART1 is the dump's provisioning "
        "state.",
}


def dump_url(binary):
    """A downloadable GitHub raw URL for a firmware dump (works in CI and locally)."""
    name = os.path.basename(binary)
    repo = os.environ.get("GITHUB_REPOSITORY", "openshwprojects/BekenEmulator")
    ref = os.environ.get("GITHUB_SHA") or "main"
    return "https://github.com/%s/raw/%s/firmwares/%s" % (repo, ref, name)


# Which silicon a test covers, read off the dump's filename. Longer names come
# first so BK7252N is not matched as BK7252, and BK7231N not as BK7231. This is
# the *part* the dump came from, which is finer-grained than most served chip
# IDs.  BK7231U remains explicit because its upper GPIO bank differs from T.
def chip_of(test_config):
    """Best-effort chip name for a test: filename first, then the -chip arg."""
    name = periph.chip_from_name(test_config["binary"])
    if name:
        return name
    args = test_config.get("args", [])
    for i, a in enumerate(args):
        if a in ("-chip", "--chip") and i + 1 < len(args):
            return args[i + 1].upper()
    return "BK7231"


# Tags shown under each test title in the HTML report. Derived from the case
# itself rather than hand-maintained, so a new test is tagged the moment it is
# added. Four groups, rendered in this order:
#   source  - who wrote the firmware: OpenBeken / Tuya, or CLI for the arg tests
#   chip    - the silicon the dump came from (BK7231N, BK7238, ...)
#   state   - pairing state, for stock Tuya dumps that expose it
#   feature - what the case actually exercises (TuyaMCU, UART1, Flash, ...)
# A case may set "tags": [...] to add anything this cannot infer.
TAG_GROUPS = {}   # tag -> group, filled in below


def _tag(group, name):
    TAG_GROUPS[name] = group
    return name


def tags_for(test_config):
    """Infer the report tags for one test case."""
    name = test_config["name"]
    base = os.path.basename(test_config["binary"])
    args = " ".join(test_config.get("args", []))
    exp = " ".join(s if isinstance(s, str) else s[0]
                   for s in test_config.get("expected_strings", []))
    hay = " ".join((name, base, exp))
    tags = []

    # --- source -----------------------------------------------------------
    if name.startswith("CLI:"):
        tags.append(_tag("source", "CLI"))
    elif "OpenBK" in base or "mathDemo" in base:
        tags.append(_tag("source", "OpenBeken"))
    else:
        tags.append(_tag("source", "Tuya"))

    # --- chip -------------------------------------------------------------
    tags.append(_tag("chip", chip_of(test_config)))

    # --- pairing state (stock Tuya dumps only) ----------------------------
    if "mf_init succ" in exp or "have actived" in exp:
        tags.append(_tag("state", "paired"))
    elif "mf_test" in exp:
        tags.append(_tag("state", "pre-pair"))

    # --- features ---------------------------------------------------------
    # Each rule keys off what the case ASSERTS, not merely what it passes on the
    # command line, so a tag means "this test covers it" rather than "this test
    # happens to touch it".
    is_obk = "OpenBeken" in tags
    feat = []
    if "TuyaMCU" in base or "TuyaMCU" in name or "55 aa 00 00" in exp:
        feat.append("TuyaMCU")
    # UART1 only when the test actually asserts traffic on it.
    if "UART1/MCU" in exp:
        feat.append("UART1")
    if "uartSendHex" in hay:
        feat.append("uartSendHex")
    if "CFG_InitAndLoad" in exp or "obkStartupCommand" in base:
        feat.append("OBK config")
    if any(k in exp for k in ("key_addr", "get key", "get kvs", "kv info")):
        feat.append("flash/KV")
    if "idle " in exp or "1s timer" in name:
        feat.append("timers")
    # Only the case that genuinely exercises MQTT registration.
    if "MQTT_RegisterCallback" in exp:
        feat.append("MQTT")
    if any(k in exp for k in ("rwble", "STACK INIT", "advertising")):
        feat.append("BLE")
    # SARADC is an OpenBeken concern: N/M block in Main_OnEverySecond's
    # temperature read until the ADC interrupt is modelled. Stock Tuya dumps on
    # the same silicon never reach that path.
    if is_obk and ("7231N" in base or "7231M" in base):
        feat.append("SARADC")
    # Keyed off the pin assertions, so the tag means "this case verifies a
    # driven pin", not merely "this firmware happens to touch GPIO".
    if test_config.get("expected_pins"):
        feat.append("GPIO")
    if "BERRY:" in exp or "berry" in base:
        feat.append("Berry")
    if "Float basic" in exp:
        feat.append("float math")
    # Crypto only for the cases whose subject IS key handling.
    if "Invalid -key" in exp or "ARM code" in exp:
        feat.append("crypto")
    if "SPI mirror" in name:
        feat.append("SPI mirror")
    # The emulated air: a case that asserts what crossed it.
    if "[WIFI] " in exp:
        feat.append("Wi-Fi")
    if "[NET] " in exp:
        feat.append("IP network")
    if "[HTTP_PROBE]" in exp:
        feat.append("localhost forward")
    if "[NET] MQTT" in exp:
        feat.append("MQTT")
    if "[NET] NTP" in exp:
        feat.append("NTP")
    if "Tuya LAN" in exp:
        feat.append("Tuya LAN")
    if "TLS ClientHello" in exp:
        feat.append("cloud capture")
    if test_config.get("pcap_expect"):
        feat.append("pcap")
    if test_config.get("kind") == "snapshot_roundtrip":
        feat.append("snapshot")
    if "XVR" in name:
        feat.append("XVR")
    for f in feat:
        tags.append(_tag("feature", f))

    for extra in test_config.get("tags", []):
        if extra not in tags:
            tags.append(_tag("feature", extra))
    return tags


def _parse_expected(expected):
    """Normalise expected_strings entries to (string, min_count) pairs.

    An entry is either a plain string (needs to appear at least once) or a
    (string, count) tuple requiring at least `count` occurrences - used to prove
    a periodic line keeps printing, not just appears once.
    """
    out = []
    for e in expected:
        if isinstance(e, (tuple, list)) and len(e) == 2:
            out.append((e[0], int(e[1])))
        else:
            out.append((e, 1))
    return out


def _add_derived_tags(result):
    """Tags for what the run actually produced, not what the case declares.

    These cannot come from the test definition: whether a dump yields a Tuya
    config, or whether the firmware touched GPIO/PWM at all, is only known
    after the emulator has run.
    """
    derived = []
    if result.get("tuya_config"):
        derived.append("has Tuya config")
    per = result.get("periph")
    if per:
        if any(st != "factory" for _pin, _alias, st, _d in per["gpio"]):
            derived.append("has GPIO data")
        # Deliberately NOT keyed on "wrote PWM registers": the FreeRTOS tick
        # is a PWM channel in timer mode, so that tags every single firmware.
        if per.get("pwm_pins"):
            derived.append("has PWM")
    if result.get("timed_out"):
        derived.append("timed out")
    have = {t["name"] for t in result["tags"]}
    for name in derived:
        if name not in have:
            result["tags"].append({"name": name, "group": "data"})


# Chips whose PWM is the pwm_new block at 0x802B00 (capture offset 0x100+).
# BL2028N is a BK7231N clone; BK7231M is the N die in another package.


def _record_check(checks, label, ok, detail):
    """Print and record one non-string assertion (pins, PWM). Returns `ok`.

    The pin and PWM loops below differ only in how they describe what they
    found, so the verdict / print / record dance lives here once.
    """
    print("  [%s] %s%s" % ("PASS" if ok else "FAIL", label,
                           (" (%s)" % detail) if ok else ", got %s" % detail))
    checks.append({"string": label, "found": ok,
                   "count": 1 if ok else 0, "required": 1})
    return ok


def _periph(lines, chip=""):
    """Decode captured [EMU_GPIO]/[EMU_PWM] lines; never breaks a run."""
    try:
        gpio, pwm = periph.parse_lines(lines)
        # The decode itself lives in periph.decode_state so the GUI, which has
        # the emulator's live register dicts and never sees these text lines,
        # renders exactly what the report does.
        return periph.decode_state(gpio, pwm, chip)
    except Exception as exc:
        # Decoding is best-effort and must never fail a run, but swallowing it
        # silently once cost a whole debug cycle: a stale 2-tuple unpack here
        # made every GPIO tab look "empty" rather than "broken".
        print("[warn] peripheral decode failed: %r" % (exc,), file=sys.stderr)
        return None


def _pin_desc(value):
    """Human-readable form of a pin config word, for test output."""
    try:
        return periph.decode_pin(value)
    except Exception:
        return "?"


def _tuya_config(binary):
    """Best-effort Tuya config extraction; never breaks a test run."""
    try:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import tuya_config
        return tuya_config.extract(binary)
    except Exception:
        return None


def _result(test_config, passed, elapsed=0.0, insns=None, timed_out=False, output="", checks=None):
    """Assemble the structured record the HTML report consumes."""
    binary = test_config["binary"]
    if checks is None:
        checks = [{"string": s, "found": False, "count": 0, "required": n}
                  for s, n in _parse_expected(test_config["expected_strings"])]
    return {
        "name": test_config["name"],
        "description": DESCRIPTIONS.get(test_config["name"], ""),
        "binary": binary,
        "binary_name": os.path.basename(binary),
        "chip": chip_of(test_config),
        # Tuya user_param_key record recovered from the dump, if it keeps
        # one in plaintext - shown in the report's "Tuya config" tab.
        "tuya_config": _tuya_config(binary),
        "tags": [{"name": t, "group": TAG_GROUPS.get(t, "feature")}
                 for t in tags_for(test_config)],
        "dump_url": dump_url(binary),
        "args": test_config["args"],
        "passed": passed,
        "timed_out": timed_out,
        "elapsed": elapsed,
        "insns": insns,
        "checks": checks,
        "output": output,
    }


_PORT_LOCK = threading.Lock()
_CLAIMED_PORTS = set()


def _free_port():
    """A TCP port on 127.0.0.1 that nothing listens on right now."""
    import socket
    with _PORT_LOCK:
        for _ in range(32):
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.bind(("127.0.0.1", 0))
            port = s.getsockname()[1]
            s.close()
            if port not in _CLAIMED_PORTS:
                _CLAIMED_PORTS.add(port)
                return port
        return port


# 802.11 frame kinds a case can require in its capture ("pcap_expect"), as
# (type, subtype); a subtype of None matches every frame of that type.
PCAP_FRAME_KINDS = {
    "beacon": (0, 8), "probe request": (0, 4), "probe response": (0, 5),
    "authentication": (0, 11), "association request": (0, 0),
    "association response": (0, 1), "data": (2, None),
}


def _read_pcap_frames(path, expected_link):
    """Read raw frame payloads from a little-endian pcap 2.4 file."""
    import struct
    with open(path, "rb") as f:
        raw = f.read()
    if len(raw) < 24:
        raise ValueError("%d bytes, no pcap header" % len(raw))
    magic, vmaj, vmin, _, _, _, link = struct.unpack("<IHHiIII", raw[:24])
    if magic != 0xA1B2C3D4 or (vmaj, vmin) != (2, 4) or link != expected_link:
        raise ValueError("header magic 0x%08X v%d.%d link type %d" % (magic, vmaj, vmin, link))
    frames, off = [], 24
    while off + 16 <= len(raw):
        incl = struct.unpack("<I", raw[off + 8:off + 12])[0]
        if off + 16 + incl > len(raw):
            break
        frames.append(raw[off + 16:off + 16 + incl])
        off += 16 + incl
    return frames


def _pcap_frames(path):
    """The 802.11 frames in a capture written by --pcap.

    Raises ValueError unless it is a little-endian pcap 2.4 with link type
    105. A record cut short at the end - the emulator is killed mid-run - is
    ignored rather than failing the file.
    """
    return _read_pcap_frames(path, 105)


def _ip_pcap_frames(path):
    """The Ethernet frames in a capture written by --pcap-ip (link type 1)."""
    return _read_pcap_frames(path, 1)


def _check_ip_pcap(path, expect, checks):
    """Assert a case's IP-level capture ("{pcap_ip}" in its args).

    Each "ip_pcap_expect" item is text some frame's bytes must contain. This
    is what proves the bytes a user opens in Wireshark are the device's real
    traffic and not a reconstruction of the log lines.
    """
    try:
        frames = _ip_pcap_frames(path)
        ok, detail = bool(frames), "%d frames" % len(frames)
    except (OSError, ValueError) as e:
        frames, ok, detail = [], False, str(e)
    all_ok = _record_check(checks, "pcap-ip: valid Ethernet capture", ok, detail)
    for item in expect:
        needle = item.encode("latin-1")
        n = sum(1 for fr in frames if needle in fr)
        all_ok = _record_check(checks, "pcap-ip: a frame carries '%s'" % item,
                               n > 0, "%d frames" % n if n else "none") and all_ok
    return all_ok


def _check_pcap(path, expect, checks):
    """Assert a case's capture file ("{pcap}" in its args).

    The file must parse as an 802.11 capture, and each "pcap_expect" item
    must be in it: a frame kind from PCAP_FRAME_KINDS, or any other text,
    which some frame's bytes must contain (an HTTP request line, an MQTT
    topic, the offline responder's reply). This is what proves the file a
    user opens in Wireshark holds the traffic the [NET] lines describe.
    """
    try:
        frames = _pcap_frames(path)
        ok, detail = bool(frames), "%d frames" % len(frames)
    except (OSError, ValueError) as e:
        frames, ok, detail = [], False, str(e)
    all_ok = _record_check(checks, "pcap: valid 802.11 capture", ok, detail)
    for item in expect:
        kind = PCAP_FRAME_KINDS.get(item)
        if kind:
            n = sum(1 for fr in frames if len(fr) >= 2 and (fr[0] >> 2) & 3 == kind[0]
                    and (kind[1] is None or fr[0] >> 4 == kind[1]))
            label = "pcap: %s frame" % item
        else:
            needle = item.encode("latin-1")
            n = sum(1 for fr in frames if needle in fr)
            label = "pcap: a frame carries '%s'" % item
        all_ok = _record_check(checks, label, n > 0, "%d frames" % n if n else "none") and all_ok
    return all_ok


def _http_probes(port, probes, emit):
    """Run a case's http_probe request(s) one after another (see _http_probe)."""
    for pr in probes:
        _http_probe(port, pr["path"], pr.get("expect", []), emit,
                    method=pr.get("method", "GET"), body=pr.get("body"), tail=pr.get("expect_tail"))


def _http_probe(port, path, expect, emit, timeout=180, method="GET", body=None, tail=None):
    """Send METHOD http://127.0.0.1:port/path the way a browser would; report via emit(line).

    The lines are what a case asserts on, so a probe that fails leaves its
    markers missing and the case fails with the reason in its log:
        [HTTP_PROBE] GET /index -> HTTP/1.1 200 OK (4909 bytes)
        [HTTP_PROBE] page contains: Built on
        [HTTP_PROBE] GET /x ends with: }}      (with `tail`: the reply is complete)
    Browser-style headers on purpose: OpenBeken answered a bare HTTP/1.0
    request with its "Not found" page, and what matters is what a browser gets.
    """
    import socket
    payload = body.encode("utf-8") if body is not None else b""
    req = ("%s %s HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nUser-Agent: Mozilla/5.0 (BekenEmulator self-test)\r\n"
           "Accept: text/html,application/xhtml+xml,application/json\r\nConnection: close\r\n"
           % (method, path, port))
    if body is not None:
        req += "Content-Type: text/plain\r\nContent-Length: %d\r\n" % len(payload)
    req = req.encode("ascii") + b"\r\n" + payload
    data = b""
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
        s.settimeout(timeout)
        s.sendall(req)
        while True:
            chunk = s.recv(65536)
            if not chunk:
                break
            data += chunk
        s.close()
    except Exception as e:
        emit("[HTTP_PROBE] %s %s failed: %s (%d bytes received)\n" % (method, path, e, len(data)))
        if not data:
            return
    head, _, reply = data.partition(b"\r\n\r\n")
    status = head.split(b"\n", 1)[0].strip().decode("latin-1", "replace") if head else "(no reply)"
    emit("[HTTP_PROBE] %s %s -> %s (%d bytes)\n" % (method, path, status, len(data)))
    text = reply.decode("utf-8", "replace")
    for s_ in expect:
        if s_ in text:
            emit("[HTTP_PROBE] page contains: %s\n" % s_)
    if tail is not None and text.rstrip().endswith(tail):
        emit("[HTTP_PROBE] %s %s ends with: %s\n" % (method, path, tail))


def _stream_until(cmd, after, until, timeout):
    """Run cmd; return (lines, timed_out) once every `until` marker is seen after `after`.

    `until` is [(string, count)]. Only lines following the first line that
    contains `after` count toward it. Unlike run_test, the [EMU_*] capture
    lines are KEPT: they carry exact instruction counts (EMU_INSNS, the
    UART1 byte timestamps), so a resumed run whose timing state drifted shows
    up as different numbers even when the firmware's own text is identical.
    The process is killed once satisfied (the emulator never exits).
    """
    env = dict(os.environ)
    env["EMU_REPORT"] = "1"
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                            bufsize=1, encoding="utf-8", errors="ignore", env=env)
    lines, lock = [], threading.Lock()
    state = {"seen_after": False, "counts": {s: 0 for s, _ in until}}

    def reader():
        for line in proc.stdout:
            with lock:
                lines.append(line)
                if state["seen_after"]:
                    for s, _ in until:
                        state["counts"][s] += line.count(s)
                elif after in line:
                    state["seen_after"] = True

    th = threading.Thread(target=reader, daemon=True)
    th.start()
    start, timed_out = time.time(), False
    while True:
        with lock:
            done = state["seen_after"] and all(state["counts"][s] >= n for s, n in until)
        if done or proc.poll() is not None:
            break
        if time.time() - start > timeout:
            timed_out = True
            break
        time.sleep(0.25)
    if proc.poll() is None:
        proc.kill()
        try:
            proc.wait(timeout=5)
        except Exception:
            pass
    th.join(timeout=2.0)
    with lock:
        return list(lines), timed_out


def run_snapshot_roundtrip(test_config):
    """Snapshot save + resume must reproduce the original run exactly.

    Run A boots and saves a snapshot at `snapshot_at` instructions, then keeps
    going; run B starts from that snapshot. Device time is derived from the
    instruction count, so the emulator is deterministic: after the snapshot
    point both runs must print the same lines in the same order. Any state the
    snapshot forgets - a peripheral shadow, a CPU bank, pending interrupts -
    shows up as the two streams diverging, usually within a few lines.
    """
    import tempfile
    name = test_config["name"]
    binary = test_config["binary"]
    timeout = test_config.get("timeout", 300)
    until = _parse_expected(test_config["expected_strings"])
    print("=====================================")
    print(f"Running Test: {name}")
    print(f"Binary: {binary}")
    if not os.path.exists(binary):
        print(f"FAIL: Binary not found: {binary}")
        return _result(test_config, passed=False, output="Binary not found: %s" % binary)

    fd, snap = tempfile.mkstemp(prefix="bekensim_selftest_", suffix=".snap")
    os.close(fd)
    try:
        os.remove(snap)
    except OSError:
        pass
    base = [sys.executable, MAIN_SCRIPT, binary] + test_config["args"]
    start = time.time()
    a_lines, a_to = _stream_until(base + ["--snapshot-save", snap, "--snapshot-at",
                                          str(test_config["snapshot_at"])],
                                  "[SNAPSHOT] saved", until, timeout)
    b_lines, b_to = ([], False)
    if os.path.exists(snap):
        b_lines, b_to = _stream_until(base + ["--snapshot-load", snap],
                                      "[SNAPSHOT] restored", until, timeout)
    elapsed = time.time() - start
    try:
        os.remove(snap)
    except OSError:
        pass

    def after(lines, marker):
        for i, l in enumerate(lines):
            if marker in l:
                return lines[i + 1:]
        return []

    a_all, b_all = after(a_lines, "[SNAPSHOT] saved"), after(b_lines, "[SNAPSHOT] restored")
    # Two comparisons. The firmware's own output must match exactly. The
    # emulator's instruction counter must match closely but not exactly: it is
    # an estimate made per translated block (bytes / 4), and where Unicorn cuts
    # blocks depends on its translation-cache history, which no snapshot can
    # carry - a resumed run starts with a cold cache and was measured one
    # instruction behind after ~500k. A lost timer or tick phase moves the
    # counts by thousands, so a tight tolerance still catches real state loss.
    a_after = [l for l in a_all if not l.startswith("[EMU_")]
    b_after = [l for l in b_all if not l.startswith("[EMU_")]

    def insn_marks(lines):
        out = []
        for l in lines:
            parts = l.split()
            if l.startswith("[EMU_INSNS]") and len(parts) > 1 and parts[1].isdigit():
                out.append(int(parts[1]))
        return out

    a_ins, b_ins = insn_marks(a_all), insn_marks(b_all)
    n_ins = max(0, min(len(a_ins), len(b_ins)) - 1)
    drift = max((abs(a_ins[i] - b_ins[i]) for i in range(n_ins)), default=None)
    tolerance = test_config.get("insn_tolerance", 1000)
    # The last line of each run may be cut off by the kill: compare whole lines only.
    k = max(0, min(len(a_after), len(b_after)) - 1)
    first_diff = next((i for i in range(k) if a_after[i] != b_after[i]), None)
    min_lines = test_config.get("min_compared_lines", 10)

    checks = []
    ok = True
    saved = any("[SNAPSHOT] saved" in l for l in a_lines)
    restored = any("[SNAPSHOT] restored" in l for l in b_lines)
    ok &= _record_check(checks, "run A saved a snapshot", saved,
                        "saved" if saved else "no [SNAPSHOT] saved line")
    ok &= _record_check(checks, "run B restored it", restored,
                        "restored" if restored else "no [SNAPSHOT] restored line")
    joined_b = "".join(b_after)
    for s, n in until:
        c = joined_b.count(s)
        ok &= _record_check(checks, "resumed run reaches '%s'%s" % (s, " x%d" % n if n > 1 else ""),
                            c >= n, "%dx" % c)
    ok &= _record_check(checks, "at least %d lines compared after the snapshot point" % min_lines,
                        k >= min_lines, "%d lines" % k)
    ok &= _record_check(checks, "resumed firmware output identical to the original run",
                        first_diff is None,
                        "%d lines match" % k if first_diff is None else
                        "line %d differs: %r vs %r" % (first_diff + 1, a_after[first_diff].rstrip(),
                                                      b_after[first_diff].rstrip()))
    ok &= _record_check(checks, "instruction counter within %d of the original run" % tolerance,
                        drift is not None and drift <= tolerance,
                        ("%d marks compared, max drift %d" % (n_ins, drift)) if drift is not None
                        else "no [EMU_INSNS] marks to compare")
    output = ("=== run A (saves at %s instructions) ===\n" % test_config["snapshot_at"] + "".join(a_lines)
              + "\n=== run B (resumes from the snapshot) ===\n" + "".join(b_lines))
    result = _result(test_config, passed=bool(ok), elapsed=elapsed, timed_out=a_to or b_to,
                     output=output, checks=checks)
    _add_derived_tags(result)
    if not ok:
        print("--- CAPTURED OUTPUT ---")
        print(output)
        print("-----------------------")
        print(f"Test '{name}' FAILED.")
        return result
    print(f"Test '{name}' PASSED. ({elapsed:.0f}s)")
    return result


def run_test(test_config):
    if test_config.get("kind") == "snapshot_roundtrip":
        return run_snapshot_roundtrip(test_config)
    name = test_config["name"]
    binary = test_config["binary"]
    # "{free_port}" in the args (and in an http_probe) becomes a port picked
    # now, so a case that forwards a host port never collides with whatever
    # else the machine is running.
    port = _free_port() if any("{free_port}" in a for a in test_config["args"]) else None
    args = [a.replace("{free_port}", str(port)) for a in test_config["args"]]
    # http_probe: one request dict or a list of them, run in order once the
    # first one's "after" marker shows up.
    probes = test_config.get("http_probe")
    if isinstance(probes, dict):
        probes = [probes]
    timeout = test_config.get("timeout", 30)
    expected = test_config["expected_strings"]

    print(f"=====================================")
    print(f"Running Test: {name}")
    print(f"Binary: {binary}")

    if not os.path.exists(binary):
        print(f"FAIL: Binary not found: {binary}")
        return _result(test_config, passed=False, output="Binary not found: %s" % binary)

    # "{pcap}" in the args becomes a fresh temporary capture file, checked
    # against the case's "pcap_expect" after the run and then deleted.
    pcap_path = ip_pcap_path = None
    if any("{pcap}" in a for a in args):
        import tempfile
        fd, pcap_path = tempfile.mkstemp(prefix="bekensim_", suffix=".pcap")
        os.close(fd)
        args = [a.replace("{pcap}", pcap_path) for a in args]
    if any("{pcap_ip}" in a for a in args):
        import tempfile
        fd, ip_pcap_path = tempfile.mkstemp(prefix="bekensim_ip_", suffix=".pcap")
        os.close(fd)
        args = [a.replace("{pcap_ip}", ip_pcap_path) for a in args]

    cmd = [sys.executable, MAIN_SCRIPT, binary] + args
    # EMU_REPORT makes the emulator periodically print its approximate
    # instruction count on [EMU_INSNS] lines, which the reader below strips out
    # of the displayed log and records for the report.
    child_env = dict(os.environ)
    child_env["EMU_REPORT"] = "1"

    # Stream the child's output and stop as soon as every expected string and
    # declared peripheral state has been seen. The emulator never exits on its
    # own, so the old
    # run-until-timeout approach cost each boot case its full timeout - and made
    # the suite fragile, because a case that reached its markers late (heavy
    # chip, or a busy machine) would be killed by the deadline before finishing.
    # Streaming makes a passing case take only as long as its last marker needs,
    # and a real failure still fails at the timeout.
    try:
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True,
                                bufsize=1, encoding="utf-8", errors="ignore", env=child_env)
    except Exception as e:
        print(f"FAIL: Failed to launch subprocess: {e}")
        return _result(test_config, passed=False, output="Failed to launch subprocess: %s" % e)

    reqs = _parse_expected(expected)
    required = {s: n for s, n in reqs}
    counts = {s: 0 for s, n in reqs}
    lines = []
    remaining = set(required)          # strings not yet at their required count
    insns_holder = [None]
    periph_lines = []
    uart1_lines = []
    lock = threading.Lock()
    test_chip = chip_of(test_config)
    has_periph_expectations = bool(test_config.get("expected_pins")
                                   or test_config.get("expected_pwm"))
    assertion_periph_lines = None

    def complete_periph_boundary():
        """Freeze the first atomic state after all text markers are complete.

        The state is captured whether or not it matches. A later animation must
        not turn a wrong completion state into a pass, and CI linger remains
        log-only rather than rewriting the verdict.
        """
        nonlocal assertion_periph_lines
        if (has_periph_expectations and assertion_periph_lines is None
                and not remaining):
            assertion_periph_lines = list(periph_lines)

    def reader():
        for line in proc.stdout:
            if line.startswith("[EMU_GPIO]") or line.startswith("[EMU_PWM]"):
                with lock:
                    periph_lines.append(line.strip())
                continue
            if line.startswith("[EMU_PERIPH]"):
                with lock:
                    complete_periph_boundary()
                continue
            if line.startswith("[EMU_INSNS]"):
                parts = line.split()
                if len(parts) >= 2 and parts[1].isdigit():
                    insns_holder[0] = int(parts[1])
                # An instruction report is also a quiescent boundary: this
                # slow tick emitted and cleared every pending GPIO/PWM write
                # immediately before this line, even if there were none.
                with lock:
                    complete_periph_boundary()
                continue
            if line.startswith("[EMU_UART1]"):
                uart1_lines.append(line.strip())
                continue
            ingest(line)
            if probes and not probe_started[0] and probes[0]["after"] in line:
                probe_started[0] = True
                threading.Thread(target=_http_probes, daemon=True,
                                 args=(port, probes, ingest)).start()

    def ingest(line):
        # One place that records a line and counts it against the markers -
        # shared by the emulator's output and the HTTP probe's report lines.
        with lock:
            lines.append(line)
            for s_ in list(remaining):
                c = line.count(s_)
                if c:
                    counts[s_] += c
                    if counts[s_] >= required[s_]:
                        remaining.discard(s_)

    probe_started = [False]

    th = threading.Thread(target=reader, daemon=True)
    th.start()

    start = time.time()
    hit_timeout = False
    done_at = None          # when every marker was first satisfied
    while True:
        with lock:
            done = (not remaining
                    and (not has_periph_expectations
                         or assertion_periph_lines is not None))
        if done:
            if proc.poll() is not None:
                break       # nothing left to capture - process already gone
            if LINGER <= 0:
                break
            # Keep running a little longer to capture more of the boot. The
            # verdict is already settled; this only enriches the log.
            if done_at is None:
                done_at = time.time()
                print(f"  ... all completion conditions met, lingering {LINGER:.0f}s "
                      "for a fuller log")
            elif time.time() - done_at >= LINGER:
                break
        elif proc.poll() is not None:
            # process exited on its own (e.g. a CLI test); let the reader drain.
            th.join(timeout=1.0)
            break
        elif time.time() - start > timeout:
            hit_timeout = True
            break
        time.sleep(0.25)

    if proc.poll() is None:
        proc.kill()
        try:
            proc.wait(timeout=5)
        except Exception:
            pass
    th.join(timeout=2.0)

    with lock:
        output = "".join(lines)
    elapsed = time.time() - start
    if hit_timeout:
        print(f"Note: reached {timeout}s timeout. Checking output up to this point.")

    all_passed = True
    checks = []
    for string, need in reqs:
        actual = output.count(string)      # recount from the full captured log
        found = actual >= need
        checks.append({"string": string, "found": found, "count": actual, "required": need})
        if found:
            if need > 1:
                print(f"  [PASS] Found '{string}' x{actual} (need {need})")
            else:
                print(f"  [PASS] Found string: '{string}'")
        else:
            if need > 1:
                print(f"  [FAIL] '{string}' found {actual}x, need {need}")
            else:
                print(f"  [FAIL] Missing string: '{string}'")
            all_passed = False

    # Strings that must NOT appear, when a case declares "unexpected_strings".
    # The negative half of a fix: a case that proves a boot now gets past an
    # assert should also prove the assert itself is gone, not merely that
    # later lines still print after it. Checked over the whole captured log,
    # which runs at least up to the last expected marker.
    for string in test_config.get("unexpected_strings") or []:
        actual = output.count(string)
        if not _record_check(checks, "absent: '%s'" % string, actual == 0,
                             "never printed" if actual == 0 else "printed %dx" % actual):
            all_passed = False

    if pcap_path:
        if not _check_pcap(pcap_path, test_config.get("pcap_expect") or [], checks):
            all_passed = False
        try:
            os.remove(pcap_path)
        except OSError:
            pass

    if ip_pcap_path:
        if not _check_ip_pcap(ip_pcap_path, test_config.get("ip_pcap_expect") or [], checks):
            all_passed = False
        try:
            os.remove(ip_pcap_path)
        except OSError:
            pass

    # Assert the first complete peripheral batch after the final text marker,
    # not whatever an animation changes during CI's optional log-only linger.
    # Failed/time-out cases still show their final state in the report, but a
    # missing completion snapshot cannot make an assertion pass.
    snapshot_captured = (not has_periph_expectations
                         or assertion_periph_lines is not None)
    periph_data = _periph(assertion_periph_lines if assertion_periph_lines is not None
                          else periph_lines, test_chip)

    # Pin assertions, when a case declares "expected_pins". This is what turns
    # the GPIO capture from a display into a tested invariant: the expected
    # register value is derived from the vendor SDK's own gpio_config() /
    # gpio_output(), not from whatever the emulator happened to emit. Without
    # it, a break in the capture path renders an empty tab and still "passes".
    for pin, want in sorted((test_config.get("expected_pins") or {}).items(),
                            key=lambda kv: kv[0]):
        got = (periph_data or {}).get("raw", {}).get(pin)
        ok = snapshot_captured and periph_data is not None and got == want
        # want=None asserts that the pin was not written through the test's
        # completion point. That is the negative half of the check: it proves
        # the captured pin follows the command rather than a fixed pin.
        if want is None:
            label = "GPIO P%d untouched" % pin
        else:
            label = "GPIO P%d config = 0x%02X" % (pin, want)
        if ok:
            detail = "no write captured" if want is None else _pin_desc(want)
        else:
            detail = "0x%02X" % got if got is not None else "no write captured"
        if not _record_check(checks, label, ok, detail):
            all_passed = False

    # PWM assertions. Same idea as expected_pins: the period is 26 MHz / freq
    # straight out of HAL_PIN_PWM_Start, and the duty is that period scaled by
    # the channel value, so both are known before the emulator runs.
    for ch, (want_period, want_duty) in sorted(
            (test_config.get("expected_pwm") or {}).items()):
        chans = {c[0]: c for c in ((periph_data or {}).get("pwm_channels") or [])}
        got = chans.get(ch)
        # duty None = assert the period only. Some devices ramp their duty
        # during light init, so it is not a stable invariant at the moment the
        # harness stops; pinning one would make the test fail on timing rather
        # than on decoding.
        ok = (snapshot_captured and got is not None and got[1] == want_period
              and (want_duty is None or got[2] == want_duty))
        if want_duty is None:
            label = "PWM%d period = 0x%04X (duty varies, not asserted)" % (ch, want_period)
        else:
            label = "PWM%d period = 0x%04X, duty = 0x%04X" % (ch, want_period, want_duty)
        if ok:
            detail = "%.1f Hz, %.1f%%" % (got[3], got[4])
        else:
            detail = ("period 0x%04X duty %s" % (got[1], got[2])) if got else "channel not decoded"
        if not _record_check(checks, label, ok, detail):
            all_passed = False

    result = _result(test_config, passed=all_passed, elapsed=elapsed,
                     insns=insns_holder[0], timed_out=hit_timeout,
                     output=output, checks=checks)
    result["periph"] = periph_data
    result["uart1"] = uart1_lines
    _add_derived_tags(result)

    if not all_passed:
        print("--- CAPTURED OUTPUT ---")
        print(output)
        print("-----------------------")
        print(f"Test '{name}' FAILED.")
        return result

    print(f"Test '{name}' PASSED. ({elapsed:.0f}s)")
    return result

def _write_report(results, wall_time=None):
    """Emit the HTML report to report/index.html. Never fails the run itself."""
    try:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import report as report_mod
        out_dir = os.path.join(ROOT_DIR, "report")
        os.makedirs(out_dir, exist_ok=True)
        run_id = os.environ.get("GITHUB_RUN_ID")
        run_url = None
        if run_id:
            run_url = "%s/%s/actions/runs/%s" % (
                os.environ.get("GITHUB_SERVER_URL", "https://github.com"),
                os.environ.get("GITHUB_REPOSITORY", ""), run_id)
        meta = {
            "total_time": wall_time if wall_time is not None else sum(r["elapsed"] for r in results),
            "chips": sorted({r["chip"] for r in results if r.get("chip")}),
            "generated_at": datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M UTC"),
            "commit": os.environ.get("GITHUB_SHA"),
            "repo": os.environ.get("GITHUB_REPOSITORY", "openshwprojects/BekenEmulator"),
            "run_url": run_url,
        }
        path = report_mod.generate(results, meta, os.path.join(out_dir, "index.html"))
        print(f"HTML report written: {path}")
    except Exception as e:
        print(f"WARN: could not write HTML report: {e}")

def main():
    # -k PATTERN runs only the cases whose name contains PATTERN (case
    # insensitive; repeatable). Selecting by name rather than by TEST_CASES
    # index matters: indices shift whenever a case is inserted, and running
    # "case 20" standalone has verified the wrong test before. A filtered run
    # leaves report/index.html alone - it describes the whole suite.
    patterns = [a.lower() for i, a in enumerate(sys.argv[1:])
                if i > 0 and sys.argv[i] == "-k"]
    jobs_arg = [int(a) for i, a in enumerate(sys.argv[1:])
                if i > 0 and sys.argv[i] in ("-j", "--jobs")]
    default_jobs = min(4, os.cpu_count() or 1) if os.environ.get("CI") else min(6, os.cpu_count() or 1)
    jobs = max(1, jobs_arg[-1] if jobs_arg else int(os.environ.get("JOBS") or default_jobs))
    selected = [t for t in TEST_CASES
                if not patterns or any(p in t["name"].lower() for p in patterns)]
    if patterns and not selected:
        print("No test name contains any of: %s" % ", ".join(patterns))
        sys.exit(2)

    suite_start = time.time()
    if jobs > 1 and len(selected) > 1:
        from concurrent.futures import ThreadPoolExecutor
        indexed = sorted(enumerate(selected),
                         key=lambda it: (-int(it[1].get("kind") == "snapshot_roundtrip"),
                                         -it[1].get("timeout", 60),
                                         it[0]))
        with ThreadPoolExecutor(max_workers=jobs) as pool:
            futs = [(idx, pool.submit(run_test, t)) for idx, t in indexed]
            out = [None] * len(selected)
            for idx, fut in futs:
                out[idx] = fut.result()
            results = out
    else:
        results = [run_test(test) for test in selected]
    wall_time = time.time() - suite_start
    passed = sum(1 for r in results if r["passed"])
    failed = len(results) - passed

    print(f"=====================================")
    print(f"Test Run Completed: {passed} passed, {failed} failed. ({wall_time:.1f}s)")

    if not patterns:
        _write_report(results, wall_time=wall_time)

    sys.exit(1 if failed else 0)

if __name__ == "__main__":
    main()

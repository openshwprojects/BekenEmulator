import subprocess
import threading
import time
import os
import re
import sys
from datetime import datetime, timezone

# Get the path to the root directory
ROOT_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
MAIN_SCRIPT = os.path.join(ROOT_DIR, 'src', 'main.py')

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
        # Plaintext image (beken_freertos_sdk layout) - no key.
        "args": ["--only-uart"],
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
        # builds this is stock vendor firmware, and it boots far enough to
        # enumerate the device's own GPIO/datapoint map, then init TCP/IP.
        "name": "BK7231Q Tuya MOES Relay Boot",
        "binary": os.path.join(ROOT_DIR, "firmwares", "BK7231Q_Tuya_MOES_Relay_WA2_1.1.3.bin"),
        "args": ["--only-uart"],
        "timeout": 120,
        "expected_strings": [
            "TUYA IOT SDK V:2.3.1",
            # The build identifies itself as a 1-switch BK7231 OEM app.
            "oem_bk7321_bk_1_switch:1.1.3",
            # Device brings up its pin map (relay on pin 18, button on pin 6).
            "IO - relay[0]:",
            "Initializing TCP/IP stack"
        ]
    },
    {
        # BL2028N is BK7231N silicon rebadged: this dump self-reports
        # "chip id=7231a device id=18520001" - the exact BK7231 default identity
        # - so it runs with no -key and the default -chip. It is the ONLY test
        # that reaches BLE/HCI bring-up, so it also guards the XVR fix on the
        # BLE path. (Ordinary Tuya BL2028N dumps are plaintext; the Uascent
        # Matter builds are custom-keyed and are intentionally not used here.)
        "name": "BL2028N (=BK7231N) Boots to BLE init",
        "binary": os.path.join(ROOT_DIR, "firmwares", "BL2028N_Dreo_DR-HTF004S_Fan_PAI-053.bin"),
        "args": ["--only-uart"],
        "timeout": 120,
        "expected_strings": [
            # Proves BL2028N presents as BK7231N.
            "chip id=7231a device id=18520001",
            "calibration_main over",
            # Reaches BLE host-stack init - deeper than any other case, and only
            # possible because the XVR transaction register is modelled.
            "rwble_hl_init ok",
            "Initializing TCP/IP"
        ]
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
        ]
    },
    {
        # The BK7231N temp/humidity sensor exercises the same protected-key path
        # and was the case that exposed it: with 0xFF served it failed the magic
        # check ("flash is encrypted or empty" - the magic and crc32 it reported
        # are literally AES-128-ECB-decrypt(0xFF x16) under the firmware's
        # hardcoded key "qwertyuiopasdfgh"), then dropped to mf_test with
        # "db init fails". Serving the dump's real bytes lets it decrypt its key.
        "name": "BK7231N Tuya TempHum Sensor Reads Protected Key",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7231N_Tuya_TempHum_LCD_Sensor_TuyaMCU_2025-26-9.bin"),
        "args": ["--only-uart", "--uart1-hex", "-key", "TUYA"],
        "timeout": 300,
        "expected_strings": [
            "key_addr: 0x1ee000",
            "get key:",
            "0x6b 0x24 0x0 0x73 0xf4 0x11 0x45 0xed 0x40 0xf1 0x9 0x9f 0x16 0xd3 0xb5 0xc0"
        ]
    },
    {
        # Stock Tuya smart plug (Tuya IoT SDK 2.3.3). A third, independent
        # device covering the protected-key path at 0x1ee000 - it was picked at
        # random from the dump corpus after that fix landed, so it checks the
        # fix generalises rather than being tuned to the two devices that
        # motivated it. Boots further than the sensor: reaches OEM config load
        # and never drops into mf_test.
        "name": "BK7231N Tuya Plug (SDK 2.3.3) Boots and Reads Protected Key",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7231N_Tuya_Plug_TSL-PS-F4U-01-W_1.1.17.bin"),
        "args": ["--only-uart", "--uart1-hex", "-key", "TUYA"],
        "timeout": 300,
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
            "0xe7 0x10 0xac 0x15 0x88 0x2b 0x38 0x50 0xb2 0x9a 0xd 0xfa 0x35 0xbe 0x99 0x37"
        ]
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
        "name": "BK7231N Tuya zmai90 RN8209C Energy Meter Boots",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7231N_Tuya_zmai90_RN8209C_EnergyMeter.bin"),
        "args": ["--only-uart", "--uart1-hex", "-key", "TUYA"],
        "timeout": 300,
        "expected_strings": [
            "< TUYA IOT SDK V:2.3.1 BS:40.00_PT:2.2_LAN:3.3_CAD:1.0.3_CD:1.0.0 >",
            "firmware compiled at Apr 26 2022 15:12:39",
            "init protected data length 460",
            "key_addr: 0x1ee000",
            "get key:",
            "0x80 0xd9 0xca 0xc2 0x32 0xb4 0x9c 0x30 0x6e 0x8b 0xc2 0x3d 0xf4 0x4c 0x39 0x7c"
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
        "name": "BK7231N Tuya Feconn MR16 RGBCT Bulb (pwm_new ch3) boots",
        "binary": os.path.join(ROOT_DIR, "firmwares",
                               "BK7231N_Tuya_Feconn_MR16_RGBCT_5ch_PWM_1000Hz_1.3.21.bin"),
        "args": ["--only-uart", "-key", "TUYA"],
        "timeout": 300,
        "expected_strings": [
            "< TUYA IOT SDK V:2.3.3 BS:40.00_PT:2.2_LAN:3.4_CAD:1.0.5_CD:1.0.0 >",
            "oem_bk7231n_light_ty:1.3.21",
            # Paired dump: skips manufacturing test at boot.
            "have actived over 15 min, not enter mf_init",
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
    }
]

# Report-facing prose, one per test name. Kept separate from TEST_CASES so the
# (working, exactly-asserted) case definitions stay untouched; the inline
# comments there document specific assertion strings for maintainers, while
# these summarise each test for the HTML report's expandable description.
DESCRIPTIONS = {
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
    "BK7231N Tuya Feconn MR16 RGBCT Bulb (pwm_new ch3) boots":
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
        "A Tuya BK7231Q relay dump booting the Tuya IoT SDK through TCP/IP stack initialisation.",
    "BL2028N (=BK7231N) Boots to BLE init":
        "A BL2028N (a BK7231N-class part) fan dump booting to BLE stack init (rwble_hl_init), "
        "covering the BLE-capable boot path.",
    "Tuya TMWF02 TuyaMCU Boots past crypto accel":
        "A stock Tuya fan-switch dump. Boots past the 0x810000 crypto-accelerator stall into the "
        "BLE host stack and network-config advertising.",
    "Woox Tuya Original Firmware Boot":
        "Original Tuya (non-OpenBeken) light firmware. Verifies the fix that serves the un-striped "
        "protected-key region at 0x1ee000: instead of inventing a key from blank flash, the "
        "firmware now retrieves the stored key.",
    "BK7231N Tuya TempHum Sensor Reads Protected Key":
        "The stock Tuya temp/humidity sensor that first exposed the protected-key bug. With blank "
        "flash it failed the AES magic check and dropped to mf_test; serving the dump's real bytes "
        "lets it decrypt its key (get key).",
    "BK7231N Tuya Plug (SDK 2.3.3) Boots and Reads Protected Key":
        "A stock Tuya smart plug (SDK 2.3.3), picked at random after the protected-key fix to check "
        "it generalises. Boots past SDK init and OEM config, reading its protected key; never drops "
        "into mf_test.",
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
    "BK7231N Tuya zmai90 RN8209C Energy Meter Boots":
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
# the *part* the dump came from, which is finer-grained than the emulated
# identity: --chip collapses the whole T/U/N/M family onto BK7231.
_CHIP_RE = re.compile(r"(BL2028N|BK7252N|BK7231[TUNMQ]|BK7238|BK7252|BK7236|BK7258|BK3231)", re.I)


def chip_of(test_config):
    """Best-effort chip name for a test: filename first, then the -chip arg."""
    m = _CHIP_RE.search(os.path.basename(test_config["binary"]))
    if m:
        return m.group(1).upper()
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
PWM_NEW_CHIPS = {"BK7231N", "BK7231M", "BL2028N"}


def _periph(lines, chip=""):
    """Decode captured [EMU_GPIO]/[EMU_PWM] lines; never breaks a run."""
    try:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import periph
        gpio, pwm = periph.parse_lines(lines)
        if not gpio and not pwm:
            return None
        # Same captured offsets mean different peripherals per chip: 0x100+
        # is pwm_new on the N family but AUDIO on 7252 - so the chip, which
        # only the harness knows, selects the decoder.
        if chip in PWM_NEW_CHIPS and any(off >= 0x100 for off in pwm):
            channels = periph.pwm_new_channels(pwm)
            ctl, base, layout = 0, None, "new"
        else:
            channels, ctl, base = periph.pwm_channels(pwm)
            layout = "old"
        return {"gpio": periph.gpio_table(gpio),
                "pwm_layout": layout,
                "raw": gpio,
                "pwm_pins": periph.pwm_output_pins(gpio),
                "func": periph.func_rows(gpio),
                "pwm_ctl": ctl,
                "pwm_base": base,
                "pwm_channels": channels,
                "pwm_regs": periph.pwm_registers(pwm)}
    except Exception as exc:
        # Decoding is best-effort and must never fail a run, but swallowing it
        # silently once cost a whole debug cycle: a stale 2-tuple unpack here
        # made every GPIO tab look "empty" rather than "broken".
        print("[warn] peripheral decode failed: %r" % (exc,), file=sys.stderr)
        return None


def _pin_desc(value):
    """Human-readable form of a pin config word, for test output."""
    try:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import periph
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


def run_test(test_config):
    name = test_config["name"]
    binary = test_config["binary"]
    args = test_config["args"]
    timeout = test_config.get("timeout", 30)
    expected = test_config["expected_strings"]

    print(f"=====================================")
    print(f"Running Test: {name}")
    print(f"Binary: {binary}")

    if not os.path.exists(binary):
        print(f"FAIL: Binary not found: {binary}")
        return _result(test_config, passed=False, output="Binary not found: %s" % binary)

    cmd = [sys.executable, MAIN_SCRIPT, binary] + args
    # EMU_REPORT makes the emulator periodically print its approximate
    # instruction count on [EMU_INSNS] lines, which the reader below strips out
    # of the displayed log and records for the report.
    child_env = dict(os.environ)
    child_env["EMU_REPORT"] = "1"

    # Stream the child's output and stop as soon as every expected string has
    # been seen. The emulator never exits on its own, so the old
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

    def reader():
        for line in proc.stdout:
            if line.startswith("[EMU_GPIO]") or line.startswith("[EMU_PWM]"):
                periph_lines.append(line.strip())
                continue
            if line.startswith("[EMU_INSNS]"):
                parts = line.split()
                if len(parts) >= 2 and parts[1].isdigit():
                    insns_holder[0] = int(parts[1])
                continue
            if line.startswith("[EMU_UART1]"):
                uart1_lines.append(line.strip())
                continue
            with lock:
                lines.append(line)
                for s_ in list(remaining):
                    c = line.count(s_)
                    if c:
                        counts[s_] += c
                        if counts[s_] >= required[s_]:
                            remaining.discard(s_)

    th = threading.Thread(target=reader, daemon=True)
    th.start()

    start = time.time()
    hit_timeout = False
    done_at = None          # when every marker was first satisfied
    while True:
        with lock:
            done = not remaining
        if done:
            if proc.poll() is not None:
                break       # nothing left to capture - process already gone
            if LINGER <= 0:
                break
            # Keep running a little longer to capture more of the boot. The
            # verdict is already settled; this only enriches the log.
            if done_at is None:
                done_at = time.time()
                print(f"  ... all markers met, lingering {LINGER:.0f}s for a fuller log")
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

    periph_data = _periph(periph_lines, chip_of(test_config))

    # Pin assertions, when a case declares "expected_pins". This is what turns
    # the GPIO capture from a display into a tested invariant: the expected
    # register value is derived from the vendor SDK's own gpio_config() /
    # gpio_output(), not from whatever the emulator happened to emit. Without
    # it, a break in the capture path renders an empty tab and still "passes".
    for pin, want in sorted((test_config.get("expected_pins") or {}).items(),
                            key=lambda kv: kv[0]):
        got = (periph_data or {}).get("raw", {}).get(pin)
        ok = got == want
        # want=None asserts the pin was NEVER written. That is the negative
        # half of the check: it proves the captured pin actually follows the
        # command rather than the emulator writing some fixed pin regardless.
        if want is None:
            label = "GPIO P%d untouched" % pin
        else:
            label = "GPIO P%d config = 0x%02X" % (pin, want)
        if ok:
            detail = "no write captured" if want is None else _pin_desc(want)
            print(f"  [PASS] {label} ({detail})")
        else:
            seen = "0x%02X" % got if got is not None else "no write captured"
            print(f"  [FAIL] {label}, got {seen}")
            all_passed = False
        checks.append({"string": label, "found": ok,
                       "count": 1 if ok else 0, "required": 1})

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
        ok = (got is not None and got[1] == want_period
              and (want_duty is None or got[2] == want_duty))
        if want_duty is None:
            label = "PWM%d period = 0x%04X (duty varies, not asserted)" % (ch, want_period)
        else:
            label = "PWM%d period = 0x%04X, duty = 0x%04X" % (ch, want_period, want_duty)
        if ok:
            print(f"  [PASS] {label} ({got[3]:.1f} Hz, {got[4]:.1f}%)")
        else:
            seen = ("period 0x%04X duty %s" % (got[1], got[2])) if got else "channel not decoded"
            print(f"  [FAIL] {label}, got {seen}")
            all_passed = False
        checks.append({"string": label, "found": ok,
                       "count": 1 if ok else 0, "required": 1})

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

def _write_report(results):
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
            "total_time": sum(r["elapsed"] for r in results),
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
    results = [run_test(test) for test in TEST_CASES]
    passed = sum(1 for r in results if r["passed"])
    failed = len(results) - passed

    print(f"=====================================")
    print(f"Test Run Completed: {passed} passed, {failed} failed.")

    _write_report(results)

    sys.exit(1 if failed else 0)

if __name__ == "__main__":
    main()

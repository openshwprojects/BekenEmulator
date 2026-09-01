@echo off
REM mathDemo image with an OpenBeken config injected at flash 0x1e1000 whose
REM initCommandLine (offset 0x5E0) starts the TuyaMCU driver and binds data
REM points to channels, both ways round.
REM TuyaMCU_Init opens UART1 itself at 9600, so no uartInit is needed. OBK then
REM talks first: TuyaMCU_RunStateMachine_V3 begins with heartbeat_timer == 0, so
REM the first per-second tick sends a HEARTBEAT.
REM A simulated MCU is attached (--tuyamcu) and answers, so the handshake
REM completes and the data points below are reported back - one of every wire
REM type. DP 1 lands on channel 1, whose change handler sets channel 20, which
REM OBK must push back to the MCU as a 0x06 SET_DP frame.
REM --tuyamcu-inject is the MCU talking on its own after the handshake: a 0x04
REM and a 0x1C that OBK must answer, then a frame with a deliberately wrong
REM checksum and one behind four junk bytes that OBK must discard and recover
REM from.
REM Expect: [UART1/MCU] 55 aa 00 00 00 00 ff
REM         55 aa 00 06 00 05 14 01 00 01 01 21   (channel 20 -> DP 20)
REM Regenerate the image with:
REM   python tools\make_obk_config.py firmwares\OpenBK7231T_QIO_1.18.300_mathDemo.bin ^
REM     firmwares\OpenBK7231T_QIO_1.18.300_mathDemo_obkStartupCommand_tuyaMCU.bin ^
REM     -c "backlog startDriver TuyaMCU; linkTuyaMCUOutputToChannel 1 bool 1; linkTuyaMCUOutputToChannel 2 val 2; linkTuyaMCUOutputToChannel 4 enum 4; linkTuyaMCUOutputToChannel 20 bool 20; addChangeHandler Channel1 != 0 setChannel 20 1"
set "SIM_SCRIPT=%~dp0src\main.py"
set TARGET_FILE=%~1

if "%TARGET_FILE%"=="" (
    set "TARGET_FILE=%~dp0firmwares\OpenBK7231T_QIO_1.18.300_mathDemo_obkStartupCommand_tuyaMCU.bin"
)

echo Running simulator (UART2 log + UART1/TuyaMCU as HEX) for: %TARGET_FILE%
python "%SIM_SCRIPT%" "%TARGET_FILE%" --only-uart --uart1-hex -key TUYA ^
    --tuyamcu ^
    --tuyamcu-dp 1:bool:1 --tuyamcu-dp 2:value:100 --tuyamcu-dp 3:string:HELLO ^
    --tuyamcu-dp 4:enum:2 --tuyamcu-dp 5:bitmap:0x0F ^
    --tuyamcu-inject 55AA0004000003 --tuyamcu-inject 55AA001C00001B ^
    --tuyamcu-inject 55AA0000000000 --tuyamcu-inject DEADBEEF55AA0099000098
pause

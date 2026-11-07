@echo off
REM Native Win32 GUI front-end for the emulator: UART1 and UART2 in their own
REM consoles, with a send bar that types into UART1's receive FIFO.
REM
REM Pick any Beken dump with Browse..., or pass one as the first argument.
REM Defaults to the OpenBeken image that already has config flag 31 set
REM (OBK_FLAG_CMD_ACCEPT_UART_COMMANDS), which is the one dump here whose
REM UART1 is a command console at boot - so "setChannel 1 1" typed in the send
REM bar actually does something. Set the flag on another image with:
REM   make_obk_config.exe in.bin out.bin -c "echo hi" --flag 31
REM
REM Note: OpenBeken does not echo, and command output goes to the UART2 debug
REM log - so you type on the left and read the result on the right.
"%~dp0..\sim_ui.exe" %1

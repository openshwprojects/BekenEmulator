@echo off
REM Tkinter front-end for the emulator: UART1 and UART2 in their own consoles,
REM with a send bar that types into UART1's receive FIFO.
REM
REM Pick any Beken dump with Browse..., or pass one as the first argument.
REM Defaults to the OpenBeken image that already has config flag 31 set
REM (OBK_FLAG_CMD_ACCEPT_UART_COMMANDS), which is the one dump here whose
REM UART1 is a command console at boot - so "setChannel 1 1" typed in the send
REM bar actually does something. Set the flag on another image with:
REM   python tools\make_obk_config.py in.bin out.bin -c "echo hi" --flag 31
REM
REM Note: OpenBeken does not echo, and command output goes to the UART2 debug
REM log - so you type on the left and read the result on the right.
REM
REM Launched with python.exe (not pythonw) on purpose: the console window
REM behind the UI is where a Python traceback would show up.
python "%~dp0tools\sim_ui.py" %1
pause

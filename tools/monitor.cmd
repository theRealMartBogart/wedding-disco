@echo off
REM monitor.cmd — live RF serial monitor for the FreeWili (rf_monitor firmware).
REM Flash apps/rf_monitor first, then run this to watch decoded transmissions.
set "PICO_SDK_PATH=%USERPROFILE%\.pico-sdk\sdk\2.2.0"
set "PATH=%USERPROFILE%\.pico-sdk\toolchain\14_2_Rel1\bin;%PATH%"
set "PYTHONIOENCODING=utf-8"
echo === RF monitor (Ctrl+C to stop) ===
python "%~dp0fw.py" rtt

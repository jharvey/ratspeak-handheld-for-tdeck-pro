@echo off
REM Serial monitor on a chosen COM port (no platformio.ini change)
REM Usage:  monitor-com.bat COM9

if "%~1"=="" (
  echo Usage: monitor-com.bat COMx
  exit /b 1
)
pio device monitor --port %~1 --baud 115200

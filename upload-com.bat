@echo off
REM Upload existing firmware to a chosen COM port WITHOUT editing platformio.ini
REM Usage:  upload-com.bat COM9
REM         upload-com.bat COM8
REM Does not fullclean; only uploads the last built firmware.bin

set ENV=tdeck_pro_cooperative
if "%~1"=="" (
  echo Usage: upload-com.bat COMx
  echo Example: upload-com.bat COM9
  exit /b 1
)
set PORT=%~1
echo Uploading %ENV% to %PORT% ...
pio run -e %ENV% -t upload --upload-port %PORT%
if errorlevel 1 exit /b 1
echo Done. Monitor with:  monitor-com.bat %PORT%

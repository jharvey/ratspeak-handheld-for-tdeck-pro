@echo off
REM Build (incremental) + upload to COMx without touching platformio.ini
REM Usage:  build-upload-com.bat COM9

set ENV=tdeck_pro_cooperative
if "%~1"=="" (
  echo Usage: build-upload-com.bat COMx
  exit /b 1
)
set PORT=%~1
echo Building %ENV% then upload to %PORT% ...
pio run -e %ENV% -t upload --upload-port %PORT%

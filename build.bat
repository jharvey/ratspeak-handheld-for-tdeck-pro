@echo off
setlocal

echo ===================================================
echo 1. Compile Firmware Only
echo 2. Flash Firmware Manually
echo 3. Clean Build Files
echo ===================================================
set /p choice="Select an option (1-3): "

if "%choice%"=="1" goto do_build
if "%choice%"=="2" goto do_flash
if "%choice%"=="3" goto do_clean
goto end

:do_build
echo Building firmware...
pio run
echo.
echo ---------------------------------------------------
echo Compilation complete.
echo ---------------------------------------------------
goto end

:do_flash
echo Flashing firmware to ESP32-S3...
:: Uses PlatformIO's built-in toolchain at a stable fallback speed
pio pkg exec -p tool-esptoolpy -- esptool.py --chip esp32s3 --baud 115200 --no-stub write_flash 0x0000 .pio/build/tdeck/bootloader.bin 0x8000 .pio/build/tdeck/partitions.bin 0x10000 .pio/build/tdeck/firmware.bin
goto end

:do_clean
echo Cleaning build directory...
pio run --target clean
goto end

:end
echo Done.
endlocal
pause

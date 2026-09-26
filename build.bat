@echo off
setlocal

echo ===================================================
echo 1. Compile T-Deck Pro Only (DO THIS FIRST)
echo 2. Flash Already-Built Pro Binaries (INSTANT)
echo 3. Clean Build Files
echo 4. WIPE ALL FIRMWARE (Factory Erase)
echo ===================================================
set /p choice="Select an option (1-4): "

if "%choice%"=="1" goto do_build
if "%choice%"=="2" goto do_flash
if "%choice%"=="3" goto do_clean
if "%choice%"=="4" goto do_wipe
goto end

:do_build
echo Building T-Deck Pro firmware...
:: Added the "-e tdeck_pro" flag to command PlatformIO to run your custom block
pio run -e tdeck_pro
echo.
echo ---------------------------------------------------
echo Compilation complete. Your Pro binaries are cached!
echo Force bootloader mode (Hold Wheel + Click RST)
echo and run Option 2 to flash instantly.
echo ---------------------------------------------------
goto end

:do_flash
echo Flashing T-Deck Pro firmware instantly via esptool...
:: Updated folder paths to pull from your new .pio/build/tdeck_pro directory
pio pkg exec -p tool-esptoolpy -- esptool.py --chip esp32s3 --baud 115200 write_flash 0x0000 .pio/build/tdeck_pro/bootloader.bin 0x8000 .pio/build/tdeck_pro/partitions.bin 0x10000 .pio/build/tdeck_pro/firmware.bin
goto end

:do_clean
echo Cleaning build directory...
pio run --target clean
goto end

:do_wipe
echo [WARNING] This will completely erase all firmware and settings!
echo Ensure your T-Deck is connected in Bootloader mode (Hold trackwheel + Click RST).
pause
echo Erasing Flash Memory...
pio pkg exec -p tool-esptoolpy -- esptool.py --chip esp32s3 --baud 115200 erase_flash
echo.
echo ---------------------------------------------------
echo WIPE COMPLETE. Your T-Deck is now completely empty.
echo Run this script again and select Option 2 to flash.
echo ---------------------------------------------------
goto end

:end
echo Done.
endlocal
pause

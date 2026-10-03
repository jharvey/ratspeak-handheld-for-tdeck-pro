@echo off
setlocal enabledelayedexpansion

echo ===================================================
echo  T-Deck Pro Build Menu
echo ===================================================
echo 1. Compile Cooperative (Node mode - current working)
echo 2. Flash Cooperative binaries
echo 3. Compile Standalone (e-ink LVGL UI - new)
echo 4. Flash Standalone binaries
echo 5. Clean all build files
echo 6. WIPE ALL FIRMWARE (Factory Erase)
echo ===================================================
set /p choice="Select an option (1-6): "

if "%choice%"=="1" goto do_build_coop
if "%choice%"=="2" goto do_flash_coop
if "%choice%"=="3" goto do_build_standalone
if "%choice%"=="4" goto do_flash_standalone
if "%choice%"=="5" goto do_clean
if "%choice%"=="6" goto do_wipe
goto end

:do_build_coop
echo.
echo Building T-Deck Pro Cooperative (Node) firmware...
pio run -e tdeck_pro_cooperative
if errorlevel 1 (
    echo.
    echo *** BUILD FAILED ***
    goto end
)
echo.
echo ---------------------------------------------------
echo Cooperative build complete.
echo Binaries are in: .pio\build\tdeck_pro_cooperative\
echo Put device in bootloader mode (Hold trackwheel + RST)
echo then choose option 2 to flash.
echo ---------------------------------------------------
goto end

:do_flash_coop
echo.
echo Flashing Cooperative firmware...
pio pkg exec -p tool-esptoolpy -- esptool.py --chip esp32s3 --baud 921600 write_flash ^
  0x0000 .pio/build/tdeck_pro_cooperative/bootloader.bin ^
  0x8000 .pio/build/tdeck_pro_cooperative/partitions.bin ^
  0x10000 .pio/build/tdeck_pro_cooperative/firmware.bin
if errorlevel 1 (
    echo.
    echo *** FLASH FAILED - check cable / bootloader mode ***
) else (
    echo.
    echo Flash complete.
)
goto end

:do_build_standalone
echo.
echo Building T-Deck Pro Standalone (e-ink LVGL) firmware...
pio run -e tdeck_pro_standalone
if errorlevel 1 (
    echo.
    echo *** BUILD FAILED ***
    echo Make sure [env:tdeck_pro_standalone] exists in platformio.ini
    goto end
)
echo.
echo ---------------------------------------------------
echo Standalone (e-ink) build complete.
echo Binaries are in: .pio\build\tdeck_pro_standalone\
echo Put device in bootloader mode (Hold trackwheel + RST)
echo then choose option 4 to flash.
echo ---------------------------------------------------
goto end

:do_flash_standalone
echo.
echo Flashing Standalone (e-ink) firmware...
pio pkg exec -p tool-esptoolpy -- esptool.py --chip esp32s3 --baud 921600 write_flash ^
  0x0000 .pio/build/tdeck_pro_standalone/bootloader.bin ^
  0x8000 .pio/build/tdeck_pro_standalone/partitions.bin ^
  0x10000 .pio/build/tdeck_pro_standalone/firmware.bin
if errorlevel 1 (
    echo.
    echo *** FLASH FAILED - check cable / bootloader mode ***
) else (
    echo.
    echo Flash complete.
)
goto end

:do_clean
echo.
echo Cleaning all build files...
pio run --target clean
echo Done.
goto end

:do_wipe
echo.
echo [WARNING] This will completely erase all firmware and settings!
echo Ensure the T-Deck Pro is in Bootloader mode (Hold trackwheel + Click RST).
pause
echo Erasing flash...
pio pkg exec -p tool-esptoolpy -- esptool.py --chip esp32s3 --baud 921600 erase_flash
echo.
echo ---------------------------------------------------
echo WIPE COMPLETE. Device is empty.
echo Run this script again and flash either Cooperative or Standalone.
echo ---------------------------------------------------
goto end

:end
echo.
echo Done.
pause
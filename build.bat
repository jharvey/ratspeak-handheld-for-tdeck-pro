@echo off
setlocal

echo ===================================================
echo 1. Build Cooperative (headless serial tests)
echo 2. Flash Cooperative
echo 3. Build Display Node (e-ink status + protocol)
echo 4. Flash Display Node
echo 5. Clean Build Files
echo 6. WIPE ALL FIRMWARE (Factory Erase)
echo ===================================================
set /p choice="Select an option (1-6): "

if "%choice%"=="1" goto do_build_coop
if "%choice%"=="2" goto do_flash_coop
if "%choice%"=="3" goto do_build_node
if "%choice%"=="4" goto do_flash_node
if "%choice%"=="5" goto do_clean
if "%choice%"=="6" goto do_wipe
goto end

:do_build_coop
echo Building T-Deck Pro Cooperative...
pio run -e tdeck_pro_cooperative
echo.
echo Build done. Use option 2 to flash.
goto end

:do_flash_coop
echo Flashing Cooperative...
pio pkg exec -p tool-esptoolpy -- esptool.py --chip esp32s3 --baud 115200 write_flash 0x0000 .pio/build/tdeck_pro_cooperative/bootloader.bin 0x8000 .pio/build/tdeck_pro_cooperative/partitions.bin 0x10000 .pio/build/tdeck_pro_cooperative/firmware.bin
goto end

:do_build_node
echo Building T-Deck Pro Display Node...
pio run -e tdeck_pro_node
echo.
echo Build done. Use option 4 to flash.
goto end

:do_flash_node
echo Flashing Display Node...
pio pkg exec -p tool-esptoolpy -- esptool.py --chip esp32s3 --baud 115200 write_flash 0x0000 .pio/build/tdeck_pro_node/bootloader.bin 0x8000 .pio/build/tdeck_pro_node/partitions.bin 0x10000 .pio/build/tdeck_pro_node/firmware.bin
goto end

:do_clean
echo Cleaning...
pio run --target clean
goto end

:do_wipe
echo [WARNING] Erases all firmware and settings!
echo Bootloader mode: Hold trackwheel + Click RST
pause
pio pkg exec -p tool-esptoolpy -- esptool.py --chip esp32s3 --baud 115200 erase_flash
echo WIPE COMPLETE.
goto end

:end
echo Done.
endlocal
pause
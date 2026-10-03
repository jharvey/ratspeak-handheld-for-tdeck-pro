@echo off
setlocal enabledelayedexpansion

set LOGFILE=build_log.txt
echo. > "%LOGFILE%"

echo ===================================================
echo  T-Deck Pro Build Menu  (log -^> %LOGFILE%)
echo ===================================================
echo 1. Compile Cooperative (Node mode)
echo 2. Flash Cooperative binaries
echo 3. Compile Standalone (e-ink bring-up)
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
echo Building Cooperative... | tee -a "%LOGFILE%" 2>nul
echo Building Cooperative... >> "%LOGFILE%"
pio run -e tdeck_pro_cooperative >> "%LOGFILE%" 2>&1
set ERR=!ERRORLEVEL!
type "%LOGFILE%"
if !ERR! neq 0 (
    echo *** BUILD FAILED - see %LOGFILE% ***
) else (
    echo Build OK. Log: %LOGFILE%
)
goto end

:do_flash_coop
echo Flashing Cooperative... >> "%LOGFILE%"
pio pkg exec -p tool-esptoolpy -- esptool.py --chip esp32s3 --baud 921600 write_flash ^
  0x0000 .pio/build/tdeck_pro_cooperative/bootloader.bin ^
  0x8000 .pio/build/tdeck_pro_cooperative/partitions.bin ^
  0x10000 .pio/build/tdeck_pro_cooperative/firmware.bin >> "%LOGFILE%" 2>&1
type "%LOGFILE%"
goto end

:do_build_standalone
echo.
echo Building Standalone (log -^> %LOGFILE%)...
echo ========== STANDALONE BUILD %DATE% %TIME% ========== > "%LOGFILE%"
pio run -e tdeck_pro_standalone >> "%LOGFILE%" 2>&1
set ERR=!ERRORLEVEL!
echo.
echo ---------- last 80 lines of log ----------
powershell -Command "Get-Content '%LOGFILE%' -Tail 80"
echo ------------------------------------------
if !ERR! neq 0 (
    echo.
    echo *** BUILD FAILED ***
    echo Full log saved to: %CD%\%LOGFILE%
    echo Share that file for the next fix.
) else (
    echo.
    echo Build OK. Full log: %CD%\%LOGFILE%
)
goto end

:do_flash_standalone
echo Flashing Standalone... > "%LOGFILE%"
pio pkg exec -p tool-esptoolpy -- esptool.py --chip esp32s3 --baud 921600 write_flash ^
  0x0000 .pio/build/tdeck_pro_standalone/bootloader.bin ^
  0x8000 .pio/build/tdeck_pro_standalone/partitions.bin ^
  0x10000 .pio/build/tdeck_pro_standalone/firmware.bin >> "%LOGFILE%" 2>&1
type "%LOGFILE%"
goto end

:do_clean
echo Cleaning... > "%LOGFILE%"
pio run --target clean >> "%LOGFILE%" 2>&1
echo Done. >> "%LOGFILE%"
type "%LOGFILE%"
goto end

:do_wipe
echo [WARNING] Erase all flash? >> "%LOGFILE%"
pause
pio pkg exec -p tool-esptoolpy -- esptool.py --chip esp32s3 --baud 921600 erase_flash >> "%LOGFILE%" 2>&1
type "%LOGFILE%"
goto end

:end
echo.
echo Done. Log file: %CD%\%LOGFILE%
pause
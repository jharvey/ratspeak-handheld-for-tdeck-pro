@echo off
setlocal enabledelayedexpansion

set LOGFILE=build_log.txt

echo ===================================================
echo  T-Deck Pro Build Menu
echo  Log file: %CD%\%LOGFILE%  (overwritten each build)
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
echo Building Cooperative...
echo ========== COOPERATIVE BUILD %DATE% %TIME% ========== > "%LOGFILE%"
call :run_and_log pio run -e tdeck_pro_cooperative
goto end

:do_flash_coop
echo.
echo Flashing Cooperative...
echo ========== COOPERATIVE FLASH %DATE% %TIME% ========== > "%LOGFILE%"
call :run_and_log pio pkg exec -p tool-esptoolpy -- esptool.py --chip esp32s3 --baud 921600 write_flash 0x0000 .pio/build/tdeck_pro_cooperative/bootloader.bin 0x8000 .pio/build/tdeck_pro_cooperative/partitions.bin 0x10000 .pio/build/tdeck_pro_cooperative/firmware.bin
goto end

:do_build_standalone
echo.
echo Building Standalone (e-ink)...
echo ========== STANDALONE BUILD %DATE% %TIME% ========== > "%LOGFILE%"
call :run_and_log pio run -e tdeck_pro_standalone
if errorlevel 1 (
    echo.
    echo *** BUILD FAILED ***
    echo Full log: %CD%\%LOGFILE%
) else (
    echo.
    echo Build OK.
    echo Full log: %CD%\%LOGFILE%
)
goto end

:do_flash_standalone
echo.
echo Flashing Standalone...
echo ========== STANDALONE FLASH %DATE% %TIME% ========== > "%LOGFILE%"
call :run_and_log pio pkg exec -p tool-esptoolpy -- esptool.py --chip esp32s3 --baud 921600 write_flash 0x0000 .pio/build/tdeck_pro_standalone/bootloader.bin 0x8000 .pio/build/tdeck_pro_standalone/partitions.bin 0x10000 .pio/build/tdeck_pro_standalone/firmware.bin
goto end

:do_clean
echo.
echo Cleaning...
echo ========== CLEAN %DATE% %TIME% ========== > "%LOGFILE%"
call :run_and_log pio run --target clean
goto end

:do_wipe
echo.
echo [WARNING] This will erase all firmware.
pause
echo ========== WIPE %DATE% %TIME% ========== > "%LOGFILE%"
call :run_and_log pio pkg exec -p tool-esptoolpy -- esptool.py --chip esp32s3 --baud 921600 erase_flash
goto end

rem ---------- helper: run command, show on screen, append to log ----------
:run_and_log
rem %* = full command line
rem PowerShell tee: live console + append to log
powershell -NoProfile -Command ^
  "& { $ErrorActionPreference = 'Continue'; " ^
  "  & %* 2>&1 | ForEach-Object { " ^
  "    $line = $_; " ^
  "    Write-Host $line; " ^
  "    Add-Content -Path '%LOGFILE%' -Value $line -Encoding UTF8 " ^
  "  }; " ^
  "  exit $LASTEXITCODE " ^
  "}"
exit /b %ERRORLEVEL%

:end
echo.
echo Done. Log file: %CD%\%LOGFILE%
pause
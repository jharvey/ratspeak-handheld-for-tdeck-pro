@echo off
setlocal enabledelayedexpansion

set LOGFILE=build_log.txt

echo ===================================================
echo  T-Deck Pro Build Menu
echo  Log: %CD%\%LOGFILE%
echo ===================================================
echo 1. Compile Cooperative
echo 2. Flash Cooperative
echo 3. Compile Standalone
echo 4. Flash Standalone
echo 5. Clean
echo 6. WIPE flash
echo ===================================================
set /p choice="Select (1-6): "

if "%choice%"=="1" goto do_build_coop
if "%choice%"=="2" goto do_flash_coop
if "%choice%"=="3" goto do_build_standalone
if "%choice%"=="4" goto do_flash_standalone
if "%choice%"=="5" goto do_clean
if "%choice%"=="6" goto do_wipe
goto end

:do_build_coop
echo ========== COOPERATIVE BUILD %DATE% %TIME% ========== > "%LOGFILE%"
call :run_pio pio run -e tdeck_pro_cooperative
goto end

:do_flash_coop
echo ========== COOPERATIVE FLASH %DATE% %TIME% ========== > "%LOGFILE%"
call :run_pio pio pkg exec -p tool-esptoolpy -- esptool.py --chip esp32s3 --baud 921600 write_flash 0x0000 .pio/build/tdeck_pro_cooperative/bootloader.bin 0x8000 .pio/build/tdeck_pro_cooperative/partitions.bin 0x10000 .pio/build/tdeck_pro_cooperative/firmware.bin
goto end

:do_build_standalone
echo ========== STANDALONE BUILD %DATE% %TIME% ========== > "%LOGFILE%"
call :run_pio pio run -e tdeck_pro_standalone
goto end

:do_flash_standalone
echo ========== STANDALONE FLASH %DATE% %TIME% ========== > "%LOGFILE%"
call :run_pio pio pkg exec -p tool-esptoolpy -- esptool.py --chip esp32s3 --baud 921600 write_flash 0x0000 .pio/build/tdeck_pro_standalone/bootloader.bin 0x8000 .pio/build/tdeck_pro_standalone/partitions.bin 0x10000 .pio/build/tdeck_pro_standalone/firmware.bin
goto end

:do_clean
echo ========== CLEAN %DATE% %TIME% ========== > "%LOGFILE%"
call :run_pio pio run --target clean
goto end

:do_wipe
echo [WARNING] Erase entire flash?
pause
echo ========== WIPE %DATE% %TIME% ========== > "%LOGFILE%"
call :run_pio pio pkg exec -p tool-esptoolpy -- esptool.py --chip esp32s3 --baud 921600 erase_flash
goto end

:run_pio
rem Run with live colored output; also capture plain text to log via temporary file + type
rem PlatformIO keeps ANSI colors when writing to a real console (CON).
rem We run twice-free: pipe through powershell only for log, keep native console for colors.

echo.
echo Running: %*
echo.

rem 1) Run with colors on the real console, tee a copy without breaking ANSI too badly
powershell -NoProfile -Command ^
  "$ErrorActionPreference='Continue';" ^
  "$log='%LOGFILE%';" ^
  "$cmd = '%*';" ^
  "cmd /c $cmd 2>&1 | ForEach-Object {" ^
  "  $line = $_;" ^
  "  Write-Host $line;" ^
  "  $plain = $line -replace '\x1B\[[0-9;]*[mK]', '';" ^
  "  Add-Content -Path $log -Value $plain -Encoding UTF8" ^
  "}; exit $LASTEXITCODE"

set ERR=%ERRORLEVEL%
if %ERR% equ 0 (
  echo.
  powershell -NoProfile -Command "Write-Host '*** SUCCESS ***' -ForegroundColor Green"
  echo SUCCESS >> "%LOGFILE%"
) else (
  echo.
  powershell -NoProfile -Command "Write-Host '*** FAILED ***' -ForegroundColor Red"
  echo FAILED errorlevel=%ERR% >> "%LOGFILE%"
)
exit /b %ERR%

:end
echo.
echo Log file: %CD%\%LOGFILE%
pause
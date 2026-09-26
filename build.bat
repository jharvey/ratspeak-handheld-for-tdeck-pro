@echo off
setlocal

echo ===================================================
echo 1. Compile Firmware Only
echo 2. Flash Firmware (PlatformIO Native)
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
echo Flashing firmware via PlatformIO...
:: Uses PlatformIO's native deployment routine to manage the CDC port timing
pio run --target upload
goto end

:do_clean
echo Cleaning build directory...
pio run --target clean
goto end

:end
echo Done.
endlocal
pause

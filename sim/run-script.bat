@echo off
rem Run a simulator script headless and exit with its result (0 = PASS).
rem   run-script.bat scripts\wifi.txt [name] [fresh]
rem   fresh = delete that device's saved data first (scripts that assume defaults, e.g. power.txt)
rem   set SIM_ARGS=--nosd   extra simulator options (e.g. empty SD slot)
rem Screenshots land in sim\build\simdata\<name>\shots\ ; output in the console.
setlocal
set NAME=%2
if "%NAME%"=="" set NAME=script
if /i "%3"=="fresh" if exist "%~dp0build\simdata\%NAME%" rmdir /s /q "%~dp0build\simdata\%NAME%"
set QT_QPA_PLATFORM=offscreen
set QT_QPA_FONTDIR=%WINDIR%\Fonts
"%~dp0build\ratspeak-sim.exe" --name %NAME% --fast --script "%~1" --quit %SIM_ARGS%
exit /b %ERRORLEVEL%

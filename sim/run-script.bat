@echo off
rem Run a simulator script headless and exit with its result (0 = PASS).
rem   run-script.bat scripts\wifi.txt [name]
rem Screenshots land in sim\build\simdata\<name>\shots\ ; output in the console.
setlocal
set NAME=%2
if "%NAME%"=="" set NAME=script
set QT_QPA_PLATFORM=offscreen
set QT_QPA_FONTDIR=%WINDIR%\Fonts
"%~dp0build\ratspeak-sim.exe" --name %NAME% --fast --script "%~1" --quit
exit /b %ERRORLEVEL%

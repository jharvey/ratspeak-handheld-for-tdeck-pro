@echo off
rem Live-device mode: mirror a real T-Deck Pro's screen over USB and control it
rem from the PC keyboard / mouse.   run-live.bat [COM8] [more options]
rem The device needs firmware with the screen mirror (remote/ScreenMirror). Close
rem PuTTY / the PlatformIO monitor first: only one program can open the port.
setlocal
set PORT=%1
if "%PORT%"=="" set PORT=COM8
shift
start "" "%~dp0build\ratspeak-sim.exe" --name live-%PORT% --live %PORT% %1 %2 %3 %4 %5 %6

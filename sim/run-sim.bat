@echo off
rem Start a simulated T-Deck Pro.   run-sim.bat [name] [more options]
rem   run-sim.bat              -> device "device1"
rem   run-sim.bat alice        -> device "alice" (own identity, NVS, flash, SD)
rem Start two with different names to message between them.
setlocal
set NAME=%1
if "%NAME%"=="" set NAME=device1
shift
start "" "%~dp0build\ratspeak-sim.exe" --name %NAME% %1 %2 %3 %4 %5 %6

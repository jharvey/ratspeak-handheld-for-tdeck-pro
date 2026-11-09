@echo off
rem Build the RatSpeak T-Deck Pro simulator (Windows, Qt MinGW kit).
rem   build-sim.bat            configure + build sim\build\ratspeak-sim.exe
rem Prereqs (see sim\README.md): Qt 6 + MinGW + CMake + Ninja under %QTROOT%,
rem Rust, the sibling repos ..\..\rsReticulumLite and ..\..\rsLXMFLite, and one
rem PlatformIO build of tdeck_pro_standalone (for .pio\libdeps lvgl/ArduinoJson).
setlocal
if "%QTROOT%"=="" set QTROOT=%USERPROFILE%\Qt
if "%QTVER%"=="" set QTVER=6.10.3
set PATH=%QTROOT%\Tools\mingw1310_64\bin;%QTROOT%\Tools\Ninja;%QTROOT%\Tools\CMake_64\bin;%QTROOT%\%QTVER%\mingw_64\bin;%USERPROFILE%\.cargo\bin;%PATH%
cd /d "%~dp0"

echo == Rust protocol (host build)
pushd ..\protocol
cargo build --release --locked --lib || goto :fail
popd

echo == Simulator
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=%QTROOT%\%QTVER%\mingw_64 || goto :fail
cmake --build build -j 8 || goto :fail
windeployqt --no-translations --no-system-d3d-compiler --no-opengl-sw build\ratspeak-sim.exe >nul || goto :fail
copy /y %QTROOT%\%QTVER%\mingw_64\plugins\platforms\qoffscreen.dll build\platforms\ >nul
for %%d in (libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll) do copy /y %QTROOT%\Tools\mingw1310_64\bin\%%d build\ >nul
echo.
echo Built sim\build\ratspeak-sim.exe   (run-sim.bat to start)
exit /b 0
:fail
echo BUILD FAILED
exit /b 1

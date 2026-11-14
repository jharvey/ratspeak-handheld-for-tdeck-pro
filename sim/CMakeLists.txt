cmake_minimum_required(VERSION 3.21)
project(RatSpeakSim LANGUAGES C CXX)

# RatSpeak T-Deck Pro desktop simulator.
# Compiles the real firmware (StandaloneApp, keyboard driver, LVGL, core storage /
# protocol, Rust protocol library) against host shims of the Arduino / ESP-IDF APIs,
# and shows it in a Qt window drawn like the device.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)          # firmware builds with -std=gnu++17
set(CMAKE_AUTOMOC ON)

get_filename_component(REPO ${CMAKE_CURRENT_SOURCE_DIR}/.. ABSOLUTE)
set(SRC   ${REPO}/src)
set(BOARD ${SRC}/boards/tdeck_Pro)
set(LIBDEPS ${REPO}/.pio/libdeps/tdeck_pro_standalone)
set(LVGL_DIR ${LIBDEPS}/lvgl CACHE PATH "lvgl 8.3.11 source (PlatformIO libdeps)")
set(AJSON_DIR ${LIBDEPS}/ArduinoJson CACHE PATH "ArduinoJson 7 source")
set(RUST_LIB ${REPO}/protocol/target/release/libratspeak_protocol.a CACHE FILEPATH
    "Host build of the Rust protocol (cargo build --release --lib in protocol/)")

find_package(Qt6 REQUIRED COMPONENTS Widgets Network SerialPort)

# Checked build (build-sim.bat checked -> simuild-checked): catches memory and logic
# errors that do not always crash. MinGW GCC has no AddressSanitizer, so this uses what
# it has: undefined-behaviour checks incl. array bounds (trap = the process stops at the
# fault), libstdc++ assertions (out-of-range [], bad iterators, empty front()), stack
# protector (buffer overruns on the stack) and _FORTIFY_SOURCE (overflowing memcpy/str*).
option(SIM_CHECKED "Build the firmware with run-time checks" OFF)
set(CHECK_FLAGS "")
if(SIM_CHECKED)
    set(CHECK_FLAGS -O1 -g -fno-omit-frame-pointer -fsanitize=undefined -fsanitize-undefined-trap-on-error
        -fstack-protector-strong -D_FORTIFY_SOURCE=2 -D_GLIBCXX_ASSERTIONS)
endif()

# ---- Firmware-side compile settings (shared) -------------------------------------
set(FW_DEFS
    SIMULATOR=1
    TDECK_PRO_BUILD=1
    DISPLAY_WIDTH=240
    DISPLAY_HEIGHT=320
    BOARD_HAS_PSRAM=1
    LV_CONF_INCLUDE_SIMPLE=1
    STORAGE_DEFERRED_IO=1
    ARDUINO=10819
    ESP32=1
    CORE_DEBUG_LEVEL=0)
set(FW_INCS
    ${CMAKE_CURRENT_SOURCE_DIR}/shim/include     # must win over any real header
    ${REPO}
    ${SRC}
    ${SRC}/core
    ${SRC}/ui/lvgl                               # lv_conf.h
    ${BOARD}
    ${REPO}/protocol/include
    ${LVGL_DIR}
    ${LVGL_DIR}/src
    ${AJSON_DIR}/src)

# ---- LVGL ----------------------------------------------------------------------------
file(GLOB_RECURSE LVGL_SRCS CONFIGURE_DEPENDS ${LVGL_DIR}/src/*.c)
add_library(sim_lvgl STATIC ${LVGL_SRCS})
target_compile_definitions(sim_lvgl PRIVATE ${FW_DEFS})
target_include_directories(sim_lvgl PRIVATE ${FW_INCS})
target_compile_options(sim_lvgl PRIVATE -w ${CHECK_FLAGS})

# ---- Firmware (real sources) + shims -----------------------------------------------
set(FW_SRCS
    ${BOARD}/main.cpp
    ${BOARD}/StandaloneApp.cpp
    ${BOARD}/hal/Keyboard.cpp
    ${BOARD}/Eink/LvglPort.cpp
    ${BOARD}/Eink/LvglStubs.cpp
    ${BOARD}/security/SecretVault.cpp
    ${BOARD}/remote/ScreenMirror.cpp
    ${BOARD}/power/PowerModes.cpp
    ${BOARD}/power/LightSleep.cpp
    ${BOARD}/power/RtcLog.cpp
    ${BOARD}/power/EnergyMonitor.cpp
    ${BOARD}/MapView.cpp
    ${BOARD}/Chess.cpp)
file(GLOB CORE_SRCS CONFIGURE_DEPENDS
    ${SRC}/core/storage/*.cpp
    ${SRC}/core/reticulum/*.cpp
    ${SRC}/core/protocol/*.cpp
    ${SRC}/core/util/*.cpp
    ${SRC}/core/history/*.cpp
    ${SRC}/core/config/*.cpp
    ${SRC}/core/transport/LoRaInterface.cpp
    ${SRC}/core/transport/WiFiInterface.cpp
    ${SRC}/core/hal/SharedSPIBus.cpp)
file(GLOB SHIM_SRCS CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/shim/src/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/hw/*.cpp)

# Inflate for map pack files (the real unit uses the copy of miniz in the ESP32-S3's ROM).
add_library(sim_miniz STATIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniz/miniz.c)
target_compile_options(sim_miniz PRIVATE -w)
target_compile_definitions(sim_miniz PRIVATE MINIZ_NO_ARCHIVE_APIS MINIZ_NO_TIME)

add_library(sim_firmware STATIC ${FW_SRCS} ${CORE_SRCS} ${SHIM_SRCS})
target_include_directories(sim_firmware PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniz)
target_link_libraries(sim_firmware PUBLIC sim_miniz)
target_compile_definitions(sim_firmware PRIVATE ${FW_DEFS})
target_include_directories(sim_firmware PRIVATE ${FW_INCS} ${CMAKE_CURRENT_SOURCE_DIR})
# newlib/POSIX extras + device clock for every firmware translation unit
target_compile_options(sim_firmware PRIVATE -include sim_libc.h -fexceptions -Wno-unused-parameter
    -Wno-missing-field-initializers -Wno-deprecated-declarations ${CHECK_FLAGS})
target_link_libraries(sim_firmware PUBLIC ${RUST_LIB})
if(WIN32)
    target_link_libraries(sim_firmware PUBLIC bcrypt ws2_32 userenv ntdll)
endif()

# ---- Qt application ------------------------------------------------------------------
add_executable(ratspeak-sim
    app/main.cpp
    app/DeviceWidget.cpp app/DeviceWidget.h
    app/MainWindow.cpp app/MainWindow.h
    app/FirmwareRunner.cpp app/FirmwareRunner.h
    app/LiveLink.cpp app/LiveLink.h)
# LVGL calls back into the firmware (custom allocator, failure hook, font), so the
# two archives are linked as a group.
target_link_libraries(ratspeak-sim PRIVATE "$<LINK_GROUP:RESCAN,sim_firmware,sim_lvgl>"
    Qt6::Widgets Qt6::Network Qt6::SerialPort)
target_include_directories(ratspeak-sim PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# (mingw-w64 provides the stack-protector / fortify runtime itself: no -lssp)

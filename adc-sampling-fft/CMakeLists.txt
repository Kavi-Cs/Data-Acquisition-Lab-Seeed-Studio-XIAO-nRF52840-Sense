# CMakeLists.txt - IDE-integration wrapper around the PlatformIO build.
#
# Why a wrapper and not a native CMake/CMSIS build
# -------------------------------------------------
# firmware/src/sampling.cpp targets the Arduino API (Serial, millis,
# delay) and Adafruit_TinyUSB's CDC wrapper, both supplied by
# PlatformIO's "arduino" framework for the nRF52840. PlatformIO already
# knows how to assemble the correct arm-none-eabi toolchain invocation,
# linker script, and UF2/bootloader packaging for the XIAO nRF52840
# Sense; platformio.ini is therefore the single source of truth for the
# build.
#
# This file does NOT reimplement that as a from-scratch nRF5
# SDK / bare-metal CMSIS build (which would require rewriting Serial,
# millis() and the USB CDC stack against Nordic's SDK). Instead it
# exposes the same actions as CMake custom targets, so the project can
# be opened and indexed in CMake-aware IDEs (CLion, VS Code CMake
# Tools, ...) without maintaining a second, parallel build description
# that could drift out of sync with platformio.ini.
#
# Usage:
#   cmake -B build -S .
#   cmake --build build --target firmware   # compile
#   cmake --build build --target upload     # flash over USB
#   cmake --build build --target monitor    # open serial monitor
#   cmake --build build --target firmware_clean

cmake_minimum_required(VERSION 3.20)
project(xiao_adc_sampling_fft NONE)   # NONE: no native compiler needed here

find_program(PIO_EXECUTABLE pio)
if(NOT PIO_EXECUTABLE)
    message(FATAL_ERROR
        "PlatformIO Core ('pio') not found on PATH. Install it with "
        "'pip install platformio' (or see https://platformio.org/install/cli), "
        "then re-run cmake.")
endif()

set(PIO_ENV "xiao_nrf52840_sense" CACHE STRING "PlatformIO environment to build (see platformio.ini)")
set(FW_PORT "" CACHE STRING "Serial port override for upload/monitor, e.g. /dev/ttyACM0 (optional)")

if(FW_PORT)
    set(_PORT_ARGS --upload-port ${FW_PORT})
    set(_MONITOR_PORT_ARGS -p ${FW_PORT})
else()
    set(_PORT_ARGS "")
    set(_MONITOR_PORT_ARGS "")
endif()

add_custom_target(firmware
    COMMAND ${PIO_EXECUTABLE} run -e ${PIO_ENV}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Building firmware via PlatformIO (env: ${PIO_ENV})"
    VERBATIM
)

add_custom_target(upload
    COMMAND ${PIO_EXECUTABLE} run -e ${PIO_ENV} -t upload ${_PORT_ARGS}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Flashing firmware via PlatformIO (env: ${PIO_ENV})"
    VERBATIM
)

add_custom_target(monitor
    COMMAND ${PIO_EXECUTABLE} device monitor -b 115200 ${_MONITOR_PORT_ARGS}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Opening serial monitor at 115200 baud"
    VERBATIM
)

add_custom_target(firmware_clean
    COMMAND ${PIO_EXECUTABLE} run -e ${PIO_ENV} -t clean
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Cleaning PlatformIO build artifacts"
    VERBATIM
)

# Once PlatformIO has built at least once, it writes a
# compile_commands.json that IDE indexers (clangd, CLion) can use for
# accurate code completion against the real nRF52840/TinyUSB headers.
set(PIO_COMPILE_COMMANDS "${CMAKE_SOURCE_DIR}/.pio/build/${PIO_ENV}/compile_commands.json")
if(EXISTS ${PIO_COMPILE_COMMANDS})
    message(STATUS "PlatformIO compile_commands.json available for IDE indexing: ${PIO_COMPILE_COMMANDS}")
else()
    message(STATUS "Run 'cmake --build build --target firmware' once to generate compile_commands.json for IDE indexing.")
endif()

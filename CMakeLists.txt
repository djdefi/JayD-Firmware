cmake_minimum_required(VERSION 3.15)
project(JayD-Firmware)
set(PORT /dev/ttyUSB0)
set(DEVICE cm:esp32:jayd) # Arduino device FQBN
set(CMAKE_CXX_STANDARD 11)

set(ROOT ${CMAKE_SOURCE_DIR})
if (WIN32 OR CYGWIN)
    set(Arduino15 $ENV{APPDATA}/../Local/Arduino15)
    set(ArduinoLibs $ENV{USERPROFILE}/OneDrive/Documents/Arduino/libraries)

    string(REPLACE "\\" "/" Arduino15 ${Arduino15})
    string(REPLACE "\\" "/" ArduinoLibs ${ArduinoLibs})

    if (CYGWIN)
        string(REGEX REPLACE "^([A-Za-z]):" "/cygdrive/\\1" Arduino15 ${Arduino15})
        string(REGEX REPLACE "^([A-Za-z]):" "/cygdrive/\\1" ArduinoLibs ${ArduinoLibs})
        string(REGEX REPLACE "^/cygdrive/([A-Za-z])" "\\1:/" ROOT ${ROOT})
    endif ()
else ()
    set(Arduino15 ~/.arduino15)
    set(ArduinoLibs ~/Arduino/libraries)
endif ()

# Include directories
include_directories(${Arduino15}/packages/cm/hardware/esp32/1.7.3/libraries/Wire/src)
include_directories(${Arduino15}/packages/cm/hardware/esp32/1.7.3/libraries/FS/src)
include_directories(${Arduino15}/packages/cm/hardware/esp32/1.7.3/libraries/SD/src)
include_directories(${Arduino15}/packages/cm/hardware/esp32/1.7.3/libraries/SPI/src)
include_directories(${Arduino15}/packages/cm/hardware/esp32/1.7.3/libraries/WiFi/src)
include_directories(${Arduino15}/packages/cm/hardware/esp32/1.7.3/libraries/SPIFFS/src)

include_directories(${Arduino15}/packages/cm/hardware/esp32/1.7.3/cores/esp32)
include_directories(${Arduino15}/packages/cm/hardware/esp32/1.7.3/tools/sdk)
include_directories(${Arduino15}/packages/cm/hardware/esp32/1.7.3/tools/sdk/include)
include_directories(${Arduino15}/packages/cm/hardware/esp32/1.7.3/tools/sdk/include/esp32)
include_directories(${Arduino15}/packages/cm/hardware/esp32/1.7.3/tools/sdk/include/freertos)
include_directories(${Arduino15}/packages/cm/hardware/esp32/1.7.3/tools/sdk/include/driver)
include_directories(${Arduino15}/packages/cm/hardware/esp32/1.7.3/tools/sdk/include/driver/driver)
include_directories(${Arduino15}/packages/cm/hardware/esp32/1.7.3/tools/sdk/include/spi_flash)
include_directories(${Arduino15}/packages/cm/hardware/esp32/1.7.3/libraries/FDK-AAC/src)
include_directories(${Arduino15}/packages/cm/hardware/esp32/1.7.3/libraries/JayD-Library/src)
include_directories(${Arduino15}/packages/cm/hardware/esp32/1.7.3/libraries/LovyanGFX/src)
include_directories(${Arduino15}/packages/cm/hardware/esp32/1.7.3/libraries/CircuitOS/src)

# Override to point host-only self-checks (DjRecordingSelfCheck) at a
# specific JayD-Library checkout's src/ directory, e.g. a pinned SHA under
# review, instead of whatever core version happens to be installed:
#   cmake -DJAYD_LIBRARY_SRC=/path/to/JayD-Library/src ..
if (NOT DEFINED JAYD_LIBRARY_SRC)
    set(JAYD_LIBRARY_SRC ${Arduino15}/packages/cm/hardware/esp32/1.7.3/libraries/JayD-Library/src)
endif ()


# Source directories
file(GLOB_RECURSE SOURCES "src/**.cpp" "src/**.c" "src/**.hpp" "src/**.h")


# Defines
add_definitions(-DARDUINO)
add_definitions(-DDEBUG)
add_definitions(-DESP32)
add_definitions(-DCIRCUITOS_FREERTOS)
add_definitions(-DCIRCUITOS_LOVYANGFX)
add_definitions(-DLOVYAN_MANUAL)


set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/build)
set(BUILD ${ROOT}/build)

SET_SOURCE_FILES_PROPERTIES(${CMAKE_PROJECT_NAME}.ino PROPERTIES LANGUAGE CXX)

add_custom_command(OUTPUT ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${CMAKE_PROJECT_NAME}.ino.elf
        COMMAND arduino-cli compile --fqbn ${DEVICE} --output-dir ${CMAKE_RUNTIME_OUTPUT_DIRECTORY} ${CMAKE_PROJECT_NAME}.ino
        DEPENDS ${CMAKE_PROJECT_NAME}.ino ${SOURCES}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_custom_target(CMBuild
        SOURCES ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${CMAKE_PROJECT_NAME}.ino.elf)

add_custom_target(CMUpload ALL
        COMMAND arduino-cli upload --fqbn ${DEVICE} -p ${PORT} --input-dir ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}
        DEPENDS CMBuild
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(${CMAKE_PROJECT_NAME}.bin ${CMAKE_PROJECT_NAME}.ino ${SOURCES})

enable_testing()
add_executable(LibraryIndexSelfCheck
        tests/library_index_selfcheck.cpp
        src/Screens/SongList/LibraryIndex.cpp)
set_target_properties(LibraryIndexSelfCheck PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME LibraryIndexSelfCheck COMMAND LibraryIndexSelfCheck)

add_executable(DjSessionSelfCheck tests/dj_session_self_test.cpp)
set_target_properties(DjSessionSelfCheck PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME DjSessionSelfCheck COMMAND DjSessionSelfCheck)

add_executable(DjBeatEngineSelfCheck tests/dj_beat_engine_selfcheck.cpp)
set_target_properties(DjBeatEngineSelfCheck PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME DjBeatEngineSelfCheck COMMAND DjBeatEngineSelfCheck)

add_executable(MetadataIntegrationSelfCheck
        tests/metadata_integration_selfcheck.cpp
        src/Screens/SongList/LibraryIndex.cpp)
set_target_properties(MetadataIntegrationSelfCheck PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME MetadataIntegrationSelfCheck COMMAND MetadataIntegrationSelfCheck)

# Coach/suggestions/one-shot-transition engine. Hardware-free by design (see
# src/DjAssist/DjAssistTypes.h), so it is held to a stricter bar than the
# other self-checks: warnings-as-errors plus ASan/UBSan on compilers that
# support it.
add_executable(DjAssistSelfCheck
        tests/dj_assist_selfcheck.cpp
        src/DjAssist/DjAssistScoring.cpp
        src/DjAssist/DjAssistEngine.cpp)
set_target_properties(DjAssistSelfCheck PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
target_compile_options(DjAssistSelfCheck PRIVATE -Wall -Wextra -Werror)
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(DjAssistSelfCheck PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(DjAssistSelfCheck PRIVATE -fsanitize=address,undefined)
endif ()
add_test(NAME DjAssistSelfCheck COMMAND DjAssistSelfCheck)

# Recording-workflow logic (naming/collision/exhaustion, WAV header repair,
# DjRecordingSnapshot/State shape). Pure logic compiled directly against the
# pinned JayD-Library headers (WavHeader.h, AudioSetup.hpp) via
# JAYD_LIBRARY_SRC, never vendored copies.
add_executable(DjRecordingSelfCheck tests/dj_recording_self_test.cpp)
target_include_directories(DjRecordingSelfCheck PRIVATE ${JAYD_LIBRARY_SRC})
set_target_properties(DjRecordingSelfCheck PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
target_compile_options(DjRecordingSelfCheck PRIVATE -Wall -Wextra -Werror)
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(DjRecordingSelfCheck PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(DjRecordingSelfCheck PRIVATE -fsanitize=address,undefined)
endif ()
add_test(NAME DjRecordingSelfCheck COMMAND DjRecordingSelfCheck)

# Wireless semantic API v2 host logic (client-id/session identity matching,
# duplicate command detection, pairing token store). Built with
# JAYD_ENABLE_WIRELESS so the wireless-only DjCommand/DjCommandResult fields
# and djCommandIdentityMatches() are compiled in, matching a wireless-enabled
# firmware build; the default (non-wireless) build never compiles this code.
add_executable(WirelessApiSelfCheck
        tests/wireless_api_self_test.cpp
        src/Wireless/WirelessApiCore.cpp)
set_target_properties(WirelessApiSelfCheck PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
target_compile_definitions(WirelessApiSelfCheck PRIVATE JAYD_ENABLE_WIRELESS)
target_compile_options(WirelessApiSelfCheck PRIVATE -Wall -Wextra -Werror)
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(WirelessApiSelfCheck PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(WirelessApiSelfCheck PRIVATE -fsanitize=address,undefined)
endif ()
add_test(NAME WirelessApiSelfCheck COMMAND WirelessApiSelfCheck)

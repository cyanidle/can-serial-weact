cmake_minimum_required(VERSION 3.21)
project(qtserialcan-weact VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(SERIALCAN_BUILD_TESTS "Build the protocol codec tests" ON)

find_package(Qt6 REQUIRED COMPONENTS Core SerialPort SerialBus)

add_library(serialcan MODULE
    src/plugin.cpp
    src/serialcanbackend.cpp
    src/slcan.cpp
)

target_compile_definitions(serialcan PRIVATE QT_NO_CAST_FROM_ASCII QT_NO_CAST_TO_ASCII)
target_link_libraries(serialcan PRIVATE Qt6::Core Qt6::SerialPort Qt6::SerialBus)

set_target_properties(serialcan PROPERTIES
    PREFIX ""
    OUTPUT_NAME "serialcan"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/canbus"
)

include(GNUInstallDirs)

set(SERIALCAN_PLUGIN_DIR "${QT6_INSTALL_PLUGINS}/canbus"
    CACHE PATH "Directory the QCanBus plugin is installed into")

install(TARGETS serialcan
    LIBRARY DESTINATION "${SERIALCAN_PLUGIN_DIR}"
)

if (SERIALCAN_BUILD_TESTS)
    enable_testing()
    add_executable(slcan_codec_test tests/codec_test.cpp src/slcan.cpp)
    target_include_directories(slcan_codec_test PRIVATE src)
    target_link_libraries(slcan_codec_test PRIVATE Qt6::Core Qt6::SerialBus)
    add_test(NAME slcan_codec COMMAND slcan_codec_test)

    add_executable(serialcan_probe tests/probe.cpp src/serialcanbackend.cpp src/slcan.cpp)
    target_include_directories(serialcan_probe PRIVATE src)
    target_link_libraries(serialcan_probe PRIVATE Qt6::Core Qt6::SerialPort Qt6::SerialBus)
endif()

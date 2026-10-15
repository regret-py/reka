cmake_minimum_required(VERSION 3.22)

project(REKA
    VERSION 0.1.0
    DESCRIPTION "REKA - low-latency modular audio mixer, VST3 host and hardware controller suite"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(REKA_BUILD_APP          "Build REKA.exe and REKA-PluginHost (requires JUCE)" ON)
option(REKA_BUILD_TESTS        "Build the reka_core unit tests" ON)
option(REKA_ENABLE_ASIO        "Enable the ASIO backend (requires the Steinberg ASIO SDK)" OFF)
option(REKA_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(REKA_BUILD_INTEGRATION_TESTS "Build the plugin-hosting integration test (needs REKA_BUILD_APP)" OFF)
set(REKA_ASIO_SDK_DIR "" CACHE PATH "Steinberg ASIO SDK root (the folder that contains common/iasiodrv.h)")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(RekaCompilerSettings)
include(RekaDependencies)

# All executables land next to each other so REKA.exe can find REKA-PluginHost.exe.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

add_subdirectory(src/core)

if(REKA_BUILD_APP)
    add_subdirectory(src/app)
    add_subdirectory(src/pluginhost)
endif()

if(REKA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(REKA_BUILD_APP AND REKA_BUILD_INTEGRATION_TESTS)
    enable_testing()
    add_subdirectory(tests/integration)
endif()

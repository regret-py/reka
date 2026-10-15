# reka_core: the JUCE-free heart of REKA.
#   real-time engine, DSP, lock-free primitives, profiles, hardware protocol bridge and the
#   platform layer (serial ports, virtual audio, plugin-sandbox IPC, thread priorities).
# Builds and unit-tests on Windows, Linux and macOS.

add_library(reka_core STATIC
    reka/common/Base64.cpp
    reka/common/Types.cpp
    reka/lockfree/AudioRingBuffer.cpp
    reka/dsp/Biquad.cpp
    reka/dsp/DelayLine.cpp
    reka/dsp/LoudnessMeter.cpp
    reka/dsp/MeterTap.cpp
    reka/dsp/ParametricEq.cpp
    reka/dsp/SincResampler.cpp
    reka/io/AsyncEndpointPort.cpp
    reka/io/DirectDevicePort.cpp
    reka/io/SoftwareClock.cpp
    reka/engine/MixerModel.cpp
    reka/engine/RekaAudioEngine.cpp
    reka/engine/StripProcessor.cpp
    reka/profile/Profile.cpp
    reka/hardware/RekaHardwareController.cpp
    reka/hardware/SerialPortCommon.cpp
    reka/platform/SharedAudioChannel.cpp
    reka/platform/ThreadPriority.cpp
    reka/platform/WindowBackdrop.cpp
    reka/virtualaudio/RekaVirtualAudio.cpp
)

if(WIN32)
    file(GLOB REKA_VIRTUALAUDIO_WIN_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/reka/virtualaudio/win/*.cpp")
    target_sources(reka_core PRIVATE
        reka/platform/SerialPortWin32.cpp
        ${REKA_VIRTUALAUDIO_WIN_SOURCES})
    target_link_libraries(reka_core PUBLIC avrt ole32 oleaut32 uuid mmdevapi dwmapi setupapi advapi32 shell32)
else()
    target_sources(reka_core PRIVATE reka/platform/SerialPortPosix.cpp)
    find_package(Threads REQUIRED)
    target_link_libraries(reka_core PUBLIC Threads::Threads)
    if(UNIX AND NOT APPLE)
        target_link_libraries(reka_core PUBLIC rt)
    endif()
endif()

target_include_directories(reka_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/firmware/REKA")  # RekaProtocol.h, shared with REKA.ino

target_compile_features(reka_core PUBLIC cxx_std_20)
target_link_libraries(reka_core
    PUBLIC  nlohmann_json::nlohmann_json
    PRIVATE reka::compiler_settings)

set_target_properties(reka_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_library(reka::core ALIAS reka_core)

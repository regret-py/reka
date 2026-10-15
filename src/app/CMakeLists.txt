# REKA.exe - the JUCE application: device management, VST3 hosting, GUI engine, control-surface glue.

juce_add_gui_app(REKA
    PRODUCT_NAME "REKA"
    COMPANY_NAME "REKA Audio"
    BUNDLE_ID "audio.reka.mixer"
    VERSION ${PROJECT_VERSION}
    NEEDS_CURL FALSE
    NEEDS_WEB_BROWSER FALSE)

file(GLOB REKA_THEME_FILES CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/resources/themes/*.json")
juce_add_binary_data(RekaBinaryData
    NAMESPACE RekaBinary
    SOURCES
        ${REKA_THEME_FILES}
        "${PROJECT_SOURCE_DIR}/resources/profiles/default.reka")

target_sources(REKA PRIVATE
    Main.cpp
    AppServices.cpp
    AppSettings.cpp
    ProfileService.cpp
    audio/RekaDeviceManager.cpp
    audio/VirtualIoManager.cpp
    hardware/MidiControllerBridge.cpp
    vst/RekaVstHost.cpp
    vst/JuceInsertProcessor.cpp
    vst/SandboxedInsertProcessor.cpp
    vst/PluginBlockAdapter.cpp
    gui/RekaGuiEngine.cpp
    gui/Theme.cpp
    gui/RekaLookAndFeel.cpp
    gui/MainComponent.cpp
    gui/components/MeterComponent.cpp
    gui/components/GainControl.cpp
    gui/components/StripComponent.cpp
    gui/components/MixerPanel.cpp
    gui/components/RoutingMatrixPanel.cpp
    gui/components/EqEditor.cpp
    gui/components/InsertRack.cpp
    gui/components/PluginBrowserPanel.cpp
    gui/components/SettingsPanel.cpp
    gui/components/ThemeEditorPanel.cpp)

target_include_directories(REKA PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

target_compile_definitions(REKA PRIVATE
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:REKA,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:REKA,JUCE_VERSION>"
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_AU=0
    JUCE_PLUGINHOST_LV2=0
    JUCE_PLUGINHOST_LADSPA=0
    JUCE_WASAPI=1
    JUCE_DIRECTSOUND=1
    JUCE_ASIO=$<BOOL:${REKA_ENABLE_ASIO}>
    JUCE_JACK=0
    JUCE_USE_FLAC=0
    JUCE_USE_OGGVORBIS=0
    JUCE_USE_MP3AUDIOFORMAT=0
    JUCE_USE_LAME_AUDIO_FORMAT=0
    JUCE_USE_WINDOWS_MEDIA_FORMAT=0)

if(REKA_ENABLE_ASIO)
    if(NOT EXISTS "${REKA_ASIO_SDK_DIR}/common/iasiodrv.h")
        message(FATAL_ERROR "REKA_ENABLE_ASIO=ON needs REKA_ASIO_SDK_DIR pointing at the Steinberg ASIO SDK")
    endif()
    target_include_directories(REKA PRIVATE "${REKA_ASIO_SDK_DIR}/common")
endif()

target_link_libraries(REKA
    PRIVATE
        reka::core
        RekaBinaryData
        juce::juce_audio_utils
        juce::juce_audio_processors
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# The plugin sandbox / scanner executable must sit next to REKA.exe.
add_dependencies(REKA REKAPluginHost)

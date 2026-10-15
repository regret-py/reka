# REKA-PluginHost.exe - out-of-process plugin scanner and sandbox.
#   REKA-PluginHost --scan <format> <file-or-identifier> <output.xml>   (one plugin file per process)
#   REKA-PluginHost <JUCE coordinator command line>                       (hosts one sandboxed plugin)
# A crashing or hanging plugin only takes this process down; REKA.exe keeps running and passes audio dry.

juce_add_gui_app(REKAPluginHost
    PRODUCT_NAME "REKA-PluginHost"
    COMPANY_NAME "REKA Audio"
    BUNDLE_ID "audio.reka.pluginhost"
    VERSION ${PROJECT_VERSION}
    NEEDS_CURL FALSE
    NEEDS_WEB_BROWSER FALSE)

target_sources(REKAPluginHost PRIVATE
    PluginHostMain.cpp
    "${PROJECT_SOURCE_DIR}/src/app/vst/PluginBlockAdapter.cpp")

target_include_directories(REKAPluginHost PRIVATE "${PROJECT_SOURCE_DIR}/src/app")

target_compile_definitions(REKAPluginHost PRIVATE
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:REKAPluginHost,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:REKAPluginHost,JUCE_VERSION>"
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_AU=0
    JUCE_PLUGINHOST_LV2=0
    JUCE_PLUGINHOST_LADSPA=0)

target_link_libraries(REKAPluginHost
    PRIVATE
        reka::core
        juce::juce_audio_utils
        juce::juce_audio_processors
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

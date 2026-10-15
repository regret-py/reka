# Plugin-hosting integration test: builds a tiny VST3 and drives RekaVstHost against it (scan in a
# separate process, in-process and sandboxed hosting, state, latency, crash containment).
juce_add_plugin(RekaTestGain
    PRODUCT_NAME "REKA Test Gain"
    COMPANY_NAME "REKA Audio"
    PLUGIN_MANUFACTURER_CODE Reka
    PLUGIN_CODE Rtg1
    FORMATS VST3
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE)
target_sources(RekaTestGain PRIVATE TestGainPlugin.cpp)
target_compile_definitions(RekaTestGain PUBLIC JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
target_link_libraries(RekaTestGain PRIVATE juce::juce_audio_utils PUBLIC juce::juce_recommended_config_flags)

juce_add_console_app(PluginHostingTest PRODUCT_NAME "PluginHostingTest")
target_sources(PluginHostingTest PRIVATE
    PluginHostingTest.cpp
    "${PROJECT_SOURCE_DIR}/src/app/vst/RekaVstHost.cpp"
    "${PROJECT_SOURCE_DIR}/src/app/vst/JuceInsertProcessor.cpp"
    "${PROJECT_SOURCE_DIR}/src/app/vst/SandboxedInsertProcessor.cpp"
    "${PROJECT_SOURCE_DIR}/src/app/vst/PluginBlockAdapter.cpp")
target_include_directories(PluginHostingTest PRIVATE "${PROJECT_SOURCE_DIR}/src/app")
target_compile_definitions(PluginHostingTest PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_PLUGINHOST_VST3=1 JUCE_PLUGINHOST_AU=0 JUCE_PLUGINHOST_LV2=0 JUCE_PLUGINHOST_LADSPA=0
    JUCE_MODAL_LOOPS_PERMITTED=1)  # the test pumps the message loop while waiting
target_link_libraries(PluginHostingTest PRIVATE reka::core juce::juce_audio_utils juce::juce_audio_processors juce::juce_gui_extra
                      PUBLIC juce::juce_recommended_config_flags)
add_dependencies(PluginHostingTest RekaTestGain_VST3 REKAPluginHost)

add_test(NAME plugin_hosting
    COMMAND PluginHostingTest "${CMAKE_CURRENT_BINARY_DIR}/RekaTestGain_artefacts/$<CONFIG>/VST3" "$<TARGET_FILE:REKAPluginHost>")

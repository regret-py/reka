add_executable(reka_tests
    main.cpp
    test_lockfree.cpp
    test_dsp.cpp
    test_resampler.cpp
    test_engine.cpp
    test_profile.cpp
    test_protocol.cpp
    test_hardware.cpp
    test_sandbox_channel.cpp
)
target_link_libraries(reka_tests PRIVATE reka::core doctest::doctest)

if(NOT MSVC)
    target_compile_options(reka_tests PRIVATE -Wall -Wextra)
endif()

add_test(NAME reka_tests COMMAND reka_tests)

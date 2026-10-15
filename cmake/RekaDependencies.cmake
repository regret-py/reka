# Third-party dependencies, pinned to exact versions.
#
# Offline / air-gapped builds: point FetchContent at local checkouts, e.g.
#   cmake -B build -DFETCHCONTENT_SOURCE_DIR_JUCE=C:/src/JUCE
#                  -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON=C:/src/json

include(FetchContent)
set(FETCHCONTENT_QUIET ON)

FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE)
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install    OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)

if(REKA_BUILD_TESTS)
    FetchContent_Declare(doctest
        GIT_REPOSITORY https://github.com/doctest/doctest.git
        GIT_TAG        v2.4.11
        GIT_SHALLOW    TRUE)
    set(DOCTEST_NO_INSTALL ON CACHE INTERNAL "")
    FetchContent_MakeAvailable(doctest)
endif()

if(REKA_BUILD_APP)
    # JUCE 8 is dual licensed (AGPLv3 / commercial). See README.md before distributing REKA.exe.
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        8.0.12
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

cmake_minimum_required(VERSION 3.22)

project(TheSolutionSuite VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(SOLUTION_BUILD_TESTS "Build the automated DSP test program (SuiteTests)" OFF)
option(BUNDLE_SKINS "Embed skin faceplates in every plugin (OFF = plugins load them from the shared Skins folder)" OFF)

# Persistent dependency cache outside build/ so cleaning build/ never re-downloads JUCE/CLAP/RTNeural
set(FETCHCONTENT_BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps_cache" CACHE PATH "Persistent dependency cache")

# Link the MSVC runtime statically so plugins load on PCs without the VC++ Redistributable
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)

# 1. Fetch JUCE 8, CLAP Extensions, and RTNeural once for the entire suite
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.7
    GIT_SHALLOW    TRUE
)

FetchContent_Declare(
    clap_juce_extensions
    GIT_REPOSITORY https://github.com/free-audio/clap-juce-extensions.git
    GIT_TAG        main
    GIT_SHALLOW    TRUE
)

FetchContent_Declare(
    RTNeural
    GIT_REPOSITORY https://github.com/jatinchowdhury18/RTNeural.git
    GIT_TAG        main
    GIT_SHALLOW    TRUE
)

set(RTNEURAL_STL ON CACHE BOOL "Use STL backend for RTNeural" FORCE)
FetchContent_MakeAvailable(JUCE clap_juce_extensions RTNeural)

# Ensure Windows WebView2 SDK headers are globally visible to all targets
if(WIN32)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/webview2/build/native/include/WebView2.h")
        message(FATAL_ERROR "WebView2.h not found at ${CMAKE_CURRENT_SOURCE_DIR}/webview2/build/native/include/WebView2.h. Run Install.bat to download it.")
    endif()
    include_directories("${CMAKE_CURRENT_SOURCE_DIR}/webview2/build/native/include")
endif()

# Bundle shared WebView UI + optional 13 hyperrealistic /cars & /places skin faceplates
if(BUNDLE_SKINS)
    set(SOLUTION_BUNDLE_SKINS_VAL 1)
    juce_add_binary_data(SolutionSuiteUI SOURCES
        UI/index.html
        UI/skins/lincoln69.jpg
        UI/skins/camaro71.jpg
        UI/skins/mach1.jpg
        UI/skins/eviltwin.jpg
        UI/skins/eldorado59.jpg
        UI/skins/cuda70.jpg
        UI/skins/gto67.jpg
        UI/skins/rome.jpg
        UI/skins/giza.jpg
        UI/skins/aztec.jpg
        UI/skins/serengeti.jpg
        UI/skins/museum.jpg
        UI/skins/piano.jpg
    )
else()
    set(SOLUTION_BUNDLE_SKINS_VAL 0)
    juce_add_binary_data(SolutionSuiteUI SOURCES
        UI/index.html
    )
endif()

# Auto-detect Avid AAX SDK (drop into aax_sdk/, C:/SDKs/AvidAAXSDK, or set AAX_SDK_PATH)
set(SOLUTION_FORMATS AU VST3 Standalone)
file(GLOB _AAX_NESTED "C:/SDKs/AvidAAXSDK/*/Interfaces/AAX.h")
set(_AAX_NESTED_DIR "")
if(_AAX_NESTED)
    list(GET _AAX_NESTED 0 _FIRST_AAX_H)
    get_filename_component(_AAX_IFACE_DIR "${_FIRST_AAX_H}" DIRECTORY)
    get_filename_component(_AAX_NESTED_DIR "${_AAX_IFACE_DIR}" DIRECTORY)
endif()
set(_AAX_CANDIDATES
    "C:/SDKs/AvidAAXSDK"
    "${_AAX_NESTED_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/aax_sdk"
    "$ENV{AAX_SDK_PATH}"
)
foreach(_AAX_DIR IN LISTS _AAX_CANDIDATES)
    if(_AAX_DIR AND EXISTS "${_AAX_DIR}/Interfaces/AAX.h")
        message(STATUS "[AAX] Found Avid AAX SDK at ${_AAX_DIR} -> AAX targets ENABLED")
        juce_set_aax_sdk_path("${_AAX_DIR}")
        list(APPEND SOLUTION_FORMATS AAX)
        break()
    endif()
endforeach()

# 2. Reusable macro to build any module in The Solution suite (VST3, AU, CLAP, Standalone, + optional AAX)
function(add_solution_module TARGET_NAME PLUGIN_CODE PRODUCT_NAME CLAP_ID)
    juce_add_plugin(${TARGET_NAME}
        COMPANY_NAME                "FastRayMedia"
        IS_SYNTH                    FALSE
        NEEDS_MIDI_INPUT            TRUE
        NEEDS_MIDI_OUTPUT           FALSE
        IS_MIDI_EFFECT              FALSE
        EDITOR_WANTS_KEYBOARD_FOCUS TRUE
        NEEDS_WEB_BROWSER           TRUE
        COPY_PLUGIN_AFTER_BUILD     FALSE
        PLUGIN_MANUFACTURER_CODE    Fstm
        PLUGIN_CODE                 ${PLUGIN_CODE}
        FORMATS                     ${SOLUTION_FORMATS}
        PRODUCT_NAME                "${PRODUCT_NAME}"
        ICON_BIG                    "${CMAKE_CURRENT_SOURCE_DIR}/UI/icon.png"
        ICON_SMALL                  "${CMAKE_CURRENT_SOURCE_DIR}/UI/icon.png"
    )

    clap_juce_extensions_plugin(
        TARGET ${TARGET_NAME}
        CLAP_ID "${CLAP_ID}"
        CLAP_FEATURES "audio-effect" "spatial" "mixing"
    )

    target_sources(${TARGET_NAME}
        PRIVATE
            Source/ModuleEngines.h
            Source/SolutionProcessor.cpp
            Source/SolutionProcessor.h
            Source/SolutionEditor.cpp
            Source/SolutionEditor.h
    )

    if(MSVC)
        target_compile_options(${TARGET_NAME} PRIVATE /MP)
    endif()

    target_compile_definitions(${TARGET_NAME}
        PUBLIC
            JUCE_WEB_BROWSER=1
            JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=1
            JUCE_USE_CURL=0
            JUCE_VST3_CAN_REPLACE_VST2=0
            SOLUTION_CEILING_DBFS=-0.5
            SOLUTION_TOOLTIP_DELAY_MS=600
            SOLUTION_BUNDLE_SKINS=${SOLUTION_BUNDLE_SKINS_VAL}
            SOLUTION_MODULE_NAME="${PRODUCT_NAME}"
            SOLUTION_MODULE_ID="${TARGET_NAME}"
    )

    if(WIN32)
        target_link_libraries(${TARGET_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/webview2/build/native/x64/WebView2LoaderStatic.lib")
    endif()

    target_link_libraries(${TARGET_NAME}
        PRIVATE
            SolutionSuiteUI
            RTNeural
            juce::juce_audio_utils
            juce::juce_dsp
            juce::juce_gui_extra
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags
    )
endfunction()

# 3. Register all 13 consolidated modules (12 core + The Scope master meter)
add_solution_module(TheBunker  Bnk1 "The Bunker"  "com.fastraymedia.thesolution.thebunker")
add_solution_module(TheStage   Stg1 "The Stage"   "com.fastraymedia.thesolution.thestage")
add_solution_module(TheLand    Lnd1 "The Land"    "com.fastraymedia.thesolution.theland")
add_solution_module(TheLoft    Lft1 "The Loft"    "com.fastraymedia.thesolution.theloft")
add_solution_module(TheOrbit   Orb1 "The Orbit"   "com.fastraymedia.thesolution.theorbit")
add_solution_module(TheBridge  Brg1 "The Bridge"  "com.fastraymedia.thesolution.thebridge")
add_solution_module(TheVoid    Vod1 "The Void"    "com.fastraymedia.thesolution.thevoid")
add_solution_module(TheAnchor  Anc1 "The Anchor"  "com.fastraymedia.thesolution.theanchor")
add_solution_module(TheAether  Eth1 "The Aether"  "com.fastraymedia.thesolution.theaether")
add_solution_module(TheMosaic  Mos1 "The Mosaic"  "com.fastraymedia.thesolution.themosaic")
add_solution_module(Innerspace Inn1 "Innerspace"  "com.fastraymedia.thesolution.innerspace")
add_solution_module(TheMatrix  Mtx1 "The Matrix"  "com.fastraymedia.thesolution.thematrix")
add_solution_module(TheScope   Scp1 "The Scope"   "com.fastraymedia.thesolution.thescope")

# 4. Automated tests (cmake -B build -DSOLUTION_BUILD_TESTS=ON && cmake --build build --target SuiteTests)
if(SOLUTION_BUILD_TESTS)
    juce_add_console_app(SuiteTests PRODUCT_NAME "SuiteTests")
    target_sources(SuiteTests PRIVATE tests/SuiteTests.cpp Source/SolutionProcessor.cpp)
    target_compile_definitions(SuiteTests PRIVATE
        SOLUTION_HEADLESS=1
        JucePlugin_Name="SuiteTests"
        SOLUTION_MODULE_NAME="The Stage"
        SOLUTION_MODULE_ID="TheStage"
        SOLUTION_CEILING_DBFS=-0.5
        JUCE_USE_CURL=0
        JUCE_WEB_BROWSER=0)
    target_link_libraries(SuiteTests PRIVATE
        RTNeural
        juce::juce_audio_processors
        juce::juce_dsp
        juce::juce_recommended_config_flags)
endif()

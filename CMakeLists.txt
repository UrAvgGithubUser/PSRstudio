cmake_minimum_required(VERSION 3.16)
project(PSRstudio_cpp CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 COMPONENTS Widgets REQUIRED)

# =========================================================
#  JUCE audio backend
#
#  JUCE is added as a CMake subdirectory. Clone it into lib/JUCE:
#    git clone --depth 1 --branch 7.0.5 https://github.com/juce-framework/JUCE.git lib/JUCE
#
#  Tested with JUCE 7.0.5 (audio modules only). Newer JUCE (7.x/8.x)
#  generally works too but may need small API updates in audio_engine.cpp
#  (e.g. this tree uses closeAudioDevice(), the post-rename API).
#
#  We only enable the audio modules (no GUI) — JUCE's GUI would
#  conflict with Qt's event loop.
# =========================================================
set(JUCE_ENABLE_MODULE_SOURCE_GROUPS OFF CACHE BOOL "" FORCE)
set(JUCE_BUILD_EXAMPLES            OFF CACHE BOOL "" FORCE)
set(JUCE_BUILD_EXTRAS              OFF CACHE BOOL "" FORCE)

# Disable JUCE GUI modules we don't need (saves build time + binary size).
set(JUCE_MODULE_COMPILE_FLAGS_juce_gui_basics       "-DJUCE_MODULE_DISABLED_juce_gui_basics=1")
set(JUCE_MODULE_COMPILE_FLAGS_juce_gui_extra        "-DJUCE_MODULE_DISABLED_juce_gui_extra=1")
set(JUCE_MODULE_COMPILE_FLAGS_juce_opengl           "-DJUCE_MODULE_DISABLED_juce_opengl=1")
set(JUCE_MODULE_COMPILE_FLAGS_juce_graphics         "-DJUCE_MODULE_DISABLED_juce_graphics=1")

# Only add JUCE if the subdirectory exists (so the build doesn't break
# for users who haven't cloned JUCE yet — they just can't use the
# audio_engine module).
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/lib/JUCE/CMakeLists.txt")
    add_subdirectory(lib/JUCE)
    set(HAVE_JUCE TRUE)
    message(STATUS "JUCE found — audio_engine module enabled")
else()
    set(HAVE_JUCE FALSE)
    message(STATUS "JUCE not found at lib/JUCE/ — audio_engine module disabled")
    message(STATUS "  To enable: git clone --depth 1 --branch 7.0.5 https://github.com/juce-framework/JUCE.git lib/JUCE")
endif()

# ---- Core engine sources (pure C++20, no Win32 / Qt dependency) ----
set(ENGINE_SOURCES
    style.cpp
    style_foreign.cpp
    chord_engine.cpp
    transpose.cpp
    remap.cpp
    player.cpp
    playback_controller.cpp
)

# ---- Platform MIDI I/O (WinMM — Windows only) ----
if(WIN32)
    set(MIDI_SOURCES
        midi.cpp
    )
endif()

# ---- FluidSynth (software synth, all platforms) ----
set(FLUID_SOURCES
    fluidsynth_output.cpp
)

# ---- JUCE audio backend (optional, requires JUCE) ----
if(HAVE_JUCE)
    list(APPEND FLUID_SOURCES
        audio_engine.cpp
    )
endif()

# ---- Qt UI ----
set(QT_SOURCES
    main_qt.cpp
    mainwindow.cpp
)

add_executable(PSRstudio_cpp
    ${ENGINE_SOURCES}
    ${MIDI_SOURCES}
    ${FLUID_SOURCES}
    ${QT_SOURCES}
    mainwindow.h
    resources.qrc
)

find_package(FluidSynth CONFIG REQUIRED)
target_link_libraries(PSRstudio_cpp PRIVATE FluidSynth::libfluidsynth)

target_link_libraries(PSRstudio_cpp PRIVATE Qt6::Widgets)

# ---- JUCE audio backend linkage ----
if(HAVE_JUCE)
    target_link_libraries(PSRstudio_cpp PRIVATE
        juce::juce_audio_basics
        juce::juce_audio_devices
        juce::juce_core
    )
    target_compile_definitions(PSRstudio_cpp PRIVATE HAVE_JUCE=1)

    # JUCE needs to know we're not a GUI app on macOS (no AU/VST plugin hosting)
    if(APPLE)
        target_compile_definitions(PSRstudio_cpp PRIVATE
            JUCE_PLUGINHOST_AU=0
            JUCE_PLUGINHOST_VST3=0
        )
    elseif(WIN32)
        target_compile_definitions(PSRstudio_cpp PRIVATE
            JUCE_PLUGINHOST_VST3=0
        )
    endif()
endif()

if(WIN32)
    target_link_libraries(PSRstudio_cpp PRIVATE winmm)
    set_target_properties(PSRstudio_cpp PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

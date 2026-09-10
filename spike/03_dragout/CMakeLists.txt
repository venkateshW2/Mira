# Phase 0 day 4 spike (PRD §9): minimal JUCE 9 app proving system-level drag-out
# (NSDraggingItem via shouldDropFilesWhenDraggedExternally) works on macOS 15.5.
# Founding requirement (PRD §2d) — if this fails, reconsider the GUI toolkit.

cmake_minimum_required(VERSION 3.22)
project(mira_spike_dragout VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(JUCE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/JUCE)
if(NOT EXISTS ${JUCE_ROOT}/CMakeLists.txt)
  message(FATAL_ERROR "JUCE not found at ${JUCE_ROOT} — run scripts/fetch-vendor.sh first")
endif()

add_subdirectory(${JUCE_ROOT} juce_build)

juce_add_gui_app(spike_dragout
  PRODUCT_NAME "mira drag-out spike"
  COMPANY_NAME "mira"
  BUNDLE_ID "app.mira.spike.dragout"
)

target_sources(spike_dragout PRIVATE Source/Main.cpp)

target_compile_definitions(spike_dragout PRIVATE
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0
  JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:spike_dragout,JUCE_PRODUCT_NAME>"
  JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:spike_dragout,JUCE_VERSION>"
  MIRA_FIXTURE_WAV="${CMAKE_CURRENT_SOURCE_DIR}/../../fixtures/flamenco.wav"
)

target_link_libraries(spike_dragout PRIVATE
  juce::juce_gui_extra
  juce::juce_audio_utils
  juce::juce_recommended_config_flags
  juce::juce_recommended_lto_flags
  juce::juce_recommended_warning_flags
)

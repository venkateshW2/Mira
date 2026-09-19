# MIRA-VIDEO.md Phase 0. Three questions whose answers change the plan:
#
#   0.1  Does JUCE's CoreAudioFormat already read the audio track of an .mp4/.mov?
#        If yes, the reference track is just a block pointing at the video file and a
#        whole demux phase disappears.
#   0.2  How far does AVPlayer drift from the audio device over a long clip, and can
#        setPlaySpeed hold it? Every sync threshold in the plan is a guess until this runs.
#   0.3  How long does the first AudioThumbnail of a long 48 kHz file take?
#
# A console app, not a GUI one, for 0.1 and 0.3 -- they need no window. 0.2 needs both a
# video view and an audio device, so it is a GUI app (see main.cpp).

cmake_minimum_required(VERSION 3.22)
project(mira_spike_video_sync VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(JUCE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/JUCE)
if(NOT EXISTS ${JUCE_ROOT}/CMakeLists.txt)
  message(FATAL_ERROR "JUCE not found at ${JUCE_ROOT}")
endif()
# Same guard as spike/03: the top-level configure reaches both this and mira_ui, and CMake
# errors if one source directory is added twice in a single run.
if(NOT TARGET juce::juce_core)
  add_subdirectory(${JUCE_ROOT} juce_build)
endif()

juce_add_gui_app(spike_video_sync
  PRODUCT_NAME "mira video sync spike"
  COMPANY_NAME "mira"
)

target_sources(spike_video_sync PRIVATE Source/main.cpp)

target_compile_definitions(spike_video_sync PRIVATE
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0
  JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:spike_video_sync,JUCE_PRODUCT_NAME>"
  JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:spike_video_sync,JUCE_VERSION>"
)

target_link_libraries(spike_video_sync PRIVATE
  juce::juce_gui_extra
  juce::juce_audio_utils
  juce::juce_audio_formats
  juce::juce_video            # <- the module mira_ui does not yet link
  juce::juce_recommended_config_flags
  juce::juce_recommended_warning_flags
)

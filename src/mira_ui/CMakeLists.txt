# mira_ui — TASKS.md Phase 5, build-order step 1: "promote spike/03_dragout into a real
# target in src/CMakeLists.txt, wired to mira's actual SQLite DB, not the spike's toy
# fixture." Drag-out mechanics and AudioThumbnailCache persistence were already proven in
# spike/03_dragout (Phase 0 day 4/5) — this is that same JUCE foundation, but reading the
# real ~/.mira/library.db (mira_core's Database) instead of one hardcoded fixture file.

set(JUCE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/JUCE)
if(NOT EXISTS ${JUCE_ROOT}/CMakeLists.txt)
  message(FATAL_ERROR "JUCE not found at ${JUCE_ROOT} — run scripts/fetch-vendor.sh first")
endif()

# See spike/03_dragout/CMakeLists.txt's matching guard comment — the same JUCE_ROOT may
# already have been add_subdirectory()'d by that spike earlier in this same configure.
if(NOT TARGET juce::juce_core)
  add_subdirectory(${JUCE_ROOT} juce_build)
endif()

juce_add_gui_app(mira_ui
  VERSION 0.1.0
  PRODUCT_NAME "MIRA" # menu-bar app name (review round 2); also the bundle name, MIRA.app
  COMPANY_NAME "mira"
  BUNDLE_ID "app.mira.ui"
)

# TASKS.md Phase 5 build-order step 2 ("palette/type tokens first"): IBM Plex Sans/Mono
# (SIL OFL 1.1, vendor/fonts/ibm-plex-*/license.txt — permissive, fine under mira's
# AGPL-3.0), embedded via JUCE's BinaryData so the app doesn't depend on the fonts being
# installed system-wide. Only the weights MiraLookAndFeel actually uses (Regular/Medium/
# SemiBold for Sans, Regular/Medium for Mono), not the full family — this app has no use
# for italics/thin/extralight/etc.
set(MIRA_FONTS_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/fonts)
if(NOT EXISTS ${MIRA_FONTS_ROOT}/ibm-plex-sans/IBMPlexSans-Regular.ttf)
  message(FATAL_ERROR "IBM Plex fonts not found at ${MIRA_FONTS_ROOT} — run scripts/fetch-vendor.sh")
endif()
juce_add_binary_data(mira_ui_fonts SOURCES
  ${MIRA_FONTS_ROOT}/ibm-plex-sans/IBMPlexSans-Regular.ttf
  ${MIRA_FONTS_ROOT}/ibm-plex-sans/IBMPlexSans-Medium.ttf
  ${MIRA_FONTS_ROOT}/ibm-plex-sans/IBMPlexSans-SemiBold.ttf
  ${MIRA_FONTS_ROOT}/ibm-plex-mono/IBMPlexMono-Regular.ttf
  ${MIRA_FONTS_ROOT}/ibm-plex-mono/IBMPlexMono-Medium.ttf
)
set_target_properties(mira_ui_fonts PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_sources(mira_ui PRIVATE
  Source/Main.cpp
  Source/MiraLookAndFeel.cpp
  Source/FileTable.cpp
  Source/FolderTreeView.cpp
  Source/WaveformView.cpp
  Source/GroupActivityView.cpp
  Source/ThumbnailStore.cpp
  Source/FileDetailsWindow.cpp
  Source/NativeBlur.mm
  Source/NativeWindowChrome.mm
)

# TASKS.md Phase 5 planning session's visual-direction decision: NSVisualEffectView
# needs real Objective-C++ (NativeBlur.mm), which needs ARC set explicitly rather than
# relying on any project-wide default — JUCE's own recommended config doesn't set this
# for consuming targets, and NSViewComponent::setView's documented retain/release
# contract assumes the caller isn't fighting it with manual memory management.
set_source_files_properties(Source/NativeBlur.mm Source/NativeWindowChrome.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")

target_include_directories(mira_ui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_compile_definitions(mira_ui PRIVATE
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0
  JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:mira_ui,JUCE_PRODUCT_NAME>"
  JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:mira_ui,JUCE_VERSION>"
)

target_link_libraries(mira_ui PRIVATE
  mira_core
  mira_ui_fonts
  juce::juce_gui_extra
  juce::juce_audio_utils
  juce::juce_audio_formats
  juce::juce_recommended_config_flags
  juce::juce_recommended_lto_flags
  juce::juce_recommended_warning_flags
)

set_target_properties(mira_ui PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
)

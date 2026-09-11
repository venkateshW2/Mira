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
  PRODUCT_NAME "mira"
  COMPANY_NAME "mira"
  BUNDLE_ID "app.mira.ui"
)

target_sources(mira_ui PRIVATE Source/Main.cpp)

target_include_directories(mira_ui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_compile_definitions(mira_ui PRIVATE
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0
  JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:mira_ui,JUCE_PRODUCT_NAME>"
  JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:mira_ui,JUCE_VERSION>"
)

target_link_libraries(mira_ui PRIVATE
  mira_core
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

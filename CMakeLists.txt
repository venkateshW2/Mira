# mira — local audio understanding and similarity search (PRD.md).
# C++20 native app; see PRD §7 for the full stack and why each piece was chosen.
#
# Status: Phase 0. Only the day 1-2 spike (spike/01_essentia_link) exists so far —
# see spike/README.md. The real app target (src/) lands in Phase 1.

cmake_minimum_required(VERSION 3.22)
project(mira LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(MIRA_BUILD_SPIKES "Build Phase 0 spike programs (spike/)" ON)

if(MIRA_BUILD_SPIKES AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/spike/01_essentia_link/CMakeLists.txt)
  add_subdirectory(spike/01_essentia_link)
endif()
if(MIRA_BUILD_SPIKES AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/spike/02_onnx_parity/CMakeLists.txt)
  add_subdirectory(spike/02_onnx_parity)
endif()
if(MIRA_BUILD_SPIKES AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/spike/03_dragout/CMakeLists.txt)
  add_subdirectory(spike/03_dragout)
endif()
if(MIRA_BUILD_SPIKES AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/spike/04_sqlite_vec/CMakeLists.txt)
  add_subdirectory(spike/04_sqlite_vec)
endif()

add_subdirectory(src)

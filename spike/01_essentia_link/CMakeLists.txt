# Phase 0 day 1-2 spike (PRD §9): link a hand-built Essentia static lib from
# an external CMake project. Kept separate from the eventual app CMakeLists
# so this spike's pass/fail is unambiguous.

cmake_minimum_required(VERSION 3.22)
project(mira_spike_essentia_link LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(EIGEN3 REQUIRED IMPORTED_TARGET eigen3)
pkg_check_modules(FFTW3F REQUIRED IMPORTED_TARGET fftw3f)
pkg_check_modules(TAGLIB REQUIRED IMPORTED_TARGET taglib)
pkg_check_modules(YAML REQUIRED IMPORTED_TARGET yaml-0.1)
pkg_check_modules(CHROMAPRINT REQUIRED IMPORTED_TARGET libchromaprint)
pkg_check_modules(SAMPLERATE REQUIRED IMPORTED_TARGET samplerate)
pkg_check_modules(AVCODEC REQUIRED IMPORTED_TARGET libavcodec)
pkg_check_modules(AVFORMAT REQUIRED IMPORTED_TARGET libavformat)
pkg_check_modules(AVUTIL REQUIRED IMPORTED_TARGET libavutil)
pkg_check_modules(SWRESAMPLE REQUIRED IMPORTED_TARGET libswresample)

set(ESSENTIA_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/essentia)
set(ESSENTIA_LIB ${ESSENTIA_ROOT}/build/src/libessentia.a)

if(NOT EXISTS ${ESSENTIA_LIB})
  message(FATAL_ERROR "libessentia.a not found at ${ESSENTIA_LIB} — "
                       "run `python3 waf configure --build-static --std=c++17 && python3 waf build` "
                       "in ${ESSENTIA_ROOT} first")
endif()

add_executable(spike_essentia_link main.cpp)

target_include_directories(spike_essentia_link PRIVATE
  ${ESSENTIA_ROOT}/src
)

target_link_libraries(spike_essentia_link PRIVATE
  ${ESSENTIA_LIB}
  PkgConfig::AVFORMAT
  PkgConfig::AVCODEC
  PkgConfig::SWRESAMPLE
  PkgConfig::AVUTIL
  PkgConfig::SAMPLERATE
  PkgConfig::TAGLIB
  PkgConfig::YAML
  PkgConfig::FFTW3F
  PkgConfig::CHROMAPRINT
  PkgConfig::EIGEN3
)

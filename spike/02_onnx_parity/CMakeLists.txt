# Phase 0 day 3 spike (PRD §9): Essentia mel frontend + ONNX Runtime C++ inference.

cmake_minimum_required(VERSION 3.22)
project(mira_spike_onnx_parity LANGUAGES CXX)

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
set(ORT_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/onnxruntime)

if(NOT EXISTS ${ESSENTIA_LIB})
  message(FATAL_ERROR "libessentia.a not found at ${ESSENTIA_LIB} — build spike 01 first")
endif()
if(NOT EXISTS ${ORT_ROOT}/lib/libonnxruntime.dylib)
  message(FATAL_ERROR "ONNX Runtime not found at ${ORT_ROOT} — "
                       "download onnxruntime-osx-arm64-1.29.0.tgz into vendor/onnxruntime")
endif()

add_executable(spike_onnx_parity main.cpp)

target_include_directories(spike_onnx_parity PRIVATE
  ${ESSENTIA_ROOT}/src
  ${ORT_ROOT}/include
)

target_link_directories(spike_onnx_parity PRIVATE ${ORT_ROOT}/lib)

target_link_libraries(spike_onnx_parity PRIVATE
  ${ESSENTIA_LIB}
  onnxruntime
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

set_target_properties(spike_onnx_parity PROPERTIES
  BUILD_RPATH ${ORT_ROOT}/lib
)

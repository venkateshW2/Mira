# MIRA-BLOCKS.md step 3.1 -- does signalsmith-stretch do what step 3 needs, and what does
# it cost in latency? Standalone, the way every spike in this folder is: it proves one
# risky assumption before any of mira depends on it.
#
#   cmake -B build && cmake --build build && ./build/stretch_latency
cmake_minimum_required(VERSION 3.22)
project(stretch_latency CXX)
set(CMAKE_CXX_STANDARD 20)
set(VENDOR ${CMAKE_CURRENT_SOURCE_DIR}/../../vendor)
add_executable(stretch_latency Source/main.cpp)
target_include_directories(stretch_latency PRIVATE
    ${VENDOR}/signalsmith-stretch
    ${VENDOR}/signalsmith-stretch/include
    ${VENDOR}/signalsmith-linear
    ${VENDOR}/signalsmith-linear/include)
# Optional per upstream, and measured to change nothing about the OUTPUT (identical to the
# sample) -- it is a speed flag, and mira is Apple-only, so it is on.
target_compile_definitions(stretch_latency PRIVATE SIGNALSMITH_USE_ACCELERATE)
target_link_libraries(stretch_latency PRIVATE "-framework Accelerate")

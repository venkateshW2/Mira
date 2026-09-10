# Phase 0 day 5 spike (PRD §9): SQLite amalgamation + sqlite-vec, statically linked.
# PRD §7: "do not link macOS's libsqlite3 — Apple builds it with SQLITE_OMIT_LOAD_EXTENSION".
# We compile the vendored amalgamation directly instead.

cmake_minimum_required(VERSION 3.22)
project(mira_spike_sqlite_vec LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SQLITE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/sqlite-amalgamation)
set(SQLITE_VEC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/sqlite-vec)

if(NOT EXISTS ${SQLITE_ROOT}/sqlite3.c)
  message(FATAL_ERROR "SQLite amalgamation not found at ${SQLITE_ROOT}")
endif()
if(NOT EXISTS ${SQLITE_VEC_ROOT}/sqlite-vec.h)
  message(FATAL_ERROR "sqlite-vec.h not generated — run `make sqlite-vec.h` in ${SQLITE_VEC_ROOT}")
endif()

add_library(sqlite3_amalgamation STATIC ${SQLITE_ROOT}/sqlite3.c)
target_include_directories(sqlite3_amalgamation PUBLIC ${SQLITE_ROOT})
target_compile_options(sqlite3_amalgamation PRIVATE
  -DSQLITE_ENABLE_LOAD_EXTENSION=1
  -DSQLITE_THREADSAFE=1
)

# vendor/sqlite-vec/ has a bare file named "version" at its root, which collides with
# libc++'s <version> standard header if that whole directory is on the include path
# for a C++ translation unit. Copy just the one header we need into an isolated dir.
file(COPY ${SQLITE_VEC_ROOT}/sqlite-vec.h DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/include)

add_library(sqlite_vec STATIC ${SQLITE_VEC_ROOT}/sqlite-vec.c)
target_include_directories(sqlite_vec PRIVATE ${SQLITE_VEC_ROOT})
target_link_libraries(sqlite_vec PUBLIC sqlite3_amalgamation)
target_compile_definitions(sqlite_vec PRIVATE SQLITE_CORE SQLITE_VEC_STATIC)

add_executable(spike_sqlite_vec main.cpp)
target_include_directories(spike_sqlite_vec PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/include)
target_compile_definitions(spike_sqlite_vec PRIVATE SQLITE_CORE SQLITE_VEC_STATIC)
target_link_libraries(spike_sqlite_vec PRIVATE sqlite_vec sqlite3_amalgamation)

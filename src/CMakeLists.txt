# The real mira app (Phase 1 onward — PRD §7). Built incrementally: today this is the
# scanner + SQLite storage; MIR, neural, and the JUCE UI attach to this same target in
# later phases.

set(SQLITE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../vendor/sqlite-amalgamation)
set(SQLITECPP_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../vendor/SQLiteCpp)

if(NOT EXISTS ${SQLITE_ROOT}/sqlite3.c)
  message(FATAL_ERROR "SQLite amalgamation not found at ${SQLITE_ROOT} — run scripts/fetch-vendor.sh")
endif()
if(NOT EXISTS ${SQLITECPP_ROOT}/CMakeLists.txt)
  message(FATAL_ERROR "SQLiteCpp not found at ${SQLITECPP_ROOT} — run scripts/fetch-vendor.sh")
endif()

# Our own vendored SQLite (3.53.4, PRD §7) as the SQLite::SQLite3 target SQLiteCpp
# expects — never Apple's libsqlite3 (built SQLITE_OMIT_LOAD_EXTENSION) and not
# SQLiteCpp's own bundled copy either, so there's exactly one SQLite version in the app.
add_library(mira_sqlite3 STATIC ${SQLITE_ROOT}/sqlite3.c)
target_include_directories(mira_sqlite3 PUBLIC ${SQLITE_ROOT})
target_compile_options(mira_sqlite3 PRIVATE
  -DSQLITE_ENABLE_LOAD_EXTENSION=1
  -DSQLITE_THREADSAFE=1
  -DSQLITE_ENABLE_COLUMN_METADATA=1
)
add_library(SQLite::SQLite3 ALIAS mira_sqlite3)

set(SQLITECPP_INTERNAL_SQLITE OFF CACHE BOOL "" FORCE)
set(SQLITECPP_FIND_SQLITE OFF CACHE BOOL "" FORCE)
set(SQLITECPP_RUN_CPPLINT OFF CACHE BOOL "" FORCE)
set(SQLITECPP_RUN_CPPCHECK OFF CACHE BOOL "" FORCE)
set(SQLITECPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SQLITECPP_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SQLITECPP_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(${SQLITECPP_ROOT} sqlitecpp_build)

add_executable(mira
  mira/main.cpp
  mira/db/Database.cpp
  mira/scan/Scanner.cpp
)

target_link_libraries(mira PRIVATE SQLiteCpp mira_sqlite3 "-framework CoreFoundation" "-framework Security")

set_target_properties(mira PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)

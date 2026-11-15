cmake_minimum_required(VERSION 3.5)

set(TARGET_NAME notebook)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD
    "17"
    CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(src/include)

# core/      plain C++ with no DuckDB dependency: format, lexer, script generation.
#            Unit-tested on its own with `make core_test`.
# functions/ the SQL surface, one file per function, written against DuckDB's C++ API.
set(EXTENSION_SOURCES
    src/core/sql_lexer.cpp
    src/core/format.cpp
    src/core/script.cpp
    src/functions/common.cpp
    src/functions/notebook_cells.cpp
    src/functions/notebook_sql.cpp
    src/functions/notebook_run.cpp
    src/notebook_extension.cpp)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")

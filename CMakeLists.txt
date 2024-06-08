cmake_minimum_required(VERSION 3.5)

# Set extension name here
set(TARGET_NAME sheetreader)

# DuckDB's extension distribution supports vcpkg. As such, dependencies can be added in ./vcpkg.json and then
# used in cmake with find_package. Feel free to remove or replace with other dependencies.
# Note that it should also be removed from vcpkg.json to prevent needlessly installing it..
# find_package(Example-Package REQUIRED)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})
include_directories(src/include)
include_directories(src/include/sheetreader/src/)
include_directories(src/include/sheetreader/src/fast_double_parser)
include_directories(src/include/sheetreader/src/miniz)

set(EXTENSION_SOURCES src/sheetreader_extension.cpp src/include/sheetreader/src/XlsxFile.cpp src/include/sheetreader/src/XlsxSheet.cpp src/include/sheetreader/src/miniz/miniz.cpp)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
# TODO: We might need this at some point -- this is probably faster to build
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")

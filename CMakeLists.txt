cmake_minimum_required(VERSION 3.16)
project(pando VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Optional: HTTP server (cpp-httplib, header-only)
include(FetchContent)
FetchContent_Declare(httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG v0.14.3
)
FetchContent_MakeAvailable(httplib)

add_library(pando_core STATIC
    src/core/mmap_file.cpp
    src/index/lexicon.cpp
    src/index/positional_attr.cpp
    src/index/structural_attr.cpp
    src/index/dependency_index.cpp
    src/corpus/corpus.cpp
    src/corpus/builder.cpp
    src/corpus/streaming_builder.cpp
    src/query/lexer.cpp
    src/query/parser.cpp
    src/query/executor.cpp
    src/api/index_api.cpp
)
target_include_directories(pando_core PUBLIC src)

add_executable(pando-index src/cli/index_main.cpp)
target_link_libraries(pando-index PRIVATE pando_core)

add_executable(pando src/cli/query_main.cpp)
target_link_libraries(pando PRIVATE pando_core)

add_executable(pando-check src/cli/check_main.cpp)
target_link_libraries(pando-check PRIVATE pando_core)

add_executable(cwb-import src/cli/cwb_import_main.cpp)
target_link_libraries(cwb-import PRIVATE pando_core)

# API layer: query execution + JSON (used by CLI and server)
add_library(pando_api STATIC src/api/query_json.cpp)
target_include_directories(pando_api PUBLIC src)
target_link_libraries(pando_api PUBLIC pando_core)

# HTTP API server (optional: requires FetchContent to get cpp-httplib)
add_executable(pando-server src/cli/server_main.cpp)
target_include_directories(pando-server PRIVATE ${httplib_SOURCE_DIR})
target_link_libraries(pando-server PRIVATE pando_api)

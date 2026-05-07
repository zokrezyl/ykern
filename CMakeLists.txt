#=============================================================================
# ykern — Linux Kernel Exploration & Manipulation Library
#=============================================================================
cmake_minimum_required(VERSION 3.20)
project(ykern VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libraries" FORCE)

option(YKERN_BUILD_TESTS "Build smoke tests" ON)
option(YKERN_BUILD_TOOLS "Build the ykern CLI tool" ON)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "ykern is Linux-only at this stage (got ${CMAKE_SYSTEM_NAME}).")
endif()

add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter
    -fno-strict-aliasing
    -D_GNU_SOURCE
)

# Modules — order matches dependency DAG
add_subdirectory(src/ykern/ycore)
add_subdirectory(src/ykern/ynetlink)
add_subdirectory(src/ykern/ysys)
add_subdirectory(src/ykern/yproc)

# Callers consume ykern by linking the module libraries they need:
#   target_link_libraries(<your_target> PRIVATE ykern_ycore ykern_ynetlink)
# We deliberately do not ship an aggregate INTERFACE library — the name
# `ykern` is reserved for the CLI binary in tools/.

if(YKERN_BUILD_TESTS)
    add_subdirectory(tests)
endif()

if(YKERN_BUILD_TOOLS)
    add_subdirectory(tools/ykern)
endif()

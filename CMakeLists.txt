cmake_minimum_required(VERSION 3.21)
project(ABTWREN VERSION 0.1.0 LANGUAGES CXX)

# ── Fetch dependencies from CERN GitLab ──────────────────────────
include(FetchContent)

FetchContent_Declare(
    ABTEdge
    GIT_REPOSITORY ssh://git@gitlab.cern.ch:7999/abt-projects/software/kitr/drivers/abtedge.git
    GIT_TAG        master  # Pin to a specific commit hash for reproducible builds
)

FetchContent_Declare(
    ABTRDA3
    GIT_REPOSITORY ssh://git@gitlab.cern.ch:7999/abt-projects/software/kitr/drivers/abtrda3.git
    GIT_TAG        master
)

FetchContent_MakeAvailable(ABTEdge ABTRDA3)

# ── Compiler flags ────────────────────────────────────────────────
add_library(abtwren_flags INTERFACE)
target_compile_features(abtwren_flags INTERFACE cxx_std_20)
target_compile_options(abtwren_flags INTERFACE
    -Wall -Wextra -Wpedantic -Werror=return-type -Wconversion -Wshadow
)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64")
    target_compile_options(abtwren_flags INTERFACE -march=native)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")
    target_compile_options(abtwren_flags INTERFACE -march=armv8-a)
endif()

target_compile_options(abtwren_flags INTERFACE
    $<$<CONFIG:Release>:-O3 -flto -fno-exceptions -DNDEBUG>
    $<$<CONFIG:Debug>:-O0 -g3 -fno-omit-frame-pointer>
)
target_link_options(abtwren_flags INTERFACE
    $<$<CONFIG:Release>:-flto>
)

# ── WREN Transmitter library ─────────────────────────────────────
add_library(wren_transmitter STATIC
    src/WRENTransmitter.cpp
)

target_include_directories(wren_transmitter PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
)

target_link_libraries(wren_transmitter
    PUBLIC  fpga_backend abtrda3
    PRIVATE abtwren_flags
)

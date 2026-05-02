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

FetchContent_Declare(
    SPSCQueue
    GIT_REPOSITORY https://github.com/rigtorp/SPSCQueue.git
    GIT_TAG        master
)

FetchContent_MakeAvailable(ABTEdge ABTRDA3 SPSCQueue)

# ── Compiler flags ────────────────────────────────────────────────
add_library(abtwren_flags INTERFACE)
target_compile_features(abtwren_flags INTERFACE cxx_std_20)
target_compile_options(abtwren_flags INTERFACE
    -Wall -Wextra -Wpedantic -Werror=return-type -Wconversion -Wshadow
    -Wno-interference-size   # suppress GCC warning from rigtorp/SPSCQueue.h
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

# ── Include paths for subdirectories ──────────────────────────────
set(ABTWREN_INCLUDE_DIRS
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/common>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/WREN>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/HAL>
)

# ── WREN Transmitter library ─────────────────────────────────────
add_library(wren_transmitter STATIC
    src/WREN/WRENTransmitter.cpp
    src/WREN/WRENCTIMConfigurator.cpp
)

target_include_directories(wren_transmitter PUBLIC ${ABTWREN_INCLUDE_DIRS})

target_link_libraries(wren_transmitter
    PUBLIC  abtedge_common abtrda3
    PRIVATE abtwren_flags fmt::fmt
)

# ── Compile-time sink selection ─────────────────────────────────
option(ABTWREN_USE_QUEUE "Use SPSC queue + EventProcessor thread (OFF = direct shm)" OFF)

# ── WREN Receiver library ───────────────────────────────────────
add_library(wren_receiver STATIC
    src/common/ShmSink.cpp
    src/common/QueueSink.cpp
    src/common/EventProcessor.cpp
)

target_include_directories(wren_receiver PUBLIC ${ABTWREN_INCLUDE_DIRS})

target_link_libraries(wren_receiver
    PUBLIC  abtrda3 SPSCQueue abtedge_common
    PRIVATE abtwren_flags fmt::fmt
)

# ── Main executable ─────────────────────────────────────────────
add_executable(abtwren src/main.cpp)

target_include_directories(abtwren PRIVATE ${ABTWREN_INCLUDE_DIRS})

target_link_libraries(abtwren
    PRIVATE wren_transmitter wren_receiver abtwren_flags fmt::fmt
)

if(ABTWREN_USE_QUEUE)
    target_compile_definitions(abtwren PRIVATE ABTWREN_USE_QUEUE)
endif()

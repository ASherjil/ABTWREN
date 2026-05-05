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

FetchContent_Declare(
    tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG        v3.4.0
)

FetchContent_MakeAvailable(ABTEdge ABTRDA3 SPSCQueue tomlplusplus)

# ABTRDA3 / ABTEdge inject -fsanitize=address,undefined in Debug builds.
# Target hardware (mkdev16, mkdev30, mkdev50) has no libasan installed.
# Strip the sanitizer flags so all code compiles without instrumentation.
foreach(_flag_target abtrda3_flags abtedge_flags)
    if(TARGET ${_flag_target})
        foreach(_prop IN ITEMS INTERFACE_COMPILE_OPTIONS INTERFACE_LINK_OPTIONS)
            get_target_property(_flags ${_flag_target} ${_prop})
            if(_flags AND NOT _flags STREQUAL "_flags-NOTFOUND")
                string(REPLACE "-fsanitize=address,undefined" "" _flags "${_flags}")
                string(REPLACE "-fsanitize=address" "" _flags "${_flags}")
                string(REPLACE "-fsanitize=undefined" "" _flags "${_flags}")
                string(STRIP "${_flags}" _flags)
                set_target_properties(${_flag_target} PROPERTIES ${_prop} "${_flags}")
            endif()
        endforeach()
    endif()
endforeach()

# ── Compiler flags ────────────────────────────────────────────────
add_library(abtwren_flags INTERFACE)
target_compile_features(abtwren_flags INTERFACE cxx_std_20)
target_compile_options(abtwren_flags INTERFACE
    -Wall -Wextra -Wpedantic -Werror=return-type -Wconversion -Wshadow
    -Wno-interference-size   # suppress GCC warning from rigtorp/SPSCQueue.h
)

# ── Architecture-specific tuning ───────────────────────────────────

if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64")
    target_compile_options(abtwren_flags INTERFACE -march=native -mtune=native)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")
    # Zynq UltraScale+ Cortex-A53 — ARMv8.0-A with crypto
    target_compile_options(abtwren_flags INTERFACE -mcpu=cortex-a53 -mtune=cortex-a53)
endif()

# ── Release: maximal optimisations for lowest latency ───────────────

target_compile_options(abtwren_flags INTERFACE
    $<$<CONFIG:Release>:-O3 -flto -fno-exceptions -DNDEBUG>
    $<$<CONFIG:Release>:-fno-plt -fno-semantic-interposition>
    $<$<CONFIG:Release>:-falign-functions=64 -falign-loops=64>
    $<$<CONFIG:Release>:-fno-math-errno -fno-trapping-math>
    $<$<CONFIG:Release>:-ffunction-sections -fdata-sections>
)
target_link_options(abtwren_flags INTERFACE
    $<$<CONFIG:Release>:-flto>
    $<$<CONFIG:Release>:-Wl,-z,now -Wl,--gc-sections>
    $<$<CONFIG:Release>:-Wl,-O3 -Wl,--hash-style=gnu>
)

# ── Debug: no optimisation, frame pointers for profiling ────────────

target_compile_options(abtwren_flags INTERFACE
    $<$<CONFIG:Debug>:-O0 -g3 -fno-omit-frame-pointer>
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
    PRIVATE wren_transmitter wren_receiver abtwren_flags fmt::fmt tomlplusplus::tomlplusplus
)

if(ABTWREN_USE_QUEUE)
    target_compile_definitions(abtwren PRIVATE ABTWREN_USE_QUEUE)
endif()

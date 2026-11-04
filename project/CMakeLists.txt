cmake_minimum_required(VERSION 3.25)
project(daytona LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(REXGLUE_USE_VULKAN "Build the Vulkan backend used by Daytona" ON)

# The submodule pins unmodified upstream ReXGlue 0.10.0.
set(REXGLUE_SDK_SOURCE_DIR
    "${CMAKE_SOURCE_DIR}/../thirdparty/rexglue-sdk"
    CACHE PATH "Path to the ReXGlue 0.10.0 SDK source tree")

if(EXISTS "${REXGLUE_SDK_SOURCE_DIR}/CMakeLists.txt")
    add_subdirectory("${REXGLUE_SDK_SOURCE_DIR}" rexglue-sdk)
else()
    message(FATAL_ERROR
        "Required ReXGlue SDK submodule not found. Run:\n"
        "  git submodule update --init --recursive\n"
        "Expected path: ${REXGLUE_SDK_SOURCE_DIR}")
endif()

# The SDK plugin exports only its C entry points. Compile its Vulkan classes
# into Daytona so the application can select its graphics system directly.
get_target_property(_GUEST_GPU_SOURCES rexgpu-xenos SOURCES)
get_target_property(_GUEST_GPU_SOURCE_DIR rexgpu-xenos SOURCE_DIR)
list(REMOVE_ITEM _GUEST_GPU_SOURCES plugin_main.cpp)
foreach(_SOURCE IN LISTS _GUEST_GPU_SOURCES)
    cmake_path(ABSOLUTE_PATH _SOURCE BASE_DIRECTORY "${_GUEST_GPU_SOURCE_DIR}"
        OUTPUT_VARIABLE _ABSOLUTE_SOURCE)
    list(APPEND _DAYTONA_GPU_SOURCES "${_ABSOLUTE_SOURCE}")
endforeach()
add_library(daytona_guest_gpu STATIC ${_DAYTONA_GPU_SOURCES})
target_include_directories(daytona_guest_gpu PRIVATE
    $<TARGET_PROPERTY:rexgpu-xenos,INCLUDE_DIRECTORIES>)
target_link_libraries(daytona_guest_gpu PUBLIC rex::runtime Vulkan::Headers
    PRIVATE $<TARGET_PROPERTY:rexgpu-xenos,LINK_LIBRARIES>)

# Include generated source list from rexglue codegen.
# On a fresh clone this file doesn't exist yet; create an empty stub so CMake
# configure succeeds before the first codegen run.
set(_SOURCES_CMAKE "${CMAKE_SOURCE_DIR}/../generated/sources.cmake")
if(NOT EXISTS "${_SOURCES_CMAKE}")
    message(STATUS "generated/sources.cmake not found — run codegen before building (see README)")
    file(MAKE_DIRECTORY "${CMAKE_SOURCE_DIR}/../generated")
    file(WRITE "${_SOURCES_CMAKE}" "# Auto-created stub; run codegen to populate\nset(GENERATED_SOURCES)\n")
endif()
include("${_SOURCES_CMAKE}")

add_executable(daytona
    src/main.cpp
    src/stubs.cpp
    src/keyboard_driver.cpp
    src/daytona_debug.cpp
    src/render_probe.cpp
    ${GENERATED_SOURCES}
)

# Fail the build with a clear message if codegen has not been run.
# Configure must succeed so the daytona_codegen target is available; the check
# runs as a PRE_BUILD step so it fires before any .cpp is compiled.
add_custom_command(TARGET daytona PRE_BUILD
    COMMAND ${CMAKE_COMMAND}
        "-DCODEGEN_INIT_H=${CMAKE_SOURCE_DIR}/../generated/daytona_init.h"
        -P "${CMAKE_CURRENT_LIST_DIR}/cmake/require_codegen.cmake"
    VERBATIM
)

# Let rexglue_configure_target add: entry point, rex_app.cpp, mcmodel=large,
# SDL3, RPATH, and other platform flags.
set(REXGLUE_ROOT "${REXGLUE_SDK_SOURCE_DIR}")
rexglue_configure_target(daytona)
if(UNIX AND NOT APPLE)
    add_custom_command(TARGET daytona POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:rex::runtime> $<TARGET_FILE_DIR:daytona>
        VERBATIM)
endif()

target_include_directories(daytona PRIVATE
    ${CMAKE_SOURCE_DIR}/../generated
    ${CMAKE_SOURCE_DIR}/../ppc
    ${REXGLUE_SDK_SOURCE_DIR}/thirdparty/simde
    ${REXGLUE_SDK_SOURCE_DIR}/include/simde
    ${REXGLUE_SDK_SOURCE_DIR}/thirdparty/imgui
    ${REXGLUE_SDK_SOURCE_DIR}/thirdparty/glslang
    ${REXGLUE_SDK_SOURCE_DIR}/thirdparty/renderdoc
    ${CMAKE_BINARY_DIR}/rexglue-sdk/include
)

target_link_libraries(daytona PRIVATE
    rex::runtime
    daytona_guest_gpu
    xxHash::xxhash
)

target_compile_definitions(daytona PRIVATE PPC_INCLUDE_DETAIL)

target_compile_options(daytona PRIVATE
    -fno-strict-aliasing
    -ffp-model=strict
    -fno-char8_t
    $<$<CONFIG:Debug>:-g -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>
    $<$<CONFIG:RelWithDebInfo>:-O2 -g -DNDEBUG>
)

add_custom_target(daytona_codegen
    COMMAND $<TARGET_FILE:rex::rexglue> --force codegen ${CMAKE_SOURCE_DIR}/../config/daytona_manifest.toml
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/../config
    COMMENT "Regenerating Daytona codegen with vendored ReXGlue SDK"
    VERBATIM
)

cmake_minimum_required(VERSION 3.25)
project(DaytonaShaderTranslate LANGUAGES CXX)

set(XENOS_RECOMP_SOURCE_DIR "" CACHE PATH "Pinned XenosRecomp source checkout")
if(NOT EXISTS "${XENOS_RECOMP_SOURCE_DIR}/XenosRecomp/shader_recompiler.cpp")
    message(FATAL_ERROR "Set XENOS_RECOMP_SOURCE_DIR to the pinned XenosRecomp checkout")
endif()

add_subdirectory("${XENOS_RECOMP_SOURCE_DIR}/thirdparty/fmt" fmt EXCLUDE_FROM_ALL)
# Translating one shader to HLSL needs no DXC library or shader-cache support.
add_executable(daytona_shader_translate main.cpp
    "${XENOS_RECOMP_SOURCE_DIR}/XenosRecomp/shader_recompiler.cpp")
target_compile_features(daytona_shader_translate PRIVATE cxx_std_17)
target_precompile_headers(daytona_shader_translate PRIVATE
    "${XENOS_RECOMP_SOURCE_DIR}/XenosRecomp/pch.h")
target_include_directories(daytona_shader_translate PRIVATE
    "${XENOS_RECOMP_SOURCE_DIR}/XenosRecomp"
    "${XENOS_RECOMP_SOURCE_DIR}/thirdparty/dxc-bin/inc"
    "${XENOS_RECOMP_SOURCE_DIR}/thirdparty/smol-v/source"
    "${XENOS_RECOMP_SOURCE_DIR}/thirdparty/xxHash"
    "${XENOS_RECOMP_SOURCE_DIR}/thirdparty/zstd/lib")
target_link_libraries(daytona_shader_translate PRIVATE fmt::fmt-header-only)
target_compile_options(daytona_shader_translate PRIVATE -fms-extensions)
set_target_properties(daytona_shader_translate PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

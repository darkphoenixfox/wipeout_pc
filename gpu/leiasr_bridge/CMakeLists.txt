# SPDX-License-Identifier: MIT
#
# gow3_leiasr.dll links the LeiaSR SDK (MSVC only) and is built separately from the port:
#   cmake -S gpu/leiasr_bridge -B out/leiasr-bridge -G "Visual Studio 17 2022" -A x64 \
#         -DLEIASR_SDK_ROOT=<LeiaSR SDK folder, with include/ and lib/>
#   cmake --build out/leiasr-bridge --config Release
# The SDK is not in the repository. At run time the SR Platform (installed with the display's
# software) provides the DLLs the bridge loads.

cmake_minimum_required(VERSION 3.20)
project(gow3_leiasr LANGUAGES CXX)

set(LEIASR_SDK_ROOT "$ENV{LEIASR_SDKROOT}" CACHE PATH "LeiaSR SDK root (contains include/ and lib/)")

if (NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "The LeiaSR bridge is Windows x64 only")
endif()
if (NOT EXISTS "${LEIASR_SDK_ROOT}/include/sr/weaver/dx11weaver.h")
    message(FATAL_ERROR "Set LEIASR_SDK_ROOT to the LeiaSR SDK (include/sr/weaver/dx11weaver.h)")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# The SR Platform's DLLs use the DLL runtime (/MD), as the SDK's examples.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

add_library(gow3_leiasr SHARED gow3_leiasr_bridge.cpp gow3_leiasr_bridge.h)
# The SDK's headers include its bundled OpenCV and GLog.
target_include_directories(gow3_leiasr PRIVATE "${LEIASR_SDK_ROOT}/include"
    "${LEIASR_SDK_ROOT}/third_party/OpenCV/include" "${LEIASR_SDK_ROOT}/third_party/GLog/include")
target_link_libraries(gow3_leiasr PRIVATE
    "${LEIASR_SDK_ROOT}/lib/SimulatedRealityCore.lib"
    "${LEIASR_SDK_ROOT}/lib/SimulatedRealityDirectX.lib"
    "${LEIASR_SDK_ROOT}/lib/SimulatedRealityDisplays.lib"
    "${LEIASR_SDK_ROOT}/lib/simulatedreality.lib"
    d3d11 dxgi)
# The SDK's headers (srcontext.h) inline a little OpenCV, which needs cv::String::deallocate. The
# SR Platform ships OpenCV as opencv_world343.dll (SimulatedRealityCore.dll links it), and the
# SDK has no import library for it: one is made for that symbol.
set(OPENCV_DEF "${CMAKE_CURRENT_BINARY_DIR}/opencv_world343.def")
set(OPENCV_LIB "${CMAKE_CURRENT_BINARY_DIR}/opencv_world343.lib")
file(WRITE "${OPENCV_DEF}" "LIBRARY opencv_world343.dll
EXPORTS
    ?deallocate@String@cv@@AEAAXXZ
")
add_custom_command(OUTPUT "${OPENCV_LIB}"
    COMMAND "${CMAKE_AR}" /nologo /machine:x64 "/def:${OPENCV_DEF}" "/out:${OPENCV_LIB}"
    DEPENDS "${OPENCV_DEF}" VERBATIM)
add_custom_target(opencv_world_import DEPENDS "${OPENCV_LIB}")
add_dependencies(gow3_leiasr opencv_world_import)
target_link_libraries(gow3_leiasr PRIVATE "${OPENCV_LIB}")

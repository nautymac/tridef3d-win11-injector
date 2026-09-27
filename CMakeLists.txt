cmake_minimum_required(VERSION 3.20)
# SRWeave - in-process SR weaving for DirectX 9 / 11 games.
#
# Builds two DLLs for the configured architecture (pass -A x64 or -A Win32):
#   SRWeaveDX9.dll   hooks IDirect3DDevice9::Present and weaves the SBS back buffer
#   SRWeaveDX11.dll  hooks IDXGISwapChain::Present and does the same for DX11
# Each DLL works either injected under its own name, or copied into the game
# folder as d3d9.dll / dxgi.dll / d3d11.dll (it exports the proxy entry points).
project(SRWeave LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: the DLL lands inside arbitrary game processes that may not have the matching
# VC++ redistributable (32-bit ones in particular).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(ROOT "${CMAKE_SOURCE_DIR}")
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(ARCH x64)
    set(SR_SDK "${ROOT}/third_party/sr-sdk")
    set(SR_SUFFIX "")
    set(SR_OPENCV_LIB "${SR_SDK}/third_party/OpenCV/lib/x64/opencv_world343.lib")
else()
    set(ARCH x86)
    set(SR_SDK "${ROOT}/third_party/sr-sdk32")
    set(SR_SUFFIX "32")
    set(SR_OPENCV_LIB "${SR_SDK}/third_party/OpenCV/lib/x86/opencv_world343.lib")
endif()
if(NOT EXISTS "${SR_SDK}/include/sr/weaver/dx9weaver.h")
    message(FATAL_ERROR "SR SDK for ${ARCH} not found at ${SR_SDK} - see README (setup-deps.ps1 -SrSdkInstaller / -SrSdk32Installer)")
endif()
if(NOT EXISTS "${SR_OPENCV_LIB}")
    message(FATAL_ERROR "SDK OpenCV import lib missing: ${SR_OPENCV_LIB}")
endif()

set(SRWEAVE_OUT_DIR "${ROOT}/bin/${ARCH}" CACHE PATH "output folder for the DLLs")

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(MINHOOK_HDE third_party/minhook/src/hde/hde64.c)
else()
    set(MINHOOK_HDE third_party/minhook/src/hde/hde32.c)
endif()
add_library(minhook STATIC
    third_party/minhook/src/buffer.c
    third_party/minhook/src/hook.c
    third_party/minhook/src/trampoline.c
    ${MINHOOK_HDE})
target_include_directories(minhook PUBLIC third_party/minhook/include)

function(srweave_dll name src def)
    add_library(${name} SHARED src/${src} src/common.cpp src/${def})
    target_include_directories(${name} PRIVATE
        "${SR_SDK}/include"
        "${SR_SDK}/third_party/OpenCV/include"
        "${SR_SDK}/third_party/GLog/include")
    # /wd4005: the SDK's IWeaverBase.h redefines WIN32_LEAN_AND_MEAN
    target_compile_options(${name} PRIVATE /EHsc /permissive- /utf-8 /W3 /wd4005)
    target_compile_definitions(${name} PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS SRW_SR_SUFFIX="${SR_SUFFIX}" SRW_ARCH="${ARCH}")
    # No d3d9/d3d11/dxgi import libs on purpose: when this DLL is renamed to
    # d3d9.dll (proxy mode) a static import of d3d9.dll would resolve to itself.
    # Everything Direct3D is resolved at runtime from the System32 copy.
    target_link_libraries(${name} PRIVATE minhook
        "${SR_SDK}/lib/SimulatedRealityCore${SR_SUFFIX}.lib"
        "${SR_SDK}/lib/SimulatedRealityDirectX${SR_SUFFIX}.lib"
        "${SR_SDK}/lib/SimulatedRealityDisplays${SR_SUFFIX}.lib"
        "${SR_OPENCV_LIB}"
        delayimp user32)
    target_link_options(${name} PRIVATE
        /DELAYLOAD:SimulatedRealityCore${SR_SUFFIX}.dll
        /DELAYLOAD:SimulatedRealityDirectX${SR_SUFFIX}.dll
        /DELAYLOAD:SimulatedRealityDisplays${SR_SUFFIX}.dll
        /DELAYLOAD:opencv_world343.dll)
    set_target_properties(${name} PROPERTIES PREFIX ""
        RUNTIME_OUTPUT_DIRECTORY "${SRWEAVE_OUT_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SRWEAVE_OUT_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${SRWEAVE_OUT_DIR}")
endfunction()

srweave_dll(SRWeaveDX9  dx9.cpp  dx9.def)
srweave_dll(SRWeaveDX11 dx11.cpp dx11.def)

cmake_minimum_required(VERSION 3.20)
project(pandora C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(PANDORA_GENERATED_DIR "${CMAKE_SOURCE_DIR}/build/recomp/generated" CACHE PATH
    "DolRecomp output directory (run tools/recompile.sh to create it)")
if(NOT EXISTS "${PANDORA_GENERATED_DIR}/generated.h")
    message(FATAL_ERROR "No recompiled sources in ${PANDORA_GENERATED_DIR}; run tools/recompile.sh first")
endif()

set(DOLRECOMP_DIR "${CMAKE_SOURCE_DIR}/extern/DolRecomp")

# aurora: GX renderer (WebGPU via Dawn) and SDL3 windowing. Only the GX pieces are used.
set(AURORA_ENABLE_DVD OFF CACHE BOOL "" FORCE)
set(AURORA_ENABLE_CARD OFF CACHE BOOL "" FORCE)
set(AURORA_ENABLE_THP OFF CACHE BOOL "" FORCE)
set(AURORA_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(AURORA_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(AURORA_ENABLE_RMLUI ON CACHE BOOL "" FORCE)  # settings menu (runtime/ui)
# Local fixes to aurora live in patches/aurora; apply any that are not applied yet.
file(GLOB AURORA_PATCHES "${CMAKE_SOURCE_DIR}/patches/aurora/*.patch")
foreach(patch IN LISTS AURORA_PATCHES)
    execute_process(COMMAND git apply --reverse --check "${patch}"
                    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/extern/aurora" RESULT_VARIABLE applied
                    OUTPUT_QUIET ERROR_QUIET)
    if(NOT applied EQUAL 0)
        execute_process(COMMAND git apply "${patch}" WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/extern/aurora"
                        RESULT_VARIABLE failed)
        if(failed)
            message(FATAL_ERROR "failed to apply ${patch}")
        endif()
        message(STATUS "applied ${patch}")
    endif()
endforeach()
add_subdirectory(extern/aurora EXCLUDE_FROM_ALL)
option(PANDORA_AURORA_DIAGNOSTICS "Build aurora's GX library with its debug diagnostics (unhandled registers etc.)" OFF)
if(PANDORA_AURORA_DIAGNOSTICS AND MSVC)
    target_compile_options(aurora_gx PRIVATE /UNDEBUG)
endif()

# Recompiled game code plus DolRecomp's CPU helpers. Built separately so the runtime can use
# its own warning settings.
file(GLOB PANDORA_CHUNKS CONFIGURE_DEPENDS "${PANDORA_GENERATED_DIR}/chunks/*.c")
add_library(guest STATIC
    ${PANDORA_CHUNKS}
    "${DOLRECOMP_DIR}/src/cpu/cpu.c"
    runtime/dispatch.c)
# runtime/fastmem shadows cpu/cpu.h for the chunks (inline RAM fast paths); cpu.c includes its own.
target_include_directories(guest PRIVATE "${CMAKE_SOURCE_DIR}/runtime/fastmem")
target_include_directories(guest PUBLIC "${DOLRECOMP_DIR}/src" "${PANDORA_GENERATED_DIR}")
if(MSVC)
    # Debug info (/Z7, in the objects) costs nothing at run time and lets profilers and crash
    # dumps name the recompiled functions.
    target_compile_options(guest PRIVATE /w /bigobj /MP /Z7)
    target_compile_definitions(guest PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(guest PRIVATE -w)
endif()

add_executable(pandora
    runtime/audio_dump.cpp
    runtime/audio_out.cpp
    runtime/boot.cpp
    runtime/dsp/accelerator.cpp
    runtime/dsp/ax.cpp
    runtime/dsp/ax_wii.cpp
    runtime/dsp/dsp.cpp
    runtime/dsp/ucode.cpp
    runtime/disc.cpp
    runtime/exi.cpp
    runtime/gpu/aurora_bridge.cpp
    runtime/gpu/command_processor.cpp
    runtime/gpu/vertex_layout.cpp
    runtime/hardware.cpp
    runtime/hle.cpp
    runtime/ios.cpp
    runtime/host_profiler.cpp
    runtime/input.cpp
    runtime/settings.cpp
    runtime/ui/settings_menu.cpp
    runtime/ios_devices.cpp
    runtime/main.cpp
    runtime/nand.cpp
    runtime/png_write.cpp
    runtime/runtime.cpp
    runtime/symbols.cpp)
target_link_libraries(pandora PRIVATE guest aurora::core aurora::gx aurora::vi)
# The bridge feeds aurora's internal FIFO directly.
target_include_directories(pandora PRIVATE "${CMAKE_SOURCE_DIR}/extern/aurora/lib")

# Runtime DLLs (Dawn, SDL3) next to the executable.
add_custom_command(TARGET pandora POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:pandora> $<TARGET_FILE_DIR:pandora>
    COMMAND_EXPAND_LISTS)
# Dawn's D3D12 backend loads the DirectX shader compiler at runtime.
set(DAWN_PREBUILT_BIN "${CMAKE_BINARY_DIR}/_deps/dawn_prebuilt-src/bin")
if(WIN32 AND EXISTS "${DAWN_PREBUILT_BIN}/dxil.dll")
    add_custom_command(TARGET pandora POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DAWN_PREBUILT_BIN}/dxcompiler.dll"
                "${DAWN_PREBUILT_BIN}/dxil.dll" $<TARGET_FILE_DIR:pandora>)
endif()
# UI assets (documents, styles, fonts) next to the executable, refreshed on every build.
add_custom_target(pandora_assets
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/assets" "$<TARGET_FILE_DIR:pandora>/assets")
add_dependencies(pandora pandora_assets)
if(MSVC)
    target_compile_options(pandora PRIVATE /W4 /permissive- /MP /Z7)
    target_link_options(pandora PRIVATE /DEBUG /OPT:REF /OPT:ICF)
    target_link_libraries(pandora PRIVATE dbghelp)
    target_compile_definitions(pandora PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

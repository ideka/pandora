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

# Recompiled game code plus DolRecomp's CPU helpers. Built separately so the runtime can use
# its own warning settings.
file(GLOB PANDORA_CHUNKS CONFIGURE_DEPENDS "${PANDORA_GENERATED_DIR}/chunks/*.c")
add_library(guest STATIC
    ${PANDORA_CHUNKS}
    "${DOLRECOMP_DIR}/src/cpu/cpu.c"
    runtime/dispatch.c)
target_include_directories(guest PUBLIC "${DOLRECOMP_DIR}/src" "${PANDORA_GENERATED_DIR}")
if(MSVC)
    target_compile_options(guest PRIVATE /w /bigobj /MP)
    target_compile_definitions(guest PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(guest PRIVATE -w)
endif()

add_executable(pandora
    runtime/boot.cpp
    runtime/exi.cpp
    runtime/hardware.cpp
    runtime/hle.cpp
    runtime/ios.cpp
    runtime/main.cpp
    runtime/runtime.cpp
    runtime/symbols.cpp)
target_link_libraries(pandora PRIVATE guest)
if(MSVC)
    target_compile_options(pandora PRIVATE /W4 /permissive- /MP)
endif()

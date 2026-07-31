cmake_minimum_required(VERSION 3.20)
project(SuperMarioRPGSNESRecomp C)
set(CMAKE_C_STANDARD 11)

set(SNESRECOMP_ROOT "${CMAKE_SOURCE_DIR}/snesrecomp" CACHE PATH
    "Path to the SA-1-capable snesrecomp checkout")
set(SNESRECOMP_SDL_BACKEND "SDL2" CACHE STRING
    "Desktop SDL backend (the SMRPG host currently uses SDL2)")
include(${SNESRECOMP_ROOT}/runner/runner.cmake)

set(RECOMP_UI_ROOT "${CMAKE_SOURCE_DIR}/../../recomp-ui" CACHE PATH
    "Path to the shared recomp-ui checkout")
include(${RECOMP_UI_ROOT}/recomp_ui.cmake)

file(GLOB SMRPG_GEN_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/gen/*.c")
if(NOT SMRPG_GEN_SOURCES)
    message(FATAL_ERROR
        "src/gen is empty; stage the verified ROM as smrpg.sfc and run "
        "`bash tools/regen.sh`.")
endif()

list(REMOVE_ITEM SNESRECOMP_RUNNER_SOURCES
    "${SNESRECOMP_ROOT}/runner/src/keybinds.c"
    "${SNESRECOMP_ROOT}/runner/src/host_report.c")

add_executable(SuperMarioRPGSNESRecompHeadless
    ${SNESRECOMP_RUNNER_SOURCES}
    ${SMRPG_GEN_SOURCES}
    src/headless_host.c
    src/headless_main.c
    src/smrpg_runtime.c)

add_executable(SuperMarioRPGSNESRecomp
    ${SNESRECOMP_RUNNER_SOURCES}
    ${SMRPG_GEN_SOURCES}
    src/sdl_main.c
    src/smrpg_runtime.c)

function(configure_smrpg_target target)
    target_include_directories(${target} PRIVATE
        "${CMAKE_SOURCE_DIR}/src"
        "${CMAKE_SOURCE_DIR}/recomp"
        ${SNESRECOMP_RUNNER_INCLUDE_DIRS})
    target_compile_definitions(${target} PRIVATE
        SYSTEM_VOLUME_MIXER_AVAILABLE=0
        SNESRECOMP_TRACE=$<BOOL:${SNESRECOMP_ENABLE_TRACE}>
        SNESRECOMP_REVERSE_DEBUG=0)
    if(NOT MSVC)
        target_compile_options(${target} PRIVATE
            $<$<COMPILE_LANGUAGE:C>:-w;-Wno-implicit-function-declaration>)
        target_link_libraries(${target} PRIVATE m)
    endif()
    target_link_libraries(${target} PRIVATE
        ${SNESRECOMP_RUNNER_LIBRARIES})
endfunction()

configure_smrpg_target(SuperMarioRPGSNESRecompHeadless)
configure_smrpg_target(SuperMarioRPGSNESRecomp)
snesrecomp_target_sdl(SuperMarioRPGSNESRecomp)
recomp_target_launcher_ui(SuperMarioRPGSNESRecomp CONSOLE snes)

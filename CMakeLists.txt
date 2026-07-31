cmake_minimum_required(VERSION 3.20)
project(SuperMarioRPGSNESRecomp C)
set(CMAKE_C_STANDARD 11)

set(SNESRECOMP_ROOT "${CMAKE_SOURCE_DIR}/snesrecomp" CACHE PATH
    "Path to the SA-1-capable snesrecomp checkout")
include(${SNESRECOMP_ROOT}/runner/runner.cmake)

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

target_include_directories(SuperMarioRPGSNESRecompHeadless PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/recomp"
    ${SNESRECOMP_RUNNER_INCLUDE_DIRS})
target_compile_definitions(SuperMarioRPGSNESRecompHeadless PRIVATE
    SYSTEM_VOLUME_MIXER_AVAILABLE=0
    SNESRECOMP_TRACE=$<BOOL:${SNESRECOMP_ENABLE_TRACE}>
    SNESRECOMP_REVERSE_DEBUG=0)
if(NOT MSVC)
    target_compile_options(SuperMarioRPGSNESRecompHeadless PRIVATE
        -w -Wno-implicit-function-declaration)
    target_link_libraries(SuperMarioRPGSNESRecompHeadless PRIVATE m)
endif()
target_link_libraries(SuperMarioRPGSNESRecompHeadless PRIVATE
    ${SNESRECOMP_RUNNER_LIBRARIES})

cmake_minimum_required(VERSION 3.20)
project(SuperMarioRPGSNESRecomp C)
set(CMAKE_C_STANDARD 11)

set(SNESRECOMP_ROOT "${CMAKE_SOURCE_DIR}/snesrecomp" CACHE PATH
    "Path to the SA-1-capable snesrecomp checkout (./snesrecomp submodule)")
set(SNESRECOMP_SDL_BACKEND "SDL3" CACHE STRING
    "Desktop SDL backend")
include(${SNESRECOMP_ROOT}/runner/runner.cmake)

# The SHARED recomp-ui launcher, consumed as the ./recomp-ui submodule like
# every other recomp game (this used to reach out to a sibling checkout via
# ../../recomp-ui, which only resolved on one machine's layout).
set(RECOMP_UI_ROOT "${CMAKE_SOURCE_DIR}/recomp-ui" CACHE PATH
    "Path to the shared recomp-ui checkout")
include(${RECOMP_UI_ROOT}/recomp_ui.cmake)

# Release version stamp: configure with -DSNESRECOMP_BUILD_VERSION=<ver>.
# Unset (local builds) the define is omitted and the host defaults to "dev".
# NOTE: quote it when configuring - PowerShell rewrites an unquoted
# -DSNESRECOMP_BUILD_VERSION=0.1.0 into "0".
set(SNESRECOMP_BUILD_VERSION "" CACHE STRING
    "Release version stamp baked into crash reports (empty = dev)")

file(GLOB SMRPG_GEN_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/gen/*.c")
if(NOT SMRPG_GEN_SOURCES)
    message(FATAL_ERROR
        "src/gen is empty; stage the verified ROM as smrpg.sfc and run "
        "`bash tools/regen.sh`.")
endif()

# host_report.c stays in: it owns the always-on breadcrumb ring, the crash
# minidump writer and the build-version stamp that every other SNES recomp
# ships. SMRPG previously replaced it with do-nothing stubs in sdl_main.c,
# which meant no crash reports at all - and, because the stub was inlined at
# -O3, the version string was dropped from the binary entirely.
list(REMOVE_ITEM SNESRECOMP_RUNNER_SOURCES
    "${SNESRECOMP_ROOT}/runner/src/keybinds.c")

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
# Hosts include "desktop/sdl_compat.h" rather than <SDL.h> so both majors build.
snesrecomp_target_sdl(SuperMarioRPGSNESRecomp)
recomp_target_launcher_ui(SuperMarioRPGSNESRecomp CONSOLE snes)

if(SNESRECOMP_BUILD_VERSION)
    target_compile_definitions(SuperMarioRPGSNESRecomp PRIVATE
        SNESRECOMP_BUILD_VERSION="${SNESRECOMP_BUILD_VERSION}")
endif()

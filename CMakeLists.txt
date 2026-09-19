cmake_minimum_required(VERSION 3.20)
project(SuperMarioRPGSNESRecomp C)
set(CMAKE_C_STANDARD 11)

set(SNESRECOMP_ROOT "${CMAKE_SOURCE_DIR}/snesrecomp" CACHE PATH
    "Path to the SA-1-capable snesrecomp checkout (./snesrecomp submodule)")
set(SNESRECOMP_SDL_BACKEND "SDL3" CACHE STRING
    "Desktop SDL backend")
set(SNESRECOMP_ENABLE_MODS ON CACHE BOOL
    "Enable SMRPG's built-in mod catalog and SNES mod packages" FORCE)
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

list(REMOVE_ITEM SNESRECOMP_RUNNER_SOURCES
    "${SNESRECOMP_ROOT}/runner/src/keybinds.c"
    "${SNESRECOMP_ROOT}/runner/src/snes_savestate_menu.c"
    "${SNESRECOMP_ROOT}/runner/src/snes_osd.c"
    "${SNESRECOMP_ROOT}/runner/src/host_report.c")

# The headless host keeps its own host_report stubs: it links no SDL, and
# host_report.c includes desktop/sdl_compat.h.
add_executable(SuperMarioRPGSNESRecompHeadless
    ${SNESRECOMP_RUNNER_SOURCES}
    ${SMRPG_GEN_SOURCES}
    src/headless_host.c
    src/headless_main.c
    src/smrpg_runtime.c
    src/smrpg_renderer.c)

# The desktop host takes the REAL host_report.c: it owns the always-on
# breadcrumb ring, the crash minidump writer and the build-version stamp that
# every other SNES recomp ships. SMRPG previously replaced it with do-nothing
# stubs in sdl_main.c, which meant no crash reports at all - and, because the
# stub was inlined at -O3, the version string was dropped from the binary.
#
# It also takes the REAL keybinds.c, for the same reason: the headless host
# links no SDL, so it can't carry a keyboard-scancode module, but the desktop
# host does link SDL and previously had its OWN hardcoded scancode->button
# table in sdl_main.c that ignored keybinds.ini entirely -- the launcher's
# Controls page (recomp-ui, a separately-prefixed recompui_keybinds_* copy of
# this same file) could edit and save that file, but the game never read it
# back. See sdl_main.c's keyboard_input() for the fix.
add_executable(SuperMarioRPGSNESRecomp
    ${SNESRECOMP_RUNNER_SOURCES}
    "${SNESRECOMP_ROOT}/runner/src/host_report.c"
    "${SNESRECOMP_ROOT}/runner/src/keybinds.c"
    ${SMRPG_GEN_SOURCES}
    src/mods/smrpg_coverage_plugin.c
    src/mods/smrpg_widescreen_plugin.c
    src/sdl_main.c
    src/smrpg_runtime.c
    src/smrpg_renderer.c)

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

add_executable(smrpg_render_capture tools/render_capture.c src/smrpg_renderer.c)
target_include_directories(smrpg_render_capture PRIVATE src ${SNESRECOMP_RUNNER_INCLUDE_DIRS})
include(CTest)
if(BUILD_TESTING)
    add_executable(smrpg_renderer_test tests/renderer_test.c)
    target_include_directories(smrpg_renderer_test PRIVATE src ${SNESRECOMP_RUNNER_INCLUDE_DIRS})
    add_test(NAME smrpg_renderer COMMAND smrpg_renderer_test)
endif()
# Hosts include "desktop/sdl_compat.h" rather than <SDL.h> so both majors build.
snesrecomp_target_sdl(SuperMarioRPGSNESRecomp)
recomp_target_launcher_ui(SuperMarioRPGSNESRecomp CONSOLE snes)

snesrecomp_target_mod_catalog(SuperMarioRPGSNESRecomp
    "${CMAKE_CURRENT_SOURCE_DIR}/mods/preloaded")

if(SNESRECOMP_BUILD_VERSION)
    target_compile_definitions(SuperMarioRPGSNESRecomp PRIVATE
        SNESRECOMP_BUILD_VERSION="${SNESRECOMP_BUILD_VERSION}")
endif()

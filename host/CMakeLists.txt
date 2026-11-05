# The host as one CMake project, for Windows (MSYS2's mingw32 gcc) and Linux (gcc -m32): the same sources, flags and
# output folders as build.ps1 and linux/build.sh.  core/ and shims/ are the generic host; games/<game>/ holds one game.
#
#   cmake --preset windows            plain build (-g, console) into ..\build, like build.ps1
#   cmake --preset windows-dev        like build.ps1 -Dev      (..\build\dev)
#   cmake --preset windows-release    like build.ps1 -Release  (..\build\release)
#   cmake --preset linux              like linux/build.sh      (../build/linux/<game>)
#   cmake --build --preset <the same name>
#
# Options (-D on the configure line):
#   GMV_GAME     the folder under games/ (default gangstar)
#   GMV_BUILD    plain, dev or release
#   GMV_NOICON   ON: leave out the game's app.ico even if it is present
#   GMV_OUT_DIR  where the program goes (empty: the folder build.ps1 / linux/build.sh use)
#   GMV_STAGE1   ON (Linux): the loader alone with the stubs in linux/, as linux/build_stage1.sh
#
# build.ps1, linux/build.sh and linux/build_stage1.sh are thin wrappers over this file, with their old arguments.
# Windows needs SDL3 from ..\tools\get_sdl3.ps1 first (third_party\SDL3, not in the repo); SDL3.dll is copied next to
# the exe.  Linux needs gcc-multilib and the 32-bit SDL3, GL and FreeType development files (see linux/build.sh).
# gmv_patch.bin is not built here:  python -I ../tools/genlow.py <binary> <output folder>/gmv_patch.bin
cmake_minimum_required(VERSION 3.20)

# 32-bit on Linux: the game is i386 code and runs in this process.
if(NOT CMAKE_HOST_WIN32 AND NOT CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/linux-i386.cmake")
endif()

project(gmv_host C ASM)

set(GMV_GAME gangstar CACHE STRING "the folder under games/")
set(GMV_BUILD plain CACHE STRING "plain, dev or release")
set_property(CACHE GMV_BUILD PROPERTY STRINGS plain dev release)
option(GMV_NOICON "leave out the game's app.ico even if it is present" OFF)
option(GMV_STAGE1 "Linux: the loader alone, with the stubs in linux/ (linux/build_stage1.sh)" OFF)
set(GMV_OUT_DIR "" CACHE PATH "where the program goes (empty: as build.ps1 / linux/build.sh)")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)           # compile_commands.json in the build folder, for editors

if(NOT GMV_BUILD MATCHES "^(plain|dev|release)$")
    message(FATAL_ERROR "GMV_BUILD must be plain, dev or release, not '${GMV_BUILD}'")
endif()
if(NOT CMAKE_C_COMPILER_ID STREQUAL "GNU" OR NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "needs a 32-bit gcc (found ${CMAKE_C_COMPILER_ID}, ${CMAKE_SIZEOF_VOID_P}-byte pointers: "
                        "${CMAKE_C_COMPILER}).  Windows: MSYS2's mingw32 gcc first on the PATH (the presets do it); "
                        "Linux: gcc-multilib")
endif()

set(here "${CMAKE_CURRENT_SOURCE_DIR}")
set(gdir "games/${GMV_GAME}")
if(NOT EXISTS "${here}/${gdir}/layout.h")
    message(FATAL_ERROR "no game folder ${gdir}")
endif()
file(STRINGS "${here}/${gdir}/layout.h" name_line REGEX "^#define GMV_GAME_NAME[ \t]+\"")
string(REGEX REPLACE ".*GMV_GAME_NAME[ \t]+\"([^\"]+)\".*" "\\1" exe_name "${name_line}")

# The flags below are the whole set, as in the scripts: a CMAKE_BUILD_TYPE adds nothing.
foreach(lang C ASM)
    foreach(cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
        set(CMAKE_${lang}_FLAGS_${cfg} "")
    endforeach()
endforeach()

# GMV_STAGE1 (Linux, linux/build_stage1.sh): the loader on its own, core/ with the stubs in linux/, which stops after
# verifying.  Run it with the game next to it, or:  ./<name>-stage1 --verify-only --app /path/to/<name>.app
if(GMV_STAGE1)
    if(WIN32)
        message(FATAL_ERROR "GMV_STAGE1 is the Linux loader check")
    endif()
    if(GMV_OUT_DIR)
        set(out "${GMV_OUT_DIR}")
    else()
        set(out "${here}/../build/linux")
    endif()
    get_filename_component(out "${out}" ABSOLUTE)
    file(GLOB src CONFIGURE_DEPENDS "${here}/core/*.c" "${here}/core/*.S")
    list(APPEND src "${here}/linux/stage1_stubs.c" "${here}/${gdir}/settings.c")
    list(SORT src CASE INSENSITIVE)
    add_executable(stage1 ${src})
    set_target_properties(stage1 PROPERTIES OUTPUT_NAME "${exe_name}-stage1" RUNTIME_OUTPUT_DIRECTORY "${out}")
    target_include_directories(stage1 PRIVATE "${here}/core" "${here}/shims" "${here}/${gdir}")
    target_compile_options(stage1 PRIVATE -std=gnu11 -O2 -g -Wall -Wextra -msse2 -fno-pic -freg-struct-return)
    target_compile_definitions(stage1 PRIVATE _FILE_OFFSET_BITS=64)
    target_link_options(stage1 PRIVATE -no-pie -Wl,-Ttext-segment=0x08048000)
    target_link_libraries(stage1 PRIVATE pthread dl)
    add_custom_command(TARGET stage1 POST_BUILD COMMAND "${CMAKE_COMMAND}" -E echo "built $<TARGET_FILE:stage1>" VERBATIM)
    message(STATUS "${GMV_GAME}, loader alone: ${out}/${exe_name}-stage1")
    return()
endif()

# Output folder: the same as the build scripts'.
if(GMV_OUT_DIR)
    set(out "${GMV_OUT_DIR}")
elseif(WIN32)
    set(out "${here}/../build")
    if(NOT GMV_GAME STREQUAL "gangstar")
        string(APPEND out "/${GMV_GAME}")
    endif()
else()
    set(out "${here}/../build/linux/${GMV_GAME}")
endif()
if(NOT GMV_OUT_DIR AND NOT GMV_BUILD STREQUAL "plain")
    string(APPEND out "/${GMV_BUILD}")
endif()
get_filename_component(out "${out}" ABSOLUTE)

# Sources: every .c and .S in core/, shims/, shims/gl/ and the game's folder tree.  LOAD_PROFILE and PERF_PROFILE are
# diagnostics, left out of a release.
file(GLOB src CONFIGURE_DEPENDS
     "${here}/core/*.c" "${here}/core/*.S" "${here}/shims/*.c" "${here}/shims/*.S"
     "${here}/shims/gl/*.c" "${here}/shims/gl/*.S")
file(GLOB_RECURSE game_src CONFIGURE_DEPENDS "${here}/${gdir}/*.c" "${here}/${gdir}/*.S")
list(APPEND src ${game_src})
if(GMV_BUILD STREQUAL "release")
    list(FILTER src EXCLUDE REGEX "/(loadprof|perf_probes)\\.c$")
endif()
list(SORT src CASE INSENSITIVE)                 # build.ps1's order (Sort-Object), so the same layout

# Include folders: core, shims, shims/gl, then every folder of the game's tree.
file(GLOB_RECURSE game_items LIST_DIRECTORIES true "${here}/${gdir}/*")
set(game_dirs "")
foreach(p IN LISTS game_items)
    if(IS_DIRECTORY "${p}")
        list(APPEND game_dirs "${p}")
    endif()
endforeach()
list(SORT game_dirs)

add_executable(host ${src})
set_target_properties(host PROPERTIES OUTPUT_NAME "${exe_name}" RUNTIME_OUTPUT_DIRECTORY "${out}")
target_include_directories(host PRIVATE "${here}/core" "${here}/shims" "${here}/shims/gl" ${game_dirs}
                                        "${here}/${gdir}")
target_include_directories(host SYSTEM PRIVATE "${here}/third_party/SDL3/include" "${here}/vendor")
target_compile_options(host PRIVATE -std=gnu11 -O2 -Wall -Wextra)
# The custom radio's decoders (vendor/) do their float maths in SSE2 on both systems, so a song decodes to the same
# samples on Windows and Linux (x87 rounds differently); they run on a thread of their own, hence the realign.
set_source_files_properties("${here}/shims/songdec_impl.c" PROPERTIES COMPILE_OPTIONS "-msse2;-mfpmath=sse;-mstackrealign")
if(GMV_BUILD STREQUAL "plain")
    target_compile_options(host PRIVATE -g)
else()
    # no local paths in the program
    target_compile_options(host PRIVATE "-ffile-prefix-map=${here}=host")
    target_link_options(host PRIVATE -s)
endif()
if(GMV_BUILD STREQUAL "release")
    target_compile_definitions(host PRIVATE GMV_RELEASE)
endif()

set(icon "${here}/${gdir}/app.ico")
if(GMV_NOICON OR NOT EXISTS "${icon}")
    message(STATUS "building without an icon")
    set(icon "")
endif()

if(WIN32)
    set(sdl "${here}/third_party/SDL3")
    if(NOT EXISTS "${sdl}/lib/libSDL3.dll.a")
        message(FATAL_ERROR "no SDL3: run tools\\get_sdl3.ps1")
    endif()
    # Only what the build scripts link, and CMake's own exe link rule without its import library and image version.
    set(CMAKE_C_STANDARD_LIBRARIES "")
    set(CMAKE_C_LINK_EXECUTABLE "<CMAKE_C_COMPILER> <FLAGS> <CMAKE_C_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")
    target_compile_options(host PRIVATE -municode)
    # -mwindows in a release: no console window; everything still goes to cache\<game>.log unless LOG=0.
    if(GMV_BUILD STREQUAL "release")
        set_target_properties(host PROPERTIES WIN32_EXECUTABLE ON)
        target_link_options(host PRIVATE -mwindows)
    else()
        target_link_options(host PRIVATE -mconsole)
    endif()
    # --image-base 0x10000 with no relocations: the exe must land exactly there (see layout.h).  The import library
    # is linked by its path, as -static would not look for it by name.
    target_link_options(host PRIVATE -municode -static -O2 -T "${here}/core/gmv.ld"
        -Wl,--image-base,0x10000 -Wl,--disable-dynamicbase -Wl,--disable-reloc-section
        -Wl,--large-address-aware -Wl,--nxcompat)
    target_link_libraries(host PRIVATE "${sdl}/lib/libSDL3.dll.a" opengl32 gdi32 winmm
                                       mfplat mfreadwrite mfuuid ole32)
    # The application icon: app.rc -> a .rsrc section; windres finds app.ico through the game's include folder.
    if(icon)
        enable_language(RC)
        target_sources(host PRIVATE "${here}/${gdir}/app.rc")
        set_source_files_properties("${here}/${gdir}/app.rc" PROPERTIES OBJECT_DEPENDS "${icon}")
    endif()
    add_custom_command(TARGET host POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${sdl}/bin/SDL3.dll" "${out}"
        VERBATIM)
else()
    target_compile_options(host PRIVATE -msse2 -fno-pic -freg-struct-return)
    target_compile_definitions(host PRIVATE _FILE_OFFSET_BITS=64)
    # FreeType's headers are for gangstar's pad pictures, which open libfreetype.so.6 when they run.
    target_include_directories(host SYSTEM PRIVATE /usr/include/freetype2)
    # The link address keeps the program above the game's address range, which it maps when it starts.
    target_link_options(host PRIVATE -no-pie -Wl,-Ttext-segment=0x08048000)
    target_link_libraries(host PRIVATE SDL3 GL m pthread dl)
    # app.ico is built into the program (.incbin in gl_context.c) and becomes the window's icon.
    if(icon)
        target_compile_definitions(host PRIVATE "GMV_ICON_FILE=\"${icon}\"")
        set_source_files_properties("${here}/shims/gl/gl_context.c" PROPERTIES OBJECT_DEPENDS "${icon}")
    endif()
endif()

# A release or dev folder gets the add-on and custom radio folders, each with a note on what goes there.
if(GMV_GAME STREQUAL "gangstar" AND NOT GMV_BUILD STREQUAL "plain")
    foreach(pair "add-on|add-on.txt" "custom radio|custom-radio.txt")
        string(REPLACE "|" ";" pair "${pair}")
        list(GET pair 0 folder)
        list(GET pair 1 note)
        add_custom_command(TARGET host POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${out}/${folder}"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${here}/../docs/folder-notes/${note}"
                    "${out}/${folder}/WHAT GOES HERE.txt"
            VERBATIM)
    endforeach()
endif()

add_custom_command(TARGET host POST_BUILD COMMAND "${CMAKE_COMMAND}" -E echo "built $<TARGET_FILE:host>" VERBATIM)
message(STATUS "${GMV_GAME}, ${GMV_BUILD} build: ${out}/${exe_name}")

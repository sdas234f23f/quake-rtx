cmake_minimum_required(VERSION 3.20)
project(vkquake LANGUAGES C CXX)

# ============================================================================
# C / C++ Standard
# ============================================================================
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ============================================================================
# Options (matching the VS project defines)
# ============================================================================
option(USE_CODEC_MP3    "Enable MP3 audio codec (libmad)"     ON)
option(USE_CODEC_VORBIS "Enable Vorbis audio codec"           ON)
option(USE_CODEC_WAVE   "Enable WAVE audio codec"             ON)
option(USE_CODEC_FLAC   "Enable FLAC audio codec"             ON)
option(USE_CODEC_OPUS   "Enable Opus audio codec"             ON)
option(USE_CODEC_UMX    "Enable UMX (Unreal) music"           ON)

option(ENABLE_MIMALLOC  "Use mimalloc allocator"              OFF)

# ============================================================================
# Platform detection
# ============================================================================
if(WIN32)
    set(VKQUAKE_PLATFORM_WINDOWS ON)
elseif(APPLE)
    set(VKQUAKE_PLATFORM_MACOS ON)
else()
    set(VKQUAKE_PLATFORM_LINUX ON)
endif()

# ============================================================================
# Compiler settings
# ============================================================================
if(MSVC)
    # Match the VS project: /W4, /WX, /MP.
    # The strict flags are scoped to C only: the vendored vkpt renderer
    # (C++) sets its own /W3 /EHsc flags in subprojects/vkpt/CMakeLists.txt.
    add_compile_options("$<$<COMPILE_LANGUAGE:C>:/W4>" "$<$<COMPILE_LANGUAGE:C>:/WX>" "$<$<COMPILE_LANGUAGE:C>:/MP>")
    add_compile_options("$<$<COMPILE_LANGUAGE:C>:/wd4100>" "$<$<COMPILE_LANGUAGE:C>:/wd4127>" "$<$<COMPILE_LANGUAGE:C>:/wd4389>" "$<$<COMPILE_LANGUAGE:C>:/wd4702>" "$<$<COMPILE_LANGUAGE:C>:/wd4706>" "$<$<COMPILE_LANGUAGE:C>:/wd5286>")

    add_compile_definitions(
        _CRT_NONSTDC_NO_DEPRECATE
        _CRT_SECURE_NO_WARNINGS
        _WINSOCK_DEPRECATED_NO_WARNINGS
    )

    # Disable exception handling (C project)
    add_compile_options($<$<COMPILE_LANGUAGE:C>:/EHsc->)
else()
    add_compile_options(-Wall -Wno-trigraphs -Wno-unused-function -Werror)

    if(CMAKE_BUILD_TYPE STREQUAL "Release" OR NOT CMAKE_BUILD_TYPE)
        add_compile_options(-O3)
    endif()
endif()

# ============================================================================
# Codec compile definitions
# ============================================================================
function(_add_codec_define opt name)
    if(${opt})
        add_compile_definitions(${name})
    endif()
endfunction()

_add_codec_define(USE_CODEC_MP3    USE_CODEC_MP3)
_add_codec_define(USE_CODEC_VORBIS USE_CODEC_VORBIS)
_add_codec_define(USE_CODEC_WAVE   USE_CODEC_WAVE)
_add_codec_define(USE_CODEC_FLAC   USE_CODEC_FLAC)
_add_codec_define(USE_CODEC_OPUS   USE_CODEC_OPUS)
_add_codec_define(USE_CODEC_UMX    USE_CODEC_UMX)

# ============================================================================
# Find Vulkan
#   On Windows, the project bundles vulkan-1.lib + headers in Windows/misc/
#   but prefer the system Vulkan SDK if installed.
# ============================================================================
if(VKQUAKE_PLATFORM_WINDOWS)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(VK_BUNDLED_LIB_DIR "${CMAKE_SOURCE_DIR}/Windows/misc/x64")
    else()
        set(VK_BUNDLED_LIB_DIR "${CMAKE_SOURCE_DIR}/Windows/misc/x86")
    endif()
    set(VK_BUNDLED_INC_DIR "${CMAKE_SOURCE_DIR}/Windows/misc/include")

    # Try system Vulkan SDK first, fall back to bundled
    find_package(Vulkan QUIET)
    if(NOT Vulkan_FOUND)
        message(STATUS "Vulkan SDK not found, using bundled Vulkan from Windows/misc/")
        add_library(Vulkan::Vulkan SHARED IMPORTED)
        set_target_properties(Vulkan::Vulkan PROPERTIES
            IMPORTED_LOCATION "${VK_BUNDLED_LIB_DIR}/vulkan-1.lib"
            INTERFACE_INCLUDE_DIRECTORIES "${VK_BUNDLED_INC_DIR}"
        )
        set(Vulkan_FOUND TRUE)
    endif()
else()
    find_package(Vulkan REQUIRED)
endif()

# ============================================================================
# SDL3 — bundled on Windows (vkQuake requires SDL3 there)
# ============================================================================
if(VKQUAKE_PLATFORM_WINDOWS)
    set(SDL3_ROOT "${CMAKE_SOURCE_DIR}/Windows/SDL3")
    set(SDL3_INCLUDE_DIR "${SDL3_ROOT}/include")

    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(SDL3_LIB_DIR "${SDL3_ROOT}/lib64")
    else()
        set(SDL3_LIB_DIR "${SDL3_ROOT}/lib")
    endif()

    # SDL3.lib is an import library for SDL3.dll
    add_library(SDL3::SDL3 SHARED IMPORTED)
    set_target_properties(SDL3::SDL3 PROPERTIES
        IMPORTED_LOCATION "${SDL3_LIB_DIR}/SDL3.dll"
        IMPORTED_IMPLIB   "${SDL3_LIB_DIR}/SDL3.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${SDL3_INCLUDE_DIR}"
    )
else()
    find_package(SDL3 REQUIRED)
endif()

# ============================================================================
# Windows bundled codec libraries
# ============================================================================
if(VKQUAKE_PLATFORM_WINDOWS)
    set(CODECS_ROOT "${CMAKE_SOURCE_DIR}/Windows/codecs")

    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(CODECS_LIB_DIR "${CODECS_ROOT}/x64")
    else()
        set(CODECS_LIB_DIR "${CODECS_ROOT}/x86")
    endif()
    set(CODECS_INC_DIR "${CODECS_ROOT}/include")

    # Helper: create an imported DLL library from bundled .lib (import lib) + .dll
    function(_add_codec_dll target_name lib_file dll_file)
        add_library(${target_name} SHARED IMPORTED)
        set_target_properties(${target_name} PROPERTIES
            IMPORTED_LOCATION "${CODECS_LIB_DIR}/${dll_file}"
            IMPORTED_IMPLIB   "${CODECS_LIB_DIR}/${lib_file}"
            INTERFACE_INCLUDE_DIRECTORIES "${CODECS_INC_DIR}"
        )
    endfunction()

    if(USE_CODEC_MP3)
        _add_codec_dll(codec::mad "libmad.lib" "libmad-0.dll")
    endif()
    if(USE_CODEC_VORBIS)
        _add_codec_dll(codec::vorbis     "libvorbis.lib"      "libvorbis-0.dll")
        _add_codec_dll(codec::vorbisfile "libvorbisfile.lib"  "libvorbisfile-3.dll")
        _add_codec_dll(codec::ogg        "libogg.lib"         "libogg-0.dll")
    endif()
    if(USE_CODEC_FLAC)
        _add_codec_dll(codec::FLAC "libFLAC.lib" "libFLAC-8.dll")
    endif()
    if(USE_CODEC_OPUS)
        _add_codec_dll(codec::opus     "libopus.lib"     "libopus-0.dll")
        _add_codec_dll(codec::opusfile "libopusfile.lib" "libopusfile-0.dll")
    endif()
endif()

# ============================================================================
# vkpt renderer — vendored
# ============================================================================
# The renderer source lives in subprojects/vkpt/ and is built as a static
# library linked into vkquake. The public RG_* API remains an internal
# interface (RG_STATIC is defined in subprojects/vkpt/CMakeLists.txt).
add_subdirectory(subprojects/vkpt)

message(STATUS "vkpt renderer: vendored (static, built from subprojects/vkpt/)")

# ============================================================================
# Native Vulkan shader pipeline (ported from meson.build)
#   glslang -> .spv -> bintoc -> .c  (+ spirv-opt in release)
#   mkpak -> vkquake.pak -> bintoc -c -> embedded_pak.c
# ============================================================================
find_program(GLSLANG_VALIDATOR glslangValidator REQUIRED)
find_program(SPIRV_OPT spirv-opt REQUIRED)

add_executable(bintoc Shaders/bintoc.c)
add_executable(mkpak Misc/vq_pak/mkpak.c)

# These are native build tools, not part of the game; don't treat warnings as
# errors for them (bintoc.c has a benign C4701 under /W4 /WX).
target_compile_options(bintoc PRIVATE $<$<COMPILE_LANGUAGE:C>:/w>)
target_compile_options(mkpak PRIVATE $<$<COMPILE_LANGUAGE:C>:/w>)

set(GLSLANG_ARGS -V --quiet)
set(SPIRV_OPT_ARGS)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    list(APPEND GLSLANG_ARGS -g)
else()
    set(SPIRV_OPT_ARGS -Os --canonicalize-ids --strip-debug)
endif()

set(SHADER_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${SHADER_OUT_DIR}")

set(SHADER_C_FILES)

# vkq_add_shader(<src> <name> [glslang extra args...])
function(vkq_add_shader src name)
    set(extra_args ${ARGN})
    set(raw "${SHADER_OUT_DIR}/${name}.raw.spv")
    set(spv "${SHADER_OUT_DIR}/${name}.spv")
    set(cfile "${SHADER_OUT_DIR}/${name}.c")

    if(SPIRV_OPT_ARGS)
        add_custom_command(
            OUTPUT "${raw}"
            COMMAND "${GLSLANG_VALIDATOR}" ${GLSLANG_ARGS} ${extra_args} -o "${raw}" "${CMAKE_CURRENT_SOURCE_DIR}/${src}"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${src}"
            COMMENT "glslang ${name}")
        add_custom_command(
            OUTPUT "${spv}"
            COMMAND "${SPIRV_OPT}" ${SPIRV_OPT_ARGS} "${raw}" -o "${spv}"
            DEPENDS "${raw}")
    else()
        add_custom_command(
            OUTPUT "${spv}"
            COMMAND "${GLSLANG_VALIDATOR}" ${GLSLANG_ARGS} ${extra_args} -o "${spv}" "${CMAKE_CURRENT_SOURCE_DIR}/${src}"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${src}"
            COMMENT "glslang ${name}")
    endif()

    add_custom_command(
        OUTPUT "${cfile}"
        COMMAND "$<TARGET_FILE:bintoc>" "${spv}" "${name}_spv" "${cfile}"
        DEPENDS "${spv}" bintoc)
    list(APPEND SHADER_C_FILES "${cfile}")
    set(SHADER_C_FILES "${SHADER_C_FILES}" PARENT_SCOPE)
endfunction()

# Base shaders (single compile)
vkq_add_shader(Shaders/alias.vert            alias.vert)
vkq_add_shader(Shaders/md5.vert              md5.vert)
vkq_add_shader(Shaders/md5.vert              md5_8.vert -DEIGHT_WEIGHT_SKINNING)
vkq_add_shader(Shaders/md5_debug.vert        md5_debug.vert)
vkq_add_shader(Shaders/basic.vert            basic.vert)
vkq_add_shader(Shaders/basic_alphatest.frag  basic_alphatest.frag)
vkq_add_shader(Shaders/basic_notex.frag      basic_notex.frag)
vkq_add_shader(Shaders/cs_tex_warp.comp      cs_tex_warp.comp)
vkq_add_shader(Shaders/indirect.comp         indirect.comp)
vkq_add_shader(Shaders/indirect_clear.comp   indirect_clear.comp)
vkq_add_shader(Shaders/postprocess.frag      postprocess.frag)
vkq_add_shader(Shaders/postprocess.vert      postprocess.vert)
vkq_add_shader(Shaders/showtris.frag         showtris.frag)
vkq_add_shader(Shaders/showtris.vert         showtris.vert)
vkq_add_shader(Shaders/sky_box.frag          sky_box.frag)
vkq_add_shader(Shaders/sky_cube.frag         sky_cube.frag)
vkq_add_shader(Shaders/sky_cube.vert         sky_cube.vert)
vkq_add_shader(Shaders/sky_layer.frag        sky_layer.frag)
vkq_add_shader(Shaders/sky_layer.vert        sky_layer.vert)
vkq_add_shader(Shaders/world.vert            world.vert)
vkq_add_shader(Shaders/ray_debug.comp       ray_debug.comp)
vkq_add_shader(Shaders/mesh_interpolate.comp mesh_interpolate.comp)
vkq_add_shader(Shaders/skinning.comp         skinning.comp)
vkq_add_shader(Shaders/skinning.comp         skinning_8.comp -DEIGHT_WEIGHT_SKINNING)

# Shader variants (same source, different defines)
vkq_add_shader(Shaders/basic.frag basic.frag)
vkq_add_shader(Shaders/basic.frag basic_oit.frag -DWBOIT=1)
vkq_add_shader(Shaders/basic.frag basic_mboit_moment.frag -DMBOIT=1)
vkq_add_shader(Shaders/basic.frag basic_mboit_composite.frag -DMBOIT=1 -DMBOIT_COMPOSITE=1)
vkq_add_shader(Shaders/basic.frag basic_mboit_composite_msaa.frag -DMBOIT=1 -DMBOIT_COMPOSITE=1 -DMSAA=1)
vkq_add_shader(Shaders/world.frag world.frag)
vkq_add_shader(Shaders/world.frag world_oit.frag -DWBOIT=1)
vkq_add_shader(Shaders/world.frag world_mboit_moment.frag -DMBOIT=1)
vkq_add_shader(Shaders/world.frag world_mboit_composite.frag -DMBOIT=1 -DMBOIT_COMPOSITE=1)
vkq_add_shader(Shaders/world.frag world_mboit_composite_msaa.frag -DMBOIT=1 -DMBOIT_COMPOSITE=1 -DMSAA=1)
vkq_add_shader(Shaders/alias.frag alias.frag)
vkq_add_shader(Shaders/alias.frag alias_alphatest.frag -DALIAS_ALPHA_TEST=1)
vkq_add_shader(Shaders/alias.frag alias_oit.frag -DWBOIT=1)
vkq_add_shader(Shaders/alias.frag alias_alphatest_oit.frag -DALIAS_ALPHA_TEST=1 -DWBOIT=1)
vkq_add_shader(Shaders/alias.frag alias_mboit_moment.frag -DMBOIT=1)
vkq_add_shader(Shaders/alias.frag alias_alphatest_mboit_moment.frag -DALIAS_ALPHA_TEST=1 -DMBOIT=1)
vkq_add_shader(Shaders/alias.frag alias_mboit_composite.frag -DMBOIT=1 -DMBOIT_COMPOSITE=1)
vkq_add_shader(Shaders/alias.frag alias_alphatest_mboit_composite.frag -DALIAS_ALPHA_TEST=1 -DMBOIT=1 -DMBOIT_COMPOSITE=1)
vkq_add_shader(Shaders/alias.frag alias_mboit_composite_msaa.frag -DMBOIT=1 -DMBOIT_COMPOSITE=1 -DMSAA=1)
vkq_add_shader(Shaders/alias.frag alias_alphatest_mboit_composite_msaa.frag -DALIAS_ALPHA_TEST=1 -DMBOIT=1 -DMBOIT_COMPOSITE=1 -DMSAA=1)
vkq_add_shader(Shaders/alias.frag md5_mboit_composite.frag -DMBOIT=1 -DMBOIT_COMPOSITE=1 -DMBOIT_INPUT_SET=4)
vkq_add_shader(Shaders/alias.frag md5_alphatest_mboit_composite.frag -DALIAS_ALPHA_TEST=1 -DMBOIT=1 -DMBOIT_COMPOSITE=1 -DMBOIT_INPUT_SET=4)
vkq_add_shader(Shaders/alias.frag md5_mboit_composite_msaa.frag -DMBOIT=1 -DMBOIT_COMPOSITE=1 -DMSAA=1 -DMBOIT_INPUT_SET=4)
vkq_add_shader(Shaders/alias.frag md5_alphatest_mboit_composite_msaa.frag -DALIAS_ALPHA_TEST=1 -DMBOIT=1 -DMBOIT_COMPOSITE=1 -DMSAA=1 -DMBOIT_INPUT_SET=4)
vkq_add_shader(Shaders/wboit_resolve.frag wboit_resolve.frag)
vkq_add_shader(Shaders/wboit_resolve.frag wboit_resolve_msaa.frag -DMSAA=1)
vkq_add_shader(Shaders/mboit_resolve.frag mboit_resolve.frag)
vkq_add_shader(Shaders/mboit_resolve.frag mboit_resolve_msaa.frag -DMSAA=1)
vkq_add_shader(Shaders/screen_effects.comp screen_effects_8bit.comp)
vkq_add_shader(Shaders/screen_effects.comp screen_effects_8bit_scale.comp -DSCALING=1)
vkq_add_shader(Shaders/screen_effects.comp screen_effects_8bit_scale_sops.comp --target-env vulkan1.1 -DSCALING=1 -DUSE_SUBGROUP_OPS=1)
vkq_add_shader(Shaders/screen_effects.comp screen_effects_10bit.comp -DUSE_10BIT=1)
vkq_add_shader(Shaders/screen_effects.comp screen_effects_10bit_scale.comp -DUSE_10BIT=1 -DSCALING=1)
vkq_add_shader(Shaders/screen_effects.comp screen_effects_10bit_scale_sops.comp --target-env vulkan1.1 -DUSE_10BIT=1 -DSCALING=1 -DUSE_SUBGROUP_OPS=1)
vkq_add_shader(Shaders/update_lightmap.comp update_lightmap_8bit.comp)
vkq_add_shader(Shaders/update_lightmap.comp update_lightmap_8bit_rt.comp -DRAY_QUERIES=1)
vkq_add_shader(Shaders/update_lightmap.comp update_lightmap_10bit.comp -DUSE_10BIT=1)
vkq_add_shader(Shaders/update_lightmap.comp update_lightmap_10bit_rt.comp -DUSE_10BIT=1 -DRAY_QUERIES=1)

# embedded pak (menus, fonts, etc. via Misc/vq_pak)
set(VQ_PAK_PATH "${CMAKE_SOURCE_DIR}/Misc/vq_pak")
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/vkquake.pak"
    COMMAND "$<TARGET_FILE:mkpak>" "${CMAKE_CURRENT_BINARY_DIR}/vkquake.pak" "${VQ_PAK_PATH}" "${VQ_PAK_PATH}/vq_pak_contents.txt" "${CMAKE_CURRENT_BINARY_DIR}/vkquake.pak.d"
    DEPENDS mkpak "${VQ_PAK_PATH}/vq_pak_contents.txt")
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/embedded_pak.c"
    COMMAND "$<TARGET_FILE:bintoc>" -c "${CMAKE_CURRENT_BINARY_DIR}/vkquake.pak" vkquake.pak "${CMAKE_CURRENT_BINARY_DIR}/embedded_pak.c"
    DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/vkquake.pak" bintoc)

# ============================================================================
# Source files (mirrors meson.build srcs)
# ============================================================================

# --- Core sources (all platforms) ---
set(CORE_SOURCES
    Quake/bgmusic.c
    Quake/cd_null.c
    Quake/cfgfile.c
    Quake/chase.c
    Quake/cl_demo.c
    Quake/cl_input.c
    Quake/cl_main.c
    Quake/cl_parse.c
    Quake/cl_tent.c
    Quake/cmd.c
    Quake/common.c
    Quake/console.c
    Quake/crc.c
    Quake/cvar.c
    Quake/gl_draw.c
    Quake/gl_fog.c
    Quake/gl_heap.c
    Quake/gl_mesh.c
    Quake/gl_model.c
    Quake/gl_refrag.c
    Quake/gl_rlight.c
    Quake/gl_rmain.c
    Quake/gl_rmisc.c
    Quake/gl_screen.c
    Quake/gl_sky.c
    Quake/gl_texmgr.c
    Quake/gl_vidsdl.c
    Quake/gl_warp.c
    Quake/hash_map.c
    Quake/host.c
    Quake/host_cmd.c
    Quake/image.c
    Quake/json.c
    Quake/keys.c
    Quake/mathlib.c
    Quake/mdfour.c
    Quake/mem.c
    Quake/menu.c
    Quake/palette.c
    Quake/pr_cmds.c
    Quake/pr_edict.c
    Quake/pr_exec.c
    Quake/pr_ext.c
    Quake/r_alias.c
    Quake/r_brush.c
    Quake/r_part.c
    Quake/r_part_fte.c
    Quake/r_sprite.c
    Quake/r_world.c
    Quake/rt_material.c
    Quake/rt_pkz.c
    Quake/sbar.c
    Quake/snd_codec.c
    Quake/snd_dma.c
    Quake/snd_mem.c
    Quake/snd_mix.c
    Quake/snd_wave.c
    Quake/steam.c
    Quake/strlcat.c
    Quake/strlcpy.c
    Quake/sv_main.c
    Quake/sv_move.c
    Quake/sv_phys.c
    Quake/sv_user.c
    Quake/sys_sdl.c
    Quake/tasks.c
    Quake/view.c
    Quake/wad.c
    Quake/world.c
)

# --- SDL layer (all platforms) ---
set(SDL_SOURCES
    Quake/gl_vidsdl.c
    Quake/in_sdl.c
    Quake/in_sdl2.c
    Quake/in_sdl3.c
    Quake/main_sdl.c
    Quake/snd_sdl.c
    Quake/snd_sdl3.c
)

# --- Networking (all platforms) ---
set(NET_COMMON_SOURCES
    Quake/net_dgrm.c
    Quake/net_loop.c
    Quake/net_main.c
)

# --- Codec sources ---
set(CODEC_SOURCES)
if(USE_CODEC_MP3)
    list(APPEND CODEC_SOURCES Quake/snd_mp3.c Quake/snd_mp3tag.c)
endif()
if(USE_CODEC_VORBIS)
    list(APPEND CODEC_SOURCES Quake/snd_vorbis.c)
endif()
if(USE_CODEC_FLAC)
    list(APPEND CODEC_SOURCES Quake/snd_flac.c)
endif()
if(USE_CODEC_OPUS)
    list(APPEND CODEC_SOURCES Quake/snd_opus.c)
endif()
if(USE_CODEC_UMX)
    list(APPEND CODEC_SOURCES Quake/snd_umx.c)
endif()

# --- Platform-specific sources ---
if(VKQUAKE_PLATFORM_WINDOWS)
    set(PLATFORM_SOURCES
        Quake/net_win.c
        Quake/net_wins.c
        Quake/net_wipx.c
        Quake/pl_win.c
        Quake/sys_sdl_win.c
    )
    set(PLATFORM_RESOURCE Windows/vkQuake.rc)
elseif(VKQUAKE_PLATFORM_LINUX)
    set(PLATFORM_SOURCES
        Quake/net_bsd.c
        Quake/net_udp.c
        Quake/pl_linux.c
        Quake/sys_sdl_unix.c
    )
    set(PLATFORM_RESOURCE "")
elseif(VKQUAKE_PLATFORM_MACOS)
    set(PLATFORM_SOURCES
        Quake/net_bsd.c
        Quake/net_udp.c
        Quake/pl_osx.m
        Quake/sys_sdl_unix.c
    )
    set(PLATFORM_RESOURCE "")
endif()

# --- mimalloc (optional) ---
set(MIMALLOC_SOURCES)
if(ENABLE_MIMALLOC)
    file(GLOB MIMALLOC_FILES Quake/mimalloc/*.c)
    list(FILTER MIMALLOC_FILES EXCLUDE REGEX ".*alloc-override-osx\\.c$")
    set(MIMALLOC_SOURCES ${MIMALLOC_FILES})
    add_compile_definitions(USE_MIMALLOC)
endif()

# ============================================================================
# Combine all sources
# ============================================================================
set(ALL_SOURCES
    ${CORE_SOURCES}
    ${SDL_SOURCES}
    ${NET_COMMON_SOURCES}
    ${CODEC_SOURCES}
    ${PLATFORM_SOURCES}
    ${MIMALLOC_SOURCES}
    ${SHADER_C_FILES}
    "${CMAKE_CURRENT_BINARY_DIR}/embedded_pak.c"
)

if(PLATFORM_RESOURCE)
    list(APPEND ALL_SOURCES ${PLATFORM_RESOURCE})
endif()

# ============================================================================
# Create executable
# ============================================================================
add_executable(vkquake ${ALL_SOURCES})

# ============================================================================
# Include directories
# ============================================================================
target_include_directories(vkquake PRIVATE
    ${CMAKE_SOURCE_DIR}/Quake
    ${CMAKE_SOURCE_DIR}/Quake/mimalloc
    ${CMAKE_CURRENT_BINARY_DIR}
)

if(VKQUAKE_PLATFORM_WINDOWS)
    target_include_directories(vkquake PRIVATE
        ${CODECS_INC_DIR}
        ${SDL3_INCLUDE_DIR}
    )
endif()

# ============================================================================
# Compile definitions (per-target)
# ============================================================================
target_compile_definitions(vkquake PRIVATE
    $<$<CONFIG:Debug>:_DEBUG>
    $<$<CONFIG:Release>:NDEBUG>
    USE_SDL3
    # The vendored vkpt renderer is linked statically: RGAPI must be empty.
    RG_STATIC
)

if(VKQUAKE_PLATFORM_WINDOWS)
    # RG_USE_SURFACE_WIN32: vkpt.h then exposes RgWin32SurfaceCreateInfo so the
    # host can pass the SDL window's HWND/HINSTANCE to the RT renderer.
    # NOMINMAX: vkpt.h includes windows.h (host TUs must not get min/max macros).
    # WIN32_LEAN_AND_MEAN: vkpt.h pulls windows.h into every host TU; without it
    # windows.h would include winsock.h and clash with net_*.c's winsock2.h.
    target_compile_definitions(vkquake PRIVATE WIN32 _WINDOWS RG_USE_SURFACE_WIN32 NOMINMAX WIN32_LEAN_AND_MEAN)
elseif(VKQUAKE_PLATFORM_LINUX)
    target_compile_definitions(vkquake PRIVATE LINUX)
endif()

# ============================================================================
# Link libraries
# ============================================================================
target_link_libraries(vkquake PRIVATE Vulkan::Vulkan)
target_link_libraries(vkquake PRIVATE vkpt)

if(VKQUAKE_PLATFORM_WINDOWS)
    target_link_libraries(vkquake PRIVATE SDL3::SDL3)
else()
    target_link_libraries(vkquake PRIVATE SDL3::SDL3)
endif()

if(VKQUAKE_PLATFORM_WINDOWS)
    target_link_libraries(vkquake PRIVATE ws2_32 winmm dbghelp ole32 uuid)
elseif(VKQUAKE_PLATFORM_LINUX)
    target_link_libraries(vkquake PRIVATE m dl pthread)
elseif(VKQUAKE_PLATFORM_MACOS)
    target_link_libraries(vkquake PRIVATE m dl pthread)
endif()

# Codec libs
if(VKQUAKE_PLATFORM_WINDOWS)
    if(USE_CODEC_MP3)
        target_link_libraries(vkquake PRIVATE codec::mad)
    endif()
    if(USE_CODEC_VORBIS)
        target_link_libraries(vkquake PRIVATE codec::vorbisfile codec::vorbis codec::ogg)
    endif()
    if(USE_CODEC_FLAC)
        target_link_libraries(vkquake PRIVATE codec::FLAC)
    endif()
    if(USE_CODEC_OPUS)
        target_link_libraries(vkquake PRIVATE codec::opusfile codec::opus)
    endif()
endif()

# ============================================================================
# Post-build: copy DLLs next to vkquake.exe
# ============================================================================
if(VKQUAKE_PLATFORM_WINDOWS)
    set(DLL_TARGETS
        "${SDL3_ROOT}/lib64/SDL3.dll"
        "${CODECS_LIB_DIR}/libmad-0.dll"
        "${CODECS_LIB_DIR}/libvorbis-0.dll"
        "${CODECS_LIB_DIR}/libvorbisfile-3.dll"
        "${CODECS_LIB_DIR}/libogg-0.dll"
        "${CODECS_LIB_DIR}/libFLAC-8.dll"
        "${CODECS_LIB_DIR}/libopus-0.dll"
        "${CODECS_LIB_DIR}/libopusfile-0.dll"
    )
    foreach(dll ${DLL_TARGETS})
        if(EXISTS "${dll}")
            add_custom_command(TARGET vkquake POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dll}" "$<TARGET_FILE_DIR:vkquake>"
                COMMENT "Copying ${dll}")
        endif()
    endforeach()

    # prebuilt AMD-signed FidelityFX API DLL, used by vkpt's FSR 2/3.1
    add_custom_command(TARGET vkquake POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/subprojects/vkpt/Source/FidelityFX_SDK_1.1.4/src/PrebuiltSignedDLL/amd_fidelityfx_vk.dll"
            "$<TARGET_FILE_DIR:vkquake>")
endif()

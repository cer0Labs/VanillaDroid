# GLSL -> SPIR-V with glslc, embedded into the binary as uint32 arrays (no asset loading needed).
#
# glslc ships with the Android NDK (shader-tools/<host>/glslc), so no Vulkan SDK is required.
# Search order: the NDK used for this build, $VULKAN_SDK, then the newest NDK in the local SDK.

# Phase 22 (gui): only the game has shaders (VD_BUILD_GAME OFF builds the tools alone, where glslc may not exist).
if(NOT VD_BUILD_GAME)
    return()
endif()

set(_vd_glslc_hints "")
if(ANDROID_NDK)
    file(GLOB _vd_ndk_shader_tools "${ANDROID_NDK}/shader-tools/*")
    list(APPEND _vd_glslc_hints ${_vd_ndk_shader_tools})
endif()
if(DEFINED ENV{VULKAN_SDK})
    list(APPEND _vd_glslc_hints "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")
endif()
if(DEFINED ENV{LOCALAPPDATA})
    file(TO_CMAKE_PATH "$ENV{LOCALAPPDATA}" _vd_local_appdata)
    file(GLOB _vd_sdk_ndks "${_vd_local_appdata}/Android/Sdk/ndk/*")
    list(SORT _vd_sdk_ndks COMPARE NATURAL ORDER DESCENDING)
    foreach(_vd_ndk IN LISTS _vd_sdk_ndks)
        list(APPEND _vd_glslc_hints "${_vd_ndk}/shader-tools/windows-x86_64")
    endforeach()
endif()

find_program(VD_GLSLC glslc HINTS ${_vd_glslc_hints} REQUIRED)
message(STATUS "glslc: ${VD_GLSLC}")

# vd_add_shaders(<target> SOURCES a.vert b.frag ...)
# Each shader produces <name>.spv.inc (comma-separated SPIR-V words) in the target's build dir:
#   constexpr uint32_t kSpv[] = {
#   #include "triangle.vert.spv.inc"
#   };
# Shaders may `#include "x.glsl"` (GL_GOOGLE_include_directive) from shaders/; any change to a .glsl
# include rebuilds every shader.
function(vd_add_shaders target)
    cmake_parse_arguments(ARG "" "" "SOURCES" ${ARGN})
    set(out_dir "${CMAKE_CURRENT_BINARY_DIR}/shaders")
    set(outputs "")
    file(GLOB includes "${PROJECT_SOURCE_DIR}/shaders/*.glsl")
    foreach(src IN LISTS ARG_SOURCES)
        get_filename_component(name "${src}" NAME)
        set(out "${out_dir}/${name}.spv.inc")
        add_custom_command(
            OUTPUT "${out}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${out_dir}"
            COMMAND "${VD_GLSLC}" --target-env=vulkan1.1 -O -Werror -mfmt=num -I "${PROJECT_SOURCE_DIR}/shaders"
                    -o "${out}" "${src}"
            DEPENDS "${src}" ${includes}
            COMMENT "glslc ${name}"
            VERBATIM)
        list(APPEND outputs "${out}")
    endforeach()
    target_sources(${target} PRIVATE ${outputs})
    target_include_directories(${target} PRIVATE "${out_dir}")
endfunction()

# Common compile options for every first-party target.
function(vd_set_target_options target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus /MP)
        target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
        if(VD_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        # -Wno-missing-field-initializers: the Vulkan idiom `VkXxxInfo info{VK_STRUCTURE_TYPE_...};`
        # intentionally zero-initializes every other field.
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wno-unused-parameter
                                                 -Wno-missing-field-initializers)
        if(VD_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()

    if(ANDROID)
        # Engine rule: no exceptions, no RTTI (errors go through vd::Result).
        target_compile_options(${target} PRIVATE
            -fno-exceptions -fno-rtti
            -fvisibility=hidden -fvisibility-inlines-hidden
            -ffunction-sections -fdata-sections)
    endif()
endfunction()

# Phase 22 (polish): a published build (VD_DIST) carries nothing of the machine that built it. __FILE__ (VD_VERIFY, the
# checks of third-party code: StormLib's libtomcrypt, SDL's asserts...) and debug information name the source, build
# and dependency folders and the builder's home folder, which would reveal the PC's account name: every
# target created after this call (the root CMakeLists.txt calls it before the dependencies) maps them away. Clang and
# GCC: -ffile-prefix-map (the folder becomes ".", a dependency its name); MSVC: /d1trimfile, its switch that cuts a
# prefix from __FILE__ (case-insensitive, separators as written: both spellings are given).
function(vd_map_build_paths)
    set(roots "${CMAKE_SOURCE_DIR}=." "${CMAKE_BINARY_DIR}=build")
    if(FETCHCONTENT_BASE_DIR)
        list(APPEND roots "${FETCHCONTENT_BASE_DIR}=deps")
    endif()
    # The sources of dependencies given by hand (a disconnected build, Gradle's -PvdDepsDir).
    get_cmake_property(cached CACHE_VARIABLES)
    foreach(variable IN LISTS cached)
        if(variable MATCHES "^FETCHCONTENT_SOURCE_DIR_" AND NOT "${${variable}}" STREQUAL "")
            string(REGEX REPLACE "^FETCHCONTENT_SOURCE_DIR_" "" name "${variable}")
            string(TOLOWER "${name}" name)
            list(APPEND roots "${${variable}}=${name}")
        endif()
    endforeach()
    if(ANDROID_NDK)
        list(APPEND roots "${ANDROID_NDK}=ndk")
    endif()
    foreach(home IN ITEMS "$ENV{USERPROFILE}" "$ENV{HOME}")
        if(home)
            list(APPEND roots "${home}=~")
        endif()
    endforeach()
    # Shortest first: a folder inside another (the build in the sources, the NDK in the home) comes after it, so
    # its own name wins also where the last matching map does (clang's debug information).
    set(ordered "")
    foreach(entry IN LISTS roots)
        string(FIND "${entry}" "=" equals REVERSE)
        string(SUBSTRING "${entry}" 0 ${equals} root)
        file(TO_CMAKE_PATH "${root}" root)
        string(LENGTH "${root}" length)
        math(EXPR length "10000 + ${length}") # same width: sorted as text
        math(EXPR after "${equals} + 1")
        string(SUBSTRING "${entry}" ${after} -1 replacement)
        list(APPEND ordered "${length}|${root}=${replacement}")
    endforeach()
    list(SORT ordered)
    foreach(entry IN LISTS ordered)
        string(SUBSTRING "${entry}" 6 -1 entry) # without "1NNNN|"
        string(FIND "${entry}" "=" equals REVERSE)
        string(SUBSTRING "${entry}" 0 ${equals} root)
        math(EXPR after "${equals} + 1")
        string(SUBSTRING "${entry}" ${after} -1 replacement)
        if(MSVC)
            file(TO_NATIVE_PATH "${root}" native)
            # Without a trailing separator: "\x\y.cpp" remains, and no backslash closes a quoted argument.
            add_compile_options("/d1trimfile:${native}" "/d1trimfile:${root}")
        else()
            add_compile_options("-ffile-prefix-map=${root}=${replacement}")
        endif()
    endforeach()
endfunction()

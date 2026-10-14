# Third-party dependencies, pinned to exact tags.
#
# Rule: a dependency is added in the phase that needs it, never "just in case".
# Each tag below names a release of its upstream repository.
#
#   Phase | Library              | Tag                   | Use
#   ------+----------------------+-----------------------+------------------------------------------
#   0     | Vulkan-Headers       | vulkan-sdk-1.4.363.0  | Vulkan headers (desktop + Android)
#   0     | volk                 | vulkan-sdk-1.4.363.0  | Vulkan meta-loader (direct device dispatch)
#   0     | VulkanMemoryAllocator| v3.4.0                | GPU memory
#   0     | Tracy                | v0.14.1               | CPU profiler (remote over adb forward)
#   0     | Dear ImGui           | v1.92.9b              | Debug overlay (dev builds only)
#   1     | StormLib             | v9.40                 | MPQ reading (baker only)
#   1     | astc-encoder         | 5.7.0                 | BLP -> ASTC (baker only)
#   2     | LZ4                  | v1.10.0               | Pack compression (terrain tiles)
#   2     | glm                  | 1.0.3                 | Math
#   4     | Mbed TLS             | v3.6.7 (LTS)          | SHA-1 + bignum for SRP6
#   4     | zlib                 | v1.3.2                | Desktop only; Android uses the system libz
#   7     | stb_truetype         | v1.26 (inside imgui)  | Baker only: SDF glyphs of ui.vdpk (no FreeType: ADR-0002)
#   9     | miniaudio            | 0.11.25               | Audio (AAudio backend on Android)
#   22    | qrcodegen (nayuki)   | v1.8.0                | VanillaDroid Setup only: the pairing QR code (MIT)
#   22    | SDL3                 | release-3.4.18        | VanillaDroid Setup's window, Windows + macOS only (zlib)
#
# Android AAR (Gradle, apps/android/app/build.gradle.kts):
#   androidx.games:games-activity 4.4.2 (phase 0), games-frame-pacing 2.1.3 (phase 0),
#   games-controller 2.0.2 (phase 7)

include(FetchContent)

# --- Phase 0: Vulkan ---------------------------------------------------------------------------
# Header-only / single-file libraries: SOURCE_SUBDIR points to a folder without CMakeLists.txt so
# FetchContent only downloads them; engine/CMakeLists.txt builds them into the vd_vulkan target.
FetchContent_Declare(vulkan_headers
    URL https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/vulkan-sdk-1.4.363.0.tar.gz
    URL_HASH SHA256=4a078be12bef21cfebc09d878b77a63cff9d68f899254a0b00d0e37ef73e7f7e
    SOURCE_SUBDIR _download_only)
FetchContent_Declare(volk
    URL https://github.com/zeux/volk/archive/refs/tags/vulkan-sdk-1.4.363.0.tar.gz
    URL_HASH SHA256=1547d8d74395d4048fb3f4a6313da56db626e89b55db238d0d7e8944c8a645f3
    SOURCE_SUBDIR _download_only)
FetchContent_Declare(vma
    URL https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/archive/refs/tags/v3.4.0.tar.gz
    URL_HASH SHA256=822aa850c6ce77346ae96a8a1d351d52e77e85929f35363849a0a4e638e0a2a1
    SOURCE_SUBDIR _download_only)
FetchContent_Declare(imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9b.tar.gz
    URL_HASH SHA256=21d8a0a565e85dce943e375db00812c2f3f0ab21f3f0f7964e364a63422d7f99
    SOURCE_SUBDIR _download_only)
FetchContent_MakeAvailable(vulkan_headers volk vma imgui)

# --- Phase 0: Tracy (profile builds only) ------------------------------------------------------
# Client and GUI versions must match: the GUI lives in .tools/tracy-0.14.1 (scripts/tracy.ps1).
if(VD_ENABLE_TRACY)
    set(TRACY_ENABLE ON CACHE BOOL "" FORCE)
    set(TRACY_ON_DEMAND ON CACHE BOOL "" FORCE) # near-zero cost until the profiler connects
    set(TRACY_STATIC ON CACHE BOOL "" FORCE)
    if(ANDROID)
        # Keep Android's own crash reports (tombstones) intact.
        set(TRACY_NO_CRASH_HANDLER ON CACHE BOOL "" FORCE)
    endif()
    FetchContent_Declare(tracy
        URL https://github.com/wolfpld/tracy/archive/refs/tags/v0.14.1.tar.gz
        URL_HASH SHA256=bf4af567e9c7524d07f3caa745fad02fb33bd5694f11910750382d1efbb251c1)
    FetchContent_MakeAvailable(tracy)
endif()

# --- Phase 2: LZ4 (pack compression) and glm (math) --------------------------------------------
# LZ4 is built from its two sources into vd_lz4 (engine/CMakeLists.txt): the runtime only calls the
# decompressor; lz4hc (compressor) is used by the baker and dropped from Android by --gc-sections.
FetchContent_Declare(lz4
    URL https://github.com/lz4/lz4/archive/refs/tags/v1.10.0.tar.gz
    URL_HASH SHA256=537512904744b35e232912055ccf8ec66d768639ff3abe5788d90d792ec5f48b
    SOURCE_SUBDIR _download_only)
FetchContent_Declare(glm
    URL https://github.com/g-truc/glm/archive/refs/tags/1.0.3.tar.gz
    URL_HASH SHA256=6775e47231a446fd086d660ecc18bcd076531cfedd912fbd66e576b118607001
    SOURCE_SUBDIR _download_only)
FetchContent_MakeAvailable(lz4 glm)

# --- Phase 4: Mbed TLS (SRP6: bignum + SHA-1) and zlib (compressed world packets) -----------------
# Release tarball (the git archive lacks the framework submodule). Only bignum/SHA-1 sources are built,
# with cmake/vd_mbedtls_config.h, into vd_mbedtls (engine/CMakeLists.txt).
FetchContent_Declare(mbedtls
    URL https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2
    URL_HASH SHA256=a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6
    SOURCE_SUBDIR _download_only)
FetchContent_MakeAvailable(mbedtls)
if(NOT ANDROID)
    FetchContent_Declare(zlib
        URL https://github.com/madler/zlib/releases/download/v1.3.2/zlib-1.3.2.tar.gz
        URL_HASH SHA256=bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16
        SOURCE_SUBDIR _download_only)
    FetchContent_MakeAvailable(zlib)
endif()

# --- Phase 9: miniaudio (audio device; its MP3 frame decoder for the music) ----------------------
# Single-file library: engine/CMakeLists.txt builds miniaudio.c into vd_miniaudio with only the backends and
# decoders the game uses (the tarball's own CMakeLists.txt is skipped through SOURCE_SUBDIR).
FetchContent_Declare(miniaudio
    URL https://github.com/mackron/miniaudio/archive/refs/tags/0.11.25.tar.gz
    URL_HASH SHA256=b900edcffe979816e2560a0580b9b1216d674b4f17fbadeca8f777a7f8ab0274
    SOURCE_SUBDIR _download_only)
FetchContent_MakeAvailable(miniaudio)

# --- Phase 1: baker-only (PC; phase 22: also the baker for Android) ---------------------------
if(VD_BUILD_TOOLS OR VD_ANDROID_BAKER)
    set(STORM_USE_BUNDLED_LIBRARIES ON CACHE BOOL "" FORCE) # its own zlib/bzip2, no system lookup
    set(STORM_SKIP_INSTALL ON CACHE BOOL "" FORCE)
    set(STORM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(stormlib
        URL https://github.com/ladislav-zezula/StormLib/archive/refs/tags/v9.40.tar.gz
        URL_HASH SHA256=f80b08bae168888702a4251312d6c2279f487673df611c2b5ef4f395e810c3b1)
    FetchContent_MakeAvailable(stormlib)

    # ASTC encoder as a library (target ${VD_ASTCENC_TARGET}). The dev PC (i7-6700K) has AVX2; phase 22: arm64
    # (the baker on Android, Apple silicon) has NEON, and Intel Macs take SSE4.1 (the AVX2 build is x86_64h there,
    # which an x86_64 binary cannot link). Every ISA encodes the same blocks (checked: AVX2, SSE4.1, SSE2 and none on
    # the PC), but for the C library's sines and cosines: see below.
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
        set(ASTCENC_ISA_NEON ON CACHE BOOL "" FORCE)
        set(VD_ASTCENC_TARGET astcenc-neon-static)
    elseif(APPLE)
        set(ASTCENC_ISA_SSE41 ON CACHE BOOL "" FORCE)
        set(VD_ASTCENC_TARGET astcenc-sse4.1-static)
    else()
        set(ASTCENC_ISA_AVX2 ON CACHE BOOL "" FORCE)
        set(VD_ASTCENC_TARGET astcenc-avx2-static)
    endif()
    set(ASTCENC_UNIVERSAL_BUILD OFF CACHE BOOL "" FORCE)
    set(ASTCENC_CLI OFF CACHE BOOL "" FORCE)
    set(ASTCENC_SHAREDLIB OFF CACHE BOOL "" FORCE)
    set(ASTCENC_UNITTEST OFF CACHE BOOL "" FORCE)
    set(ASTCENC_WERROR OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(astcenc
        URL https://github.com/ARM-software/astc-encoder/archive/refs/tags/5.7.0.tar.gz
        URL_HASH SHA256=7c1b28ece59c9c2737e297123a9910c1c548676c7aed87a09d5734e2fa7bdaf0)
    FetchContent_MakeAvailable(astcenc)
    # Phase 22 (ADR-0005 §3, the same packs everywhere): astcenc_weight_align.cpp and astcenc_entry.cpp built through
    # cmake/astcenc/, with sinf, cosf and logf alike on every platform (a phone's bionic made 19 of 58,502 textures
    # differ by one block).
    get_target_property(_vd_astcenc_sources ${VD_ASTCENC_TARGET} SOURCES)
    list(REMOVE_ITEM _vd_astcenc_sources astcenc_weight_align.cpp astcenc_entry.cpp)
    list(APPEND _vd_astcenc_sources "${PROJECT_SOURCE_DIR}/cmake/astcenc/astcenc_weight_align_portable.cpp"
                                    "${PROJECT_SOURCE_DIR}/cmake/astcenc/astcenc_entry_portable.cpp")
    set_property(TARGET ${VD_ASTCENC_TARGET} PROPERTY SOURCES ${_vd_astcenc_sources})
    target_include_directories(${VD_ASTCENC_TARGET} PRIVATE "${PROJECT_SOURCE_DIR}/cmake/astcenc")

    # Phase 22 (setup): the QR code VanillaDroid Setup shows for pairing the phone. Only its C version
    # (c/qrcodegen.c: no allocations, no exceptions) is built, into vd_qrcodegen (tools/setup/CMakeLists.txt).
    FetchContent_Declare(qrcodegen
        URL https://github.com/nayuki/QR-Code-generator/archive/refs/tags/v1.8.0.tar.gz
        URL_HASH SHA256=2ec0a4d33d6f521c942eeaf473d42d5fe139abcfa57d2beffe10c5cf7d34ae60
        SOURCE_SUBDIR _download_only)
    FetchContent_MakeAvailable(qrcodegen)

    # Phase 22 (gui): VanillaDroid Setup's window (Dear ImGui over SDL3 and its 2D renderer: Direct3D 11 on Windows,
    # Metal on macOS; nothing to install). Static, with only a window, its renderer and the folder dialog: no audio,
    # joystick, haptics, camera, sensors, tray, GPU API, OpenGL or Vulkan. Target SDL3::SDL3-static.
    if(WIN32 OR APPLE)
        set(SDL_SHARED OFF CACHE BOOL "" FORCE)
        set(SDL_STATIC ON CACHE BOOL "" FORCE)
        set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
        set(SDL_TESTS OFF CACHE BOOL "" FORCE)
        set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
        set(SDL_UNINSTALL OFF CACHE BOOL "" FORCE)
        foreach(vd_sdl_off SDL_AUDIO SDL_GPU SDL_CAMERA SDL_JOYSTICK SDL_HAPTIC SDL_HIDAPI SDL_POWER SDL_SENSOR SDL_TRAY
                           SDL_OPENGL SDL_OPENGLES SDL_VULKAN SDL_RENDER_VULKAN SDL_RENDER_GPU SDL_RENDER_D3D
                           SDL_RENDER_D3D12 SDL_XINPUT SDL_WASAPI)
            set(${vd_sdl_off} OFF CACHE BOOL "" FORCE)
        endforeach()
        set(SDL_VIDEO ON CACHE BOOL "" FORCE)
        set(SDL_RENDER ON CACHE BOOL "" FORCE)
        set(SDL_DIALOG ON CACHE BOOL "" FORCE)
        FetchContent_Declare(sdl3
            URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz
            URL_HASH SHA256=9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3)
        FetchContent_MakeAvailable(sdl3)
    endif()
endif()

# --- Tests -------------------------------------------------------------------------------------
if(VD_BUILD_TESTS)
    set(DOCTEST_WITH_TESTS OFF CACHE BOOL "" FORCE)
    set(DOCTEST_NO_INSTALL ON CACHE BOOL "" FORCE)
    FetchContent_Declare(doctest
        GIT_REPOSITORY https://github.com/doctest/doctest.git
        GIT_TAG v2.5.3
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(doctest)
endif()

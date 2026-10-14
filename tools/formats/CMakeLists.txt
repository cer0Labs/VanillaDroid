# vd_formats: parsers for the ORIGINAL client formats (MPQ, DBC, BLP, later ADT/WMO/M2) and the
# baking code that turns them into packs. PC only: used by the baker and tests. The runtime never
# links this; it reads baked packs.
add_library(vd_formats STATIC
    src/bake/ActorBaker.cpp
    src/bake/AppPackBaker.cpp # phase 22 (apppack): app.vdpk
    src/bake/CinematicBaker.cpp
    src/bake/CollisionBaker.cpp
    src/bake/DoodadMotion.cpp
    src/bake/EffectBaker.cpp
    src/bake/GlueBaker.cpp # phase 18 (lead): the glue scenes
    src/bake/GrassBaker.cpp
    src/bake/LocaleBaker.cpp # phase 21: clients in other languages
    src/bake/ModelBaker.cpp
    src/bake/PackWriter.cpp
    src/bake/SkyBaker.cpp
    src/bake/SoundBaker.cpp
    src/bake/TerrainBaker.cpp
    src/bake/TextureBaker.cpp
    src/bake/TrackWriter.cpp
    src/bake/UiBaker.cpp
    src/bake/UiStrings.cpp # phase 22 (apppack): the clients' texts and ours, apart
    src/bake/WorldSoundBaker.cpp
    src/formats/Adt.cpp
    src/formats/AdtSound.cpp
    src/formats/Blp.cpp
    src/formats/Dbc.cpp
    src/formats/Dxt.cpp
    src/formats/LuaStrings.cpp
    src/formats/M2.cpp
    src/formats/MpqArchiveSet.cpp
    src/formats/PngWriter.cpp
    src/formats/Wav.cpp
    src/formats/Wmo.cpp)
target_include_directories(vd_formats PUBLIC src)
# stb_truetype (UiBaker): the copy inside Dear ImGui, pinned with it.
target_include_directories(vd_formats PRIVATE "${imgui_SOURCE_DIR}")
target_link_libraries(vd_formats PUBLIC vd_core vd_audio PRIVATE StormLib::storm ${VD_ASTCENC_TARGET})
vd_set_target_options(vd_formats)
# Phase 22: the same packs on every platform (bake/Portable.h): no fused multiply-add where clang would contract one
# (arm64: Android, Apple silicon), as MSVC never does on x64. PUBLIC: the baker's own code bakes too.
if(NOT MSVC)
    target_compile_options(vd_formats PUBLIC -ffp-contract=off)
endif()

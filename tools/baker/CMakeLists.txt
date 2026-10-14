# Asset baker: original client (MPQ) -> mobile-ready packs (VDPK). PC (phase 22: and Android, VD_ANDROID_BAKER).
add_executable(vd_baker
    ActorCommands.cpp
    BakerCommon.cpp
    BodyFxCommands.cpp # phase 14 (bodyfx)
    CinematicCommands.cpp
    CollisionCommands.cpp
    DistributionCommands.cpp # phase 22: recipes and the packs folder's manifest
    DoodadCommands.cpp
    EffectCommands.cpp
    GrassCommands.cpp
    LocaleCommands.cpp # phase 21: clients in other languages
    MacroIcons.cpp # phase 15 (options)
    UiModels.cpp # retouches (ui models)
    ${PROJECT_SOURCE_DIR}/engine/src/world/GrassPlacement.cpp # phase 14: the census runs WoW.exe's placement
    ModelCommands.cpp
    SoundCommands.cpp
    UiCommands.cpp
    WorldCommands.cpp
    WorldSoundCommands.cpp
    # phase 22 (wave 1) baker files here
    PhoneBakeCommands.cpp # phase 22 (phonebake): install, a step baked on the phone into packs/
    AppCommands.cpp # phase 22 (apppack): app.vdpk
    # phase 22 (setup): the process's code page is UTF-8, so the WoW folders vdsetup passes arrive intact
    $<$<BOOL:${WIN32}>:${PROJECT_SOURCE_DIR}/tools/setup/utf8.manifest>
    RunOptions.cpp # phase 22 (phonebaker): --jobs, --cache, what a run cost
    AssetDigest.cpp # phase 22 (phonebaker): digest-assets
    main.cpp)
target_link_libraries(vd_baker PRIVATE vd_formats vd_physics)
# Phase 21: the clients in other languages, '|'-separated in the definition (a ';' would split it into two).
string(REPLACE ";" "|" VD_WOW_LOCALE_DIRS_DEFINE "${VD_WOW_LOCALE_DIRS}")
if(ANDROID)
    # Phase 22 (phonebaker): on a phone, under /data/local/tmp, where scripts/android-baker.ps1 puts them (a missing
    # client in another language is skipped).
    target_compile_definitions(vd_baker PRIVATE VD_DEFAULT_WOW_DIR="/data/local/tmp/wow"
                                                VD_DEFAULT_WOW_LOCALE_DIRS="/data/local/tmp/wow-esES"
                                                VD_DEFAULT_LOCALE_DIR="/data/local/tmp/vd/locale"
                                                VD_DEFAULT_ART_DIR="/data/local/tmp/vd/art")
elseif(VD_DIST)
    # Phase 22 (polish): a published baker names no folder of the machine that built it (this PC's, the CI's private
    # repository): VanillaDroid Setup always passes --wow-dir, and the CI bakes app.vdpk with --locale-dir and
    # --art-dir.
    target_compile_definitions(vd_baker PRIVATE VD_DEFAULT_WOW_DIR="" VD_DEFAULT_WOW_LOCALE_DIRS=""
                                                VD_DEFAULT_LOCALE_DIR="" VD_DEFAULT_ART_DIR="")
else()
    target_compile_definitions(vd_baker PRIVATE VD_DEFAULT_WOW_DIR="${VD_WOW_DIR}"
                                                VD_DEFAULT_WOW_LOCALE_DIRS="${VD_WOW_LOCALE_DIRS_DEFINE}"
                                                VD_DEFAULT_LOCALE_DIR="${PROJECT_SOURCE_DIR}/locale"
                                                VD_DEFAULT_ART_DIR="${PROJECT_SOURCE_DIR}/art")
endif()
vd_set_target_options(vd_baker)

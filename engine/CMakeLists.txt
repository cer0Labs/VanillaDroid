# vd_lz4: LZ4 block format. The runtime decompresses pack assets; the baker compresses with lz4hc.
add_library(vd_lz4 STATIC
    "${lz4_SOURCE_DIR}/lib/lz4.c"
    "${lz4_SOURCE_DIR}/lib/lz4hc.c")
target_include_directories(vd_lz4 SYSTEM PUBLIC "${lz4_SOURCE_DIR}/lib")

# glm: header-only math. Vulkan conventions: depth in [0, 1], radians only.
add_library(vd_glm INTERFACE)
target_include_directories(vd_glm SYSTEM INTERFACE "${glm_SOURCE_DIR}")
target_compile_definitions(vd_glm INTERFACE GLM_FORCE_DEPTH_ZERO_TO_ONE GLM_FORCE_XYZW_ONLY GLM_ENABLE_EXPERIMENTAL)

# vd_core: platform-agnostic utilities. Also used by tools and tests.
add_library(vd_core STATIC
    src/core/Assert.cpp
    src/core/Config.cpp
    src/core/FrameArena.cpp
    src/core/Json.cpp # phase 22: manifests and release files
    src/core/Log.cpp
    src/core/Sha256.cpp # phase 22
    src/core/Text.cpp
    src/db/DbcView.cpp
    src/jobs/JobSystem.cpp
    src/pack/AsyncReader.cpp
    src/pack/PackDigest.cpp # phase 22: the packs' recipes
    src/pack/PackManifest.cpp # phase 22
    src/pack/PackReader.cpp
    # phase 22 (wave 1) vd_core files here
    # phase 22 (phonebake): the WoW copied to the phone, a step baked on it into packs/
    src/pack/PackInstall.cpp
    src/pack/WowArchives.cpp
    src/core/ReleaseSignature.cpp # phase 22 (release): the release manifest's signature
    src/pack/AppPack.cpp # phase 22 (apppack): app.vdpk from the APK
    src/pack/ReleaseManifest.cpp # phase 22 (updates): a release's manifest.json and what it changes
    src/platform/MappedFile.cpp
    src/platform/Thread.cpp)
target_include_directories(vd_core PUBLIC src)
target_link_libraries(vd_core PUBLIC vd_lz4 vd_glm PRIVATE vd_mbedtls)
vd_set_target_options(vd_core)
# Phase 22 (distribution): the version in a generated header (core/Version.h), so a new version only rebuilds what
# includes it, and release/packs.json (the recipes of the release's packs, pack/PackManifest.h) built in.
if(NOT VD_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
    message(FATAL_ERROR "VD_VERSION must be X.Y.Z, not '${VD_VERSION}'")
endif()
math(EXPR VD_VERSION_CODE "${CMAKE_MATCH_1} * 1000000 + ${CMAKE_MATCH_2} * 1000 + ${CMAKE_MATCH_3}")
configure_file("${PROJECT_SOURCE_DIR}/cmake/Version.h.in" "${CMAKE_BINARY_DIR}/generated/vd_version.gen.h" @ONLY)
file(READ "${PROJECT_SOURCE_DIR}/release/packs.json" VD_RELEASE_PACKS_JSON)
configure_file("${PROJECT_SOURCE_DIR}/cmake/ReleasePacks.h.in" "${CMAKE_BINARY_DIR}/generated/vd_release_packs.gen.h"
               @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/release/packs.json")
# Phase 22 (release): the release public key (release/release-key.pub.hex, the uncompressed P-256 point in hex) that
# core/ReleaseSignature checks the release manifest against, built in as bytes.
file(READ "${PROJECT_SOURCE_DIR}/release/release-key.pub.hex" VD_RELEASE_KEY_HEX)
string(STRIP "${VD_RELEASE_KEY_HEX}" VD_RELEASE_KEY_HEX)
string(LENGTH "${VD_RELEASE_KEY_HEX}" vd_release_key_length)
if(NOT VD_RELEASE_KEY_HEX MATCHES "^04[0-9a-f]+$" OR NOT vd_release_key_length EQUAL 130)
    message(FATAL_ERROR "release/release-key.pub.hex must be an uncompressed P-256 point (04 || X || Y, 130 "
                        "lowercase hex digits): scripts/release-key.ps1 writes it")
endif()
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," VD_RELEASE_KEY_BYTES "${VD_RELEASE_KEY_HEX}")
configure_file("${PROJECT_SOURCE_DIR}/cmake/ReleaseKey.h.in" "${CMAKE_BINARY_DIR}/generated/vd_release_key.gen.h" @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/release/release-key.pub.hex")
target_include_directories(vd_core PUBLIC "${CMAKE_BINARY_DIR}/generated")
if(VD_DIST)
    target_compile_definitions(vd_core PUBLIC VD_DIST=1)
endif()
if(ANDROID)
    target_link_libraries(vd_core PUBLIC log)
endif()
if(VD_ANDROID_BAKER)
    # Phase 22: the baker on a phone is a command-line program: its log goes to stderr, as on the PC.
    target_compile_definitions(vd_core PRIVATE VD_LOG_STDERR=1)
endif()
if(VD_ENABLE_TRACY)
    # Propagates TRACY_ENABLE: core/Profile.h turns VD_PROFILE_* into Tracy zones.
    target_link_libraries(vd_core PUBLIC Tracy::TracyClient)
endif()

# vd_mbedtls: the big-number and SHA-1 parts of Mbed TLS, configured by cmake/vd_mbedtls_config.h (SRP6).
add_library(vd_mbedtls STATIC
    "${mbedtls_SOURCE_DIR}/library/bignum.c"
    "${mbedtls_SOURCE_DIR}/library/bignum_core.c"
    "${mbedtls_SOURCE_DIR}/library/constant_time.c"
    "${mbedtls_SOURCE_DIR}/library/platform_util.c"
    "${mbedtls_SOURCE_DIR}/library/sha1.c"
    "${mbedtls_SOURCE_DIR}/library/sha256.c" # phase 22: packs and downloads
    # phase 22 (release): ECDSA P-256 verification of the release manifest (core/ReleaseSignature)
    "${mbedtls_SOURCE_DIR}/library/asn1parse.c"
    "${mbedtls_SOURCE_DIR}/library/asn1write.c"
    "${mbedtls_SOURCE_DIR}/library/ecdsa.c"
    "${mbedtls_SOURCE_DIR}/library/ecp.c"
    "${mbedtls_SOURCE_DIR}/library/ecp_curves.c")
target_include_directories(vd_mbedtls SYSTEM PUBLIC "${mbedtls_SOURCE_DIR}/include" "${PROJECT_SOURCE_DIR}/cmake")
target_include_directories(vd_mbedtls PRIVATE "${mbedtls_SOURCE_DIR}/library")
target_compile_definitions(vd_mbedtls PUBLIC MBEDTLS_CONFIG_FILE="vd_mbedtls_config.h") # phase 22 (release): renamed

# zlib: inflate for SMSG_COMPRESSED_UPDATE_OBJECT. Android has it in the system (libz).
if(ANDROID)
    add_library(vd_zlib INTERFACE)
    target_link_libraries(vd_zlib INTERFACE z)
else()
    add_library(vd_zlib STATIC
        "${zlib_SOURCE_DIR}/adler32.c"
        "${zlib_SOURCE_DIR}/crc32.c"
        "${zlib_SOURCE_DIR}/deflate.c"
        "${zlib_SOURCE_DIR}/inffast.c"
        "${zlib_SOURCE_DIR}/inflate.c"
        "${zlib_SOURCE_DIR}/inftrees.c"
        "${zlib_SOURCE_DIR}/trees.c"
        "${zlib_SOURCE_DIR}/uncompr.c"
        "${zlib_SOURCE_DIR}/zutil.c")
    target_include_directories(vd_zlib SYSTEM PUBLIC "${zlib_SOURCE_DIR}")
    if(MSVC)
        target_compile_definitions(vd_zlib PRIVATE _CRT_SECURE_NO_DEPRECATE _CRT_NONSTDC_NO_DEPRECATE)
    endif()
endif()

# vd_net: sockets, the realmd/world protocol (SRP6, header cipher), generated opcodes and update fields,
# and the network thread. No Vulkan, no game logic: usable by tests.
add_library(vd_net STATIC
    src/net/CombatMessages.cpp
    src/net/Crypto.cpp
    src/net/GameMessages.cpp
    src/net/ItemMessages.cpp
    src/net/Messages.cpp
    src/net/PetMessages.cpp # phase 15 (pets)
    src/net/MinimapMessages.cpp
    src/net/NetClient.cpp
    src/net/ResponseCodes.gen.cpp
    src/net/WorldConnection.cpp
    src/net/RealmClient.cpp
    src/net/SocialMessages.cpp
    src/net/PlayerCommandMessages.cpp
    src/net/MeetingStoneMessages.cpp # phase 16 (alerts)
    src/net/BattlegroundMessages.cpp # phase 17 (lead)
    # phase 17 (queue)
    src/net/BattlefieldListMessages.cpp
    src/net/PvpLogMessages.cpp # phase 17 (scores)
    src/net/PvpMessages.cpp # phase 17 (honor)
    # phase 18 (glue)
    src/net/RealmListMessages.cpp
    src/net/CharRenameMessages.cpp
    # phase 18 (create)
    # phase 18 (p3)
    src/net/AccountDataMessages.cpp
    # phase 18 (extras)
    src/net/ExtrasMessages.cpp
    src/net/CharacterMessages.cpp # phase 15 (character)
    src/net/GuildMessages.cpp # phase 15 (guild)
    src/net/HelpMessages.cpp # phase 15 (help)
    src/net/HttpClient.cpp # phase 22 (pairing): the downloads from VanillaDroid Setup
    src/net/Socket.cpp
    src/net/Opcodes.gen.cpp
    src/net/UpdateFields.gen.cpp
    src/net/TravelMessages.cpp
    src/net/ExchangeMessages.cpp
    src/net/MirrorTimerMessages.cpp
    src/net/PopupMessages.cpp
    src/net/QuestMessages.cpp
    src/net/WeatherMessages.cpp
    src/net/MiscMessages.cpp
    src/net/SpellMessages.cpp
    src/net/ControlMessages.cpp
    src/net/SplineMessages.cpp # phase 18 (motion)
    src/net/SpellLogMessages.cpp)
target_link_libraries(vd_net PUBLIC vd_core PRIVATE vd_mbedtls vd_zlib)
if(WIN32)
    target_link_libraries(vd_net PRIVATE ws2_32)
endif()
vd_set_target_options(vd_net)

# vd_game: the game state the server describes (entities and their update fields; phase 7: combat, spells, auras,
# NPC dialogs, quests, loot, chat, death). No Vulkan: usable by tests.
add_library(vd_game STATIC
    src/game/Abilities.cpp
    src/game/AbilityData.cpp
    src/game/ActionBar.cpp
    src/game/Appearance.cpp
    src/game/Auras.cpp
    src/game/BodyData.cpp # phase 14 (bodyfx)
    src/game/CharCreate.cpp
    src/game/CharacterNames.cpp
    src/game/Chat.cpp
    src/game/ChatCommands.cpp
    src/game/Combat.cpp
    src/game/CombatAnimation.cpp
    src/game/CombatText.cpp
    src/game/Entities.cpp
    src/game/UnitMotion.cpp # phase 18 (motion)
    src/game/GameData.cpp
    src/game/GameObjectAnim.cpp
    src/game/Gameplay.cpp
    src/game/Inventory.cpp
    src/game/ItemActions.cpp
    src/game/ItemData.cpp
    src/game/ItemTooltip.cpp
    src/game/Loot.cpp
    src/game/Minimap.cpp
    src/game/MinimapData.cpp
    src/game/NpcDialog.cpp
    src/game/Progress.cpp
    src/game/Queries.cpp
    src/game/QuestData.cpp
    src/game/QuestJournal.cpp
    src/game/Quests.cpp
    src/game/Reactions.cpp
    src/game/SpellBookTabs.cpp
    src/game/SpellText.cpp
    src/game/Social.cpp
    src/game/PlayerCommands.cpp
    # Phase 16 (raids): the raid tab's rules, the pullouts' aura filters, ready checks and meeting stones.
    src/game/RaidRoster.cpp
    src/game/RaidAuras.cpp
    src/game/MeetingStone.cpp
    src/game/Battlegrounds.cpp # phase 17 (lead)
    src/game/WorldStates.cpp
    # phase 17 (queue)
    src/game/BattlefieldQueue.cpp
    src/game/BattlefieldLines.cpp
    src/game/WorldStateUi.cpp # phase 17 (scores)
    src/game/BattlefieldScore.cpp
    src/game/AreaSpiritHealer.cpp # phase 17 (honor)
    src/game/GameplayPvp.cpp
    src/game/Honor.cpp
    src/game/PvpLines.cpp
    # phase 18 (glue)
    # phase 18 (create)
    # phase 18 (p3)
    src/game/ChatWindows.cpp
    src/game/Tutorials.cpp
    src/game/AccountData.cpp
    src/game/ListSort.cpp
    # phase 18 (extras)
    src/game/Languages.cpp
    src/game/GameplayExtras.cpp
    src/game/RaidAlertLines.cpp
    src/game/ReadyCheck.cpp
    # Phase 15 (character): the character sheet's numbers, reputation and skill lists, inspect honor.
    src/game/CharacterData.cpp
    src/game/CharacterSheet.cpp
    src/game/FactionList.cpp
    src/game/PaperDoll.cpp
    src/game/SkillList.cpp
    src/game/GuildControl.cpp # phase 15 (guild)
    src/game/GuildEmblem.cpp
    src/game/Petitions.cpp
    src/game/TabardDesigner.cpp
    src/game/HelpData.cpp # phase 15 (help)
    src/game/HelpDesk.cpp
    src/game/UnitPopup.cpp
    src/game/SocialData.cpp
    src/game/WhoQuery.cpp # retouches (friends frame): SendWho's reading of a who query
    src/game/SocialText.cpp
    src/game/Spells.cpp
    src/game/SpellMods.cpp
    src/game/SpellTargeting.cpp
    src/game/UnitFade.cpp
    src/game/UnitInfo.cpp
    src/game/UnitText.cpp
    src/game/WeaponEnchants.cpp
    src/game/AreaData.cpp
    src/game/TaxiData.cpp
    src/game/Transports.cpp
    src/game/Travel.cpp
    src/game/WorldMapData.cpp
    src/game/Auction.cpp
    src/game/Exchange.cpp
    src/game/ExchangeData.cpp
    src/game/LootRolls.cpp
    src/game/Mail.cpp
    src/game/Trade.cpp
    src/game/SpellVisualData.cpp
    src/game/SpellKitFx.cpp # phase 14 (spellfx)
    src/game/MirrorTimers.cpp
    src/game/Notices.cpp
    src/game/ChatLinks.cpp
    src/game/BindConfirm.cpp
    src/game/Logout.cpp
    src/game/Summon.cpp
    src/game/TradeSkillData.cpp # phase 13 (professions)
    src/game/TradeSkills.cpp
    src/game/CombatFeedback.cpp # phase 15 (pets)
    src/game/PetData.cpp
    src/game/PetStable.cpp
    src/game/Weather.cpp
    src/game/ItemCompare.cpp # phase 15 (items)
    src/game/MasterLoot.cpp # phase 15 (items)
    src/game/TryOn.cpp # phase 15 (items)
    src/game/MoverControl.cpp
    src/game/Macros.cpp # phase 15 (options)
    src/game/TouchLayout.cpp # phase 20 (docs/MOBILE_CONTROLS.md)
    src/game/TouchContext.cpp # phase 20
    src/game/SpellLines.cpp # phase 20
    src/game/ClassPresets.cpp # phase 20 (classes)
    src/game/TouchPet.cpp # phase 20 (pets): the pet button, another unit controlled, no control, death
    # phase 20 (smart)
    src/game/Consumables.cpp
    src/game/Mounts.cpp
    src/game/ReactiveSpells.cpp
    # phase 20 (edit)
    src/game/TouchLayoutEdit.cpp # phase 20 (edit)
    # phase 20 (classes)
    src/game/ClassPresetData.cpp # phase 20 (classes): the presets' tables
    # phase 20 (assists)
    src/game/TouchAssistRules.cpp # phase 20 (assists): the rules of the touch cluster's assists
    src/game/GameplayOptions.cpp)
# Phase 9: the spell visual tables name their models by the effect hash of vd_fx.
target_link_libraries(vd_game PUBLIC vd_net vd_glm vd_fx)
vd_set_target_options(vd_game)

# vd_anim: skeletons of baked actor models (sampling, blending, sequence choice). No Vulkan: usable by tests.
add_library(vd_anim STATIC
    src/anim/Animator.cpp
    src/anim/Skeleton.cpp
    src/anim/TrackTable.cpp)
target_link_libraries(vd_anim PUBLIC vd_core vd_glm)
vd_set_target_options(vd_anim)

# vd_fx: effects of the M2 models (phase 9): particle emitters, ribbons, texture animations. Simulation only (the
# renderer draws what it writes): no Vulkan, usable by tests.
add_library(vd_fx STATIC
    src/fx/EffectSet.cpp
    src/fx/ParticleSystem.cpp)
target_link_libraries(vd_fx PUBLIC vd_core vd_anim vd_glm)
vd_set_target_options(vd_fx)

# vd_physics: baked collision (streamed tiles, queries) and the player's character controller. No Vulkan: usable by
# tests and tools.
add_library(vd_physics STATIC
    src/physics/CharacterController.cpp
    src/physics/Collision.cpp
    src/physics/CollisionWorld.cpp)
target_link_libraries(vd_physics PUBLIC vd_core vd_glm)
vd_set_target_options(vd_physics)

# vd_ui: the touch UI (phase 7): ui.vdpk access, draw lists (quads for the renderer), text, touch input. No Vulkan:
# usable by tests.
add_library(vd_ui STATIC
    src/ui/Context.cpp
    src/ui/DrawList.cpp
    src/ui/TextInput.cpp
    src/ui/UiAssets.cpp
    src/ui/Widgets.cpp
    src/ui/MessageFrame.cpp
    src/ui/ChatDock.cpp
    src/ui/UiModels.cpp) # retouches (ui models)
target_link_libraries(vd_ui PUBLIC vd_core vd_glm)
vd_set_target_options(vd_ui)

# vd_miniaudio: the audio device (phase 9) with only the backends the game uses (Android: AAudio, OpenSL ES as
# fallback; desktop: WASAPI) plus the null backend (silent runs, tests), and its MP3 frame decoder for the music.
# The definitions are PUBLIC: they shape miniaudio's structs, so every includer must see the same ones.
# MiniaudioImpl.c is miniaudio.c plus the C entry points of its MP3 frame decoder (audio/Mp3Frame.h).
add_library(vd_miniaudio STATIC src/audio/MiniaudioImpl.c)
target_include_directories(vd_miniaudio SYSTEM PUBLIC "${miniaudio_SOURCE_DIR}")
target_include_directories(vd_miniaudio PRIVATE src)
target_compile_definitions(vd_miniaudio PUBLIC
    MA_ENABLE_ONLY_SPECIFIC_BACKENDS MA_ENABLE_NULL
    MA_NO_ENCODING MA_NO_WAV MA_NO_FLAC MA_NO_GENERATION MA_NO_RESOURCE_MANAGER MA_NO_NODE_GRAPH MA_NO_ENGINE)
if(ANDROID)
    target_compile_definitions(vd_miniaudio PUBLIC MA_ENABLE_AAUDIO MA_ENABLE_OPENSL)
    target_link_libraries(vd_miniaudio PUBLIC dl)
elseif(WIN32)
    target_compile_definitions(vd_miniaudio PUBLIC MA_ENABLE_WASAPI)
endif()
if(MSVC)
    target_compile_options(vd_miniaudio PRIVATE /W0)
else()
    target_compile_options(vd_miniaudio PRIVATE -w)
endif()
if(ANDROID)
    # Hidden like the engine's own code: exported, every miniaudio function would survive --gc-sections.
    target_compile_options(vd_miniaudio PRIVATE -fvisibility=hidden -ffunction-sections -fdata-sections)
endif()

# vd_audio: the sound pack, the mixer, the device and the voices (phase 9). No Vulkan, no game state: usable by
# tests and by the baker (its ADPCM codec).
add_library(vd_audio STATIC
    src/audio/Adpcm.cpp
    src/audio/AudioDevice.cpp
    src/audio/AudioSystem.cpp
    src/audio/GroundSounds.cpp
    src/audio/Mixer.cpp
    src/audio/ModelSoundRules.cpp
    src/audio/SoundAsset.cpp
    src/audio/SoundData.cpp
    src/audio/SoundRules.cpp
    src/audio/WorldSoundTables.cpp)
target_link_libraries(vd_audio PUBLIC vd_core vd_glm PRIVATE vd_miniaudio)
vd_set_target_options(vd_audio)

# Phase 22 (gui): the tools alone (VD_BUILD_GAME OFF: VanillaDroid Setup and the baker on a Mac) need nothing below.
if(NOT VD_BUILD_GAME)
    return()
endif()

# vd_vulkan: volk (meta-loader) + VMA implementation, built with third-party warning levels.
# VK_USE_PLATFORM_* is safe to define globally: volk forward-declares the Win32 types instead of
# pulling <windows.h> into every translation unit.
add_library(vd_vulkan STATIC
    "${volk_SOURCE_DIR}/volk.c"
    src/render/vk/VmaImpl.cpp)
target_include_directories(vd_vulkan SYSTEM PUBLIC
    "${vulkan_headers_SOURCE_DIR}/include"
    "${volk_SOURCE_DIR}"
    "${vma_SOURCE_DIR}/include")
target_compile_definitions(vd_vulkan PUBLIC VK_NO_PROTOTYPES)
if(ANDROID)
    target_compile_definitions(vd_vulkan PUBLIC VK_USE_PLATFORM_ANDROID_KHR)
    target_link_libraries(vd_vulkan PUBLIC dl)
elseif(WIN32)
    target_compile_definitions(vd_vulkan PUBLIC VK_USE_PLATFORM_WIN32_KHR)
endif()

# vd_imgui: Dear ImGui + its Vulkan backend (through volk). Debug overlay only, never game UI.
add_library(vd_imgui STATIC
    "${imgui_SOURCE_DIR}/imgui.cpp"
    "${imgui_SOURCE_DIR}/imgui_demo.cpp"
    "${imgui_SOURCE_DIR}/imgui_draw.cpp"
    "${imgui_SOURCE_DIR}/imgui_tables.cpp"
    "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp")
target_include_directories(vd_imgui SYSTEM PUBLIC "${imgui_SOURCE_DIR}")
target_compile_definitions(vd_imgui PUBLIC IMGUI_IMPL_VULKAN_USE_VOLK IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
target_link_libraries(vd_imgui PUBLIC vd_vulkan)

# Phase 22 (distribution): the third-party code we compile ourselves stays hidden inside libvanilladroid.so, like the
# engine's own (exported, every function would also survive --gc-sections). The published build (VD_DIST) never
# calls Dear ImGui: hidden, the linker drops it.
if(ANDROID)
    foreach(vd_third_party vd_lz4 vd_mbedtls vd_vulkan vd_imgui)
        target_compile_options(${vd_third_party} PRIVATE -fvisibility=hidden -ffunction-sections -fdata-sections)
    endforeach()
    target_compile_definitions(vd_lz4 PUBLIC LZ4LIB_VISIBILITY=) # lz4.h forces "default" on clang otherwise
endif()

# vd_engine: the game runtime. Only ever reads baked packs, never original client files.
# Vulkan stays private: game code talks to render/Renderer.h, which exposes no Vulkan types.
add_library(vd_engine STATIC
    src/app/WorldStateScreens.cpp # phase 17 (lead stub, f17/scores)
    # phase 17 (queue)
    src/app/BattlefieldScreens.cpp
    src/app/BattlefieldWindows.cpp
    src/app/BattlefieldText.cpp
    src/app/BattlefieldGallery.cpp
    src/app/WorldStateWindows.cpp # phase 17 (scores)
    src/app/WorldStateGallery.cpp
    src/app/PvpGallery.cpp # phase 17 (honor)
    src/app/PvpScreens.cpp
    src/app/GlueScene.cpp # phase 18 (lead)
    src/app/GlueStage.cpp
    # phase 18 (glue)
    src/app/GlueModel.cpp
    src/app/GlueWindows.cpp
    src/app/GlueScreens.cpp
    src/app/GlueGallery.cpp
    # phase 18 (create)
    src/app/CharCreateLayout.cpp
    src/app/CharCreateWindows.cpp
    src/app/CharCreateScreens.cpp
    src/app/CharCreateGallery.cpp
    # phase 18 (p3)
    src/app/P3Gallery.cpp
    src/app/AccountDataFiles.cpp
    src/app/ChatConfigScreens.cpp
    src/app/TutorialScreens.cpp
    src/app/TutorialWindows.cpp
    src/app/SortHeaders.cpp
    # phase 18 (extras)
    src/app/SpellBookFlash.cpp
    src/app/ExtrasGallery.cpp
    src/app/WtfFolders.cpp
    src/app/GameScreensKeys.cpp
    src/app/AuraGallery.cpp
    src/app/AuraLayout.cpp
    src/app/AuraScreens.cpp
    src/app/AuraWindows.cpp
    src/app/Benchmark.cpp
    src/app/CombatGallery.cpp
    src/app/CombatScreens.cpp
    src/app/CombatTextScreens.cpp
    src/app/CombatWindows.cpp
    src/app/ChatFrame.cpp
    src/app/ChatLinkInsert.cpp
    src/app/CharacterPreview.cpp
    # Phase 15 (character): the character window, reputation, skills, honor, inspect, the watch bar.
    src/app/CharacterGallery.cpp
    src/app/CharacterScreens.cpp
    src/app/CharacterWindows.cpp
    src/app/Cinematics.cpp
    src/app/Crowd.cpp
    src/app/CrowdFight.cpp
    src/app/DbcBrowser.cpp
    src/app/DebugHud.cpp
    src/app/DialogKit.cpp
    src/app/Dialogs.cpp
    src/app/FlyCamera.cpp
    src/app/Game.cpp
    src/app/GameSpellFx.cpp # phase 14 (spellfx)
    src/app/GameAudio.cpp
    src/app/GameAudioModels.cpp
    src/app/GameAudioPanel.cpp
    src/app/GameAudioScene.cpp
    src/app/GameAudioWorld.cpp
    src/app/GameAudioWeather.cpp
    src/app/GameSession.cpp
    src/app/Hud.cpp
    src/app/HudActionBar.cpp
    src/app/HudDemo.cpp
    src/app/HudAuras.cpp
    src/app/HudUnitFrames.cpp
    src/app/HudWorld.cpp
    src/app/HudTouchBar.cpp # phase 20 (docs/MOBILE_CONTROLS.md)
    src/app/HudTouchEdit.cpp # phase 20 (edit)
    src/app/TouchBarScreens.cpp # phase 20
    src/app/TouchSmart.cpp # phase 20 (smart)
    src/app/TouchEditScreens.cpp # phase 20 (edit)
    src/app/TouchPetScreens.cpp # phase 20 (pets)
    src/app/TouchPetWindows.cpp # phase 20 (pets)
    # phase 20 (smart) app
    # phase 20 (edit) app
    src/app/TouchEditWindows.cpp # phase 20 (edit)
    # phase 20 (classes) app
    src/app/TouchClassesDemo.cpp # phase 20 (classes): VD_TOUCH_DEMO=<class>:<level>
    # phase 20 (assists) app
    src/app/TouchAssists.cpp # phase 20 (assists)
    src/app/GameScreens.cpp
    src/app/GameScreensPanels.cpp
    src/app/InventoryGallery.cpp
    src/app/InventoryScreens.cpp
    src/app/InventoryWindows.cpp
    src/app/MicroMenu.cpp
    src/app/MinimapScreens.cpp
    src/app/MinimapView.cpp
    src/app/MessageHud.cpp
    src/app/MessagesGallery.cpp
    src/app/ServerObjects.cpp
    src/app/SpellEffects.cpp
    src/app/SocialGallery.cpp
    src/app/SocialScreens.cpp
    src/app/SocialWindows.cpp
    src/app/PanelDemo.cpp
    src/app/PlayerController.cpp
    src/app/PopupGallery.cpp
    src/app/PopupScreens.cpp
    src/app/PopupStack.cpp
    src/app/PopupWindows.cpp
    src/app/ModelFrame.cpp
    src/app/ModelFrameGallery.cpp
    src/app/ModelViews.cpp
    src/app/PetGallery.cpp # phase 15 (pets)
    src/app/PetScreens.cpp
    src/app/PetWindows.cpp
    src/app/PortraitGallery.cpp
    src/app/Portraits.cpp
    src/app/QualityGovernor.cpp
    src/app/QuestGallery.cpp
    src/app/QuestScreens.cpp
    src/app/QuestWindows.cpp
    src/app/SettingsFile.cpp
    src/app/TextureBrowser.cpp
    src/app/UiGallery.cpp
    src/app/AppOnlyGallery.cpp # phase 22 (apppack): VD_UI_GALLERY=apponly, app.vdpk alone
    # phase 22 (pairing): the first-run screen, the pairing with the PC, the download of the data
    src/app/DataDownload.cpp
    src/app/FirstRunScreens.cpp
    src/app/FirstRunWindows.cpp
    src/app/GameFirstRun.cpp
    src/app/Pairing.cpp
    # phase 22 (phonebake): the phone bakes its own data (the plan, the game's side, the desktop's runner)
    src/app/PhoneBake.cpp
    src/app/PhoneBakePlan.cpp
    src/app/PhoneBakeRunner.cpp
    src/platform/BakeLauncher.cpp
    # phase 22 (updates): the app's update notices and updates
    src/app/AppUpdates.cpp
    src/app/UpdateScreens.cpp
    src/app/UpdateWindows.cpp
    src/app/UiPanels.cpp
    src/app/UiResources.cpp
    src/app/Units.cpp
    src/app/QuestMarks.cpp # ours: TalkToMe*.m2 over the NPCs
    # retouches (ui models)
    src/app/UiModelsGallery.cpp # VD_UI_GALLERY=cooldowns, autocast, minimapping, roundicons
    src/app/TouchRoundIcon.cpp # the touch cluster's round icon with the 1.12 button's states
    # retouches (windows)
    src/app/TalentBranches.cpp # TalentFrame.lua's prerequisite lines
    src/app/WindowArt.cpp # 1.12's tabs, radio buttons, page arrows, skill bars, trade highlight
    src/app/WindowsGallery.cpp # VD_UI_GALLERY pages of the above
    src/app/UnitCombat.cpp
    src/app/UnitFrames.cpp
    src/app/UnitScreens.cpp
    src/app/UnitMenuScreens.cpp
    src/app/UnitMenuWindows.cpp
    src/app/UnitTooltip.cpp
    src/app/UnitsGallery.cpp
    src/app/WalkTest.cpp
    src/app/MapTravelTest.cpp # phase 18 (world)
    src/app/ChatBubbles.cpp
    src/app/SelectionCircle.cpp
    src/app/SpellTargetingScreens.cpp
    src/app/TargetingWindows.cpp
    src/app/TargetingGallery.cpp
    src/app/WorldLabels.cpp
    src/app/WorldLabelsGallery.cpp
    src/app/Transports.cpp
    src/app/MotionDemo.cpp # phase 18 (motion)
    src/app/FarSightView.cpp
    src/app/LoadingLayout.cpp # phase 20 (retouches): the 1.12 loading screen
    src/app/QuestLogLayout.cpp # retouches: QuestLogFrame's art, wide and tall
    src/app/QuestFrameLayout.cpp # retouches: QuestFrame's and GossipFrame's art
    src/app/FriendsFrameLayout.cpp # retouches: FriendsFrame's art with its tabs turned along its right edge
    src/app/CharacterFrameLayout.cpp # retouches: CharacterFrame's art, tall (the original) and wide (ours)
    src/app/TravelScreens.cpp
    src/app/TravelWindows.cpp
    src/app/ExchangeGallery.cpp
    src/app/ExchangeScreens.cpp
    src/app/ExchangeWindows.cpp
    src/app/TradeSkillGallery.cpp # phase 13 (professions)
    src/app/TradeSkillScreens.cpp
    src/app/TradeSkillWindows.cpp
    src/app/GuildGallery.cpp # phase 15 (guild, help)
    src/app/GuildScreens.cpp
    src/app/GuildWindows.cpp
    src/app/HelpScreens.cpp
    src/app/HelpWindows.cpp
    # Phase 16 (raids): the raid tab, the pullout frames, ready checks, raid warnings, meeting stones.
    src/app/RaidPullouts.cpp
    src/app/RaidWindows.cpp
    src/app/RaidScreens.cpp
    src/app/RaidGallery.cpp
    src/app/RaidPulloutLayout.cpp
    src/app/RaidPulloutWindows.cpp
    src/app/RaidPulloutSamples.cpp
    src/app/RaidPulloutGallery.cpp
    src/app/ObjectTooltip.cpp
    src/app/RaidAlertGallery.cpp
    src/app/RaidAlertScreens.cpp
    src/app/RaidAlertWindows.cpp
    src/app/PlayerLook.cpp
    src/app/MacroScreens.cpp # phase 15 (options)
    src/app/MacroWindows.cpp
    src/app/OptionsGallery.cpp
    src/app/OptionsScreens.cpp
    src/app/OptionsWindows.cpp
    src/app/WeatherControl.cpp
    src/app/StatusFlash.cpp
    src/app/CompareTooltips.cpp # phase 15 (items)
    src/app/ContainerLayout.cpp # phase 15 (items)
    src/app/ContainerWindows.cpp # phase 15 (items)
    src/app/DressUpScreens.cpp # phase 15 (items)
    src/app/DressUpWindows.cpp # phase 15 (items)
    src/app/InventoryCompare.cpp # phase 15 (items)
    src/app/InventoryContainers.cpp # phase 15 (items)
    src/app/ItemsGallery.cpp # phase 15 (items)
    src/app/TooltipLayout.cpp # phase 15 (items)
    src/platform/DeviceMonitor.cpp
    src/platform/Haptics.cpp # phase 20 (assists): vibration
    src/platform/PerformanceHint.cpp
    src/platform/UpdatePlatform.cpp # phase 22 (updates): HTTPS, DownloadManager, PackageInstaller (Java)
    src/render/GrassRenderer.cpp # phase 14 (grass)
    src/render/HorizonRenderer.cpp
    src/render/ModelRenderer.cpp
    src/render/ParticleRenderer.cpp
    src/render/ModelViewRenderer.cpp
    src/render/PortraitRenderer.cpp
    src/render/Renderer.cpp
    src/render/SkyRenderer.cpp
    src/render/UiRenderer.cpp
    src/render/WeatherRenderer.cpp
    src/render/TerrainRenderer.cpp
    src/render/TextureCompositor.cpp
    src/render/TextureStaging.cpp # phase 18 (world)
    src/render/vk/AstcSoftware.cpp
    src/render/vk/Device.cpp
    src/render/vk/Pipeline.cpp
    src/render/vk/Swapchain.cpp
    src/render/vk/Texture.cpp
    src/render/vk/Uploader.cpp
    src/render/vk/VkCommon.cpp
    src/render/vk/WorldPass.cpp # phase 18 (world)
    src/world/Actors.cpp
    src/world/AssetStreamer.cpp
    src/world/BodyDecals.cpp # phase 14 (bodyfx)
    src/world/Camera.cpp
    src/world/Celestial.cpp
    src/world/CinematicCamera.cpp
    src/world/DoodadAnimation.cpp
    src/world/Effects.cpp
    src/world/EffectsVisualWorld.cpp
    src/world/GrassPlacement.cpp # phase 14 (grass)
    src/world/GroundEffects.cpp
    src/world/InteriorLighting.cpp
    src/world/SpellVisuals.cpp
    src/world/SpellVisualsFx.cpp # phase 14 (spellfx): beams, tints, shakes
    src/world/CameraShakes.cpp
    src/world/SpellBeams.cpp
    src/world/Lighting.cpp
    src/world/PortraitCache.cpp
    src/world/Sky.cpp
    src/world/Terrain.cpp
    src/world/TerrainMapLoader.cpp # phase 18 (world)
    src/world/TerrainStreamer.cpp
    src/world/UnitBodyEffects.cpp # phase 14 (bodyfx)
    src/world/WorldObjects.cpp
    src/world/WmoBsp.cpp # phase 18 (world)
    src/world/WeatherEffect.cpp
    src/world/WmoAreas.cpp)
target_link_libraries(vd_engine PUBLIC vd_core vd_net vd_game vd_anim vd_fx vd_physics vd_ui vd_audio PRIVATE vd_vulkan vd_imgui)
if(WIN32)
    target_link_libraries(vd_engine PRIVATE winhttp) # phase 22 (updates): VD_UPDATE_URL in development
endif()
if(NOT ANDROID AND TARGET astcenc-avx2-static)
    # Desktop GPUs usually lack ASTC: decode on the CPU when uploading (development only).
    target_link_libraries(vd_engine PRIVATE astcenc-avx2-static)
    target_compile_definitions(vd_engine PRIVATE VD_ASTC_SOFTWARE_DECODE=1)
endif()
vd_set_target_options(vd_engine)
vd_add_shaders(vd_engine SOURCES
    "${PROJECT_SOURCE_DIR}/shaders/actor.vert"
    "${PROJECT_SOURCE_DIR}/shaders/compose.vert"
    "${PROJECT_SOURCE_DIR}/shaders/compose.frag"
    "${PROJECT_SOURCE_DIR}/shaders/grass.vert" # phase 14 (grass)
    "${PROJECT_SOURCE_DIR}/shaders/grass.frag"
    "${PROJECT_SOURCE_DIR}/shaders/doodad.vert"
    "${PROJECT_SOURCE_DIR}/shaders/horizon.vert"
    "${PROJECT_SOURCE_DIR}/shaders/horizon.frag"
    "${PROJECT_SOURCE_DIR}/shaders/liquid.vert"
    "${PROJECT_SOURCE_DIR}/shaders/model.vert"
    "${PROJECT_SOURCE_DIR}/shaders/model.frag"
    "${PROJECT_SOURCE_DIR}/shaders/model_alphakey.frag"
    "${PROJECT_SOURCE_DIR}/shaders/model_alphakey_a2c.frag" # phase 18 (world): with alpha to coverage
    "${PROJECT_SOURCE_DIR}/shaders/model_liquid.vert"
    "${PROJECT_SOURCE_DIR}/shaders/particle.vert"
    "${PROJECT_SOURCE_DIR}/shaders/particle.frag"
    "${PROJECT_SOURCE_DIR}/shaders/liquid.frag"
    "${PROJECT_SOURCE_DIR}/shaders/sky.vert"
    "${PROJECT_SOURCE_DIR}/shaders/sky.frag"
    "${PROJECT_SOURCE_DIR}/shaders/sky_clouds.vert"
    "${PROJECT_SOURCE_DIR}/shaders/sky_clouds.frag"
    "${PROJECT_SOURCE_DIR}/shaders/sky_model.vert"
    "${PROJECT_SOURCE_DIR}/shaders/sky_model.frag"
    "${PROJECT_SOURCE_DIR}/shaders/sky_sprite.vert"
    "${PROJECT_SOURCE_DIR}/shaders/sky_sprite.frag"
    "${PROJECT_SOURCE_DIR}/shaders/terrain.vert"
    "${PROJECT_SOURCE_DIR}/shaders/terrain.frag"
    "${PROJECT_SOURCE_DIR}/shaders/triangle.vert"
    "${PROJECT_SOURCE_DIR}/shaders/triangle.frag"
    "${PROJECT_SOURCE_DIR}/shaders/ui.vert"
    "${PROJECT_SOURCE_DIR}/shaders/ui.frag"
    "${PROJECT_SOURCE_DIR}/shaders/upscale.vert"
    "${PROJECT_SOURCE_DIR}/shaders/upscale.frag"
    "${PROJECT_SOURCE_DIR}/shaders/weather_rain.vert"
    "${PROJECT_SOURCE_DIR}/shaders/weather_flake.vert"
    "${PROJECT_SOURCE_DIR}/shaders/weather_mist.vert"
    "${PROJECT_SOURCE_DIR}/shaders/weather.frag")

if(ANDROID AND NOT VD_ANDROID_BAKER) # phase 22: the baker alone builds no game
    # Swappy (AGDK frame pacing). It loads Vulkan itself through dlopen, so it coexists with volk.
    find_package(games-frame-pacing REQUIRED CONFIG)
    target_link_libraries(vd_engine PRIVATE games-frame-pacing::swappy_static android)
endif()

add_executable(vd_tests
    test_main.cpp
    audio/test_audio.cpp
    audio/test_sound_data.cpp
    audio/test_game_audio.cpp
    audio/test_sound_parity.cpp
    audio/test_model_sounds.cpp # phase 14 (sound)
    # The game's sounds (phase 9) are app code without Vulkan: tested on their own.
    ${PROJECT_SOURCE_DIR}/engine/src/app/GameAudio.cpp
    ${PROJECT_SOURCE_DIR}/engine/src/app/GameAudioWorld.cpp
    ${PROJECT_SOURCE_DIR}/engine/src/app/GameAudioWeather.cpp
    # Phase 14 (sound): the models' sounds, over the AudioScene interface (fakes in the tests); the rooms of buildings.
    ${PROJECT_SOURCE_DIR}/engine/src/app/GameAudioModels.cpp
    ${PROJECT_SOURCE_DIR}/engine/src/world/WmoAreas.cpp
    world/test_wmo_areas.cpp
    audio/test_weather_audio.cpp
    # The spell visuals' scheduling (phase 9) works against an interface: tested with a recorder.
    ${PROJECT_SOURCE_DIR}/engine/src/world/SpellVisuals.cpp
    world/test_spell_visuals.cpp
    # Phase 14 (spellfx): the beams' geometry, the camera shakes and the kits' procs in SpellVisuals.
    ${PROJECT_SOURCE_DIR}/engine/src/world/SpellVisualsFx.cpp
    ${PROJECT_SOURCE_DIR}/engine/src/world/CameraShakes.cpp
    ${PROJECT_SOURCE_DIR}/engine/src/world/SpellBeams.cpp
    world/test_spell_fx.cpp
    # Phase 11: Light.dbc's parameter sets per condition, against the client's tables.
    ${PROJECT_SOURCE_DIR}/engine/src/world/Lighting.cpp
    world/test_lighting.cpp
    # Phase 11: the weather's fade, ground and particles (no Vulkan), and the protocol and state of SMSG_WEATHER.
    ${PROJECT_SOURCE_DIR}/engine/src/world/WeatherEffect.cpp
    world/test_weather.cpp
    game/test_weather.cpp
    game/test_lost_messages.cpp
    # Phase 11: the sky's curves of WoW.exe (the light's direction, the sun, the moons, the stars, the glares).
    ${PROJECT_SOURCE_DIR}/engine/src/world/Celestial.cpp
    world/test_sky.cpp
    # Phase 11 (f11/interiors): interior light, fogs, night glow and terrain layer scrolling, without Vulkan.
    ${PROJECT_SOURCE_DIR}/engine/src/world/InteriorLighting.cpp
    world/test_interior_lighting.cpp
    # Phase 14 (grass): WoW.exe's detail doodad placement, without Vulkan.
    ${PROJECT_SOURCE_DIR}/engine/src/world/GrassPlacement.cpp
    world/test_grass.cpp
    # The unit frames' portrait slots (parity) are bookkeeping without Vulkan: tested on their own.
    ${PROJECT_SOURCE_DIR}/engine/src/world/PortraitCache.cpp
    world/test_portrait_cache.cpp
    # phase 18 (world): opening a map off the game thread (the loader, the texture staging, the asset ring)
    world/test_map_open.cpp
    ${PROJECT_SOURCE_DIR}/engine/src/render/TextureStaging.cpp
    ${PROJECT_SOURCE_DIR}/engine/src/render/vk/AstcSoftware.cpp
    ${PROJECT_SOURCE_DIR}/engine/src/world/AssetStreamer.cpp
    ${PROJECT_SOURCE_DIR}/engine/src/world/TerrainMapLoader.cpp
    ${PROJECT_SOURCE_DIR}/engine/src/app/MapTravelTest.cpp
    world/test_wmo_bsp.cpp # and the camera's group and the portal walk, the 1.12 way
    ${PROJECT_SOURCE_DIR}/engine/src/world/WmoBsp.cpp
    # World feedback (parity): chat bubbles and the selection circle's geometry are app code without Vulkan.
    ${PROJECT_SOURCE_DIR}/engine/src/app/ChatBubbles.cpp
    ${PROJECT_SOURCE_DIR}/engine/src/app/SelectionCircle.cpp
    app/test_world_feedback.cpp
    # The aura buttons' layout (parity) is app code without Vulkan: tested on its own.
    ${PROJECT_SOURCE_DIR}/engine/src/app/AuraLayout.cpp
    app/test_aura_layout.cpp
    # The StaticPopup stack's rules (parity: popups) are app code without Vulkan: tested on their own.
    ${PROJECT_SOURCE_DIR}/engine/src/app/PopupStack.cpp
    app/test_popup_stack.cpp
    # The panel windows' rules and places (parity pass) are app code without Vulkan: tested on their own.
    ${PROJECT_SOURCE_DIR}/engine/src/app/UiPanels.cpp
    app/test_panels.cpp
    # Phase 15 (items): the ContainerFrames' rules and layout and the tooltips' placement, app code without Vulkan.
    ${PROJECT_SOURCE_DIR}/engine/src/app/ContainerLayout.cpp
    ${PROJECT_SOURCE_DIR}/engine/src/app/TooltipLayout.cpp
    app/test_containers.cpp
    game/test_try_on.cpp # phase 15 (items): comparisons, the dressing room, master loot
    game/test_battlegrounds.cpp # phase 17 (lead): the queues and the world states
    # phase 17 (queue)
    game/test_battlefield_queue.cpp
    ${PROJECT_SOURCE_DIR}/engine/src/app/BattlefieldText.cpp
    game/test_world_state_ui.cpp # phase 17 (scores): WorldStateUI, the score, positions, joined / left
    game/test_battlefield_score.cpp
    game/test_pvp.cpp # phase 17 (honor): the area spirit healer, honor, the insignia, outdoor PvP
    # phase 18 (glue) tests here
    net/test_realm_list.cpp
    net/test_realm_choice.cpp
    ${PROJECT_SOURCE_DIR}/engine/src/app/GlueModel.cpp
    app/test_glue_model.cpp
    # phase 18 (create) tests here
    game/test_char_create_rules.cpp
    ${PROJECT_SOURCE_DIR}/engine/src/app/CharCreateLayout.cpp
    app/test_char_create_layout.cpp
    # phase 18 (p3) tests here
    game/test_tutorials.cpp # phase 18 (p3): tutorials and the account data through Gameplay
    net/test_account_data.cpp
    game/test_chat_windows.cpp # phase 18 (p3): chat-cache.txt, the mailbox's delete, the new popups
    game/test_list_sort.cpp # phase 18 (p3): SortWho and SortAuctionItems
    # phase 18 (extras) tests here
    game/test_languages.cpp # B4: the languages garbled as WoW.exe does; the small packets
    audio/test_extras_audio.cpp # the pet's sounds
    ${PROJECT_SOURCE_DIR}/engine/src/app/SpellBookFlash.cpp
    ${PROJECT_SOURCE_DIR}/engine/src/app/WtfFolders.cpp
    app/test_extras_app.cpp # the spell book's new-spell flash, the renamed character's WTF folders
    # Phase 10: the dynamic quality and the settings file are app code without Vulkan: tested on their own.
    ${PROJECT_SOURCE_DIR}/engine/src/app/QualityGovernor.cpp
    ${PROJECT_SOURCE_DIR}/engine/src/app/SettingsFile.cpp
    app/test_quality.cpp
    app/test_video_options.cpp # phase 18 (world): anisotropic filtering and multisampling in the options
    app/test_options_app.cpp # phase 15 (options)
    # Phase 13 (social): the chat's link insertion (app/ChatLinkInsert) is app code without Vulkan.
    ${PROJECT_SOURCE_DIR}/engine/src/app/ChatLinkInsert.cpp
    # Phase 13 (combat): the full-screen status flashes' rules (UIParent.lua) are app code without Vulkan.
    ${PROJECT_SOURCE_DIR}/engine/src/app/StatusFlash.cpp
    app/test_status_flash.cpp
    # Phase 14: the cinematics' cameras and their player are app and world code without Vulkan.
    ${PROJECT_SOURCE_DIR}/engine/src/world/CinematicCamera.cpp
    ${PROJECT_SOURCE_DIR}/engine/src/app/Cinematics.cpp
    core/test_frame_arena.cpp
    core/test_text.cpp
    core/test_result.cpp
    core/test_json.cpp # phase 22: manifests and release files
    pack/test_pack_manifest.cpp # phase 22: recipes, bake steps, packs/manifest.json
    # phase 22 (wave 1) tests here
    # phase 22 (phonebake): baking on the phone: the plan, the runner, the WoW's archives, the install, the game's side
    app/test_phone_bake.cpp
    ${PROJECT_SOURCE_DIR}/engine/src/app/PhoneBake.cpp
    ${PROJECT_SOURCE_DIR}/engine/src/app/PhoneBakePlan.cpp
    ${PROJECT_SOURCE_DIR}/engine/src/app/PhoneBakeRunner.cpp
    ${PROJECT_SOURCE_DIR}/engine/src/platform/BakeLauncher.cpp
    core/test_release_signature.cpp # phase 22 (release): the release manifest's signature
    # phase 22 (pairing): the app's HTTP client, the pairing link and code, pairing.json, the download's rules
    net/test_http_client.cpp
    app/test_pairing.cpp
    ${PROJECT_SOURCE_DIR}/engine/src/app/Pairing.cpp
    ${PROJECT_SOURCE_DIR}/engine/src/app/DataDownload.cpp
    pack/test_app_pack_install.cpp # phase 22 (apppack): app.vdpk copied from the APK
    jobs/test_job_defaults.cpp # phase 22 (phonebaker): the baker's --jobs
    pack/test_release_manifest.cpp # phase 22 (updates): a release's manifest and what it changes
    fx/test_particles.cpp
    fx/test_model_particles.cpp # phase 14 (spellfx)
    game/test_appearance.cpp
    game/test_macros.cpp # phase 15 (options)
    game/test_options.cpp # phase 15 (options)
    game/test_auras.cpp
    game/test_core_texts.cpp
    game/test_char_create.cpp
    game/test_chat_commands.cpp
    game/test_gameplay.cpp
    game/test_quests.cpp
    game/test_abilities.cpp
    game/test_pets.cpp # phase 15 (pets)
    game/test_ability_data.cpp
    game/test_action_bar.cpp
    game/test_touch_layout.cpp # phase 20: the touch layout, its layers and file, the spell lines
    ui/test_touch_widgets.cpp # phase 20: round widgets
    game/test_touch_pets.cpp # phase 20 (pets): the pet button, control, death, Feed Pet's memory
    # phase 20 (smart) tests here
    game/test_touch_smart.cpp # the consumables, the mounts, the reactive spells
    # phase 20 (edit) tests here
    app/test_touch_edit.cpp # phase 20 (edit): the edit mode's changes, the server's copy
    # phase 20 (classes) tests here
    game/test_class_presets.cpp # phase 20 (classes): the presets of the nine classes, the usability rules
    # phase 20 (assists) tests here
    app/test_touch_assists.cpp # phase 20 (assists): the assists' rules and state machines
    # retouches (ui models)
    ui/test_ui_models.cpp # the cooldown, autocast and minimap ping models (VD_PACKS_DIR)
    # retouches (windows)
    ${PROJECT_SOURCE_DIR}/engine/src/app/TalentBranches.cpp
    app/test_talent_branches.cpp # retouches (windows): TalentFrame.lua's prerequisite lines
    ${PROJECT_SOURCE_DIR}/engine/src/app/LoadingLayout.cpp
    app/test_loading_screen.cpp # retouches: the 1.12 loading screen's layout and its tips
    ${PROJECT_SOURCE_DIR}/engine/src/app/QuestLogLayout.cpp
    ${PROJECT_SOURCE_DIR}/engine/src/app/QuestFrameLayout.cpp
    app/test_quest_log_layout.cpp # retouches: QuestLogFrame's art, wide and tall
    ${PROJECT_SOURCE_DIR}/engine/src/app/FriendsFrameLayout.cpp
    app/test_friends_frame_layout.cpp # retouches: FriendsFrame's art with its tabs turned
    ${PROJECT_SOURCE_DIR}/engine/src/app/CharacterFrameLayout.cpp
    app/test_character_frame_layout.cpp # retouches: CharacterFrame's art, tall and wide
    game/test_spell_targeting.cpp
    game/test_combat_text.cpp
    game/test_social.cpp
    game/test_who_query.cpp # retouches (friends frame): SendWho's reading of a who query
    game/test_inventory.cpp
    game/test_minimap.cpp
    game/test_travel.cpp
    game/test_spell_visual_data.cpp
    game/test_spell_kit_fx.cpp # phase 14 (spellfx)
    game/test_exchange.cpp
    game/test_messages.cpp
    game/test_unit_text.cpp
    game/test_unit_fade.cpp
    render/test_screen_fx.cpp
    game/test_popups.cpp
    game/test_tradeskills.cpp # phase 13 (professions)
    game/test_character_sheet.cpp # phase 15 (character)
    net/test_character_messages.cpp # phase 15 (character)
    game/test_guild.cpp # phase 15 (guild, help)
    jobs/test_job_system.cpp
    net/test_combat_messages.cpp
    net/test_pet_messages.cpp # phase 15 (pets)
    net/test_guild_messages.cpp # phase 15 (guild, help)
    net/test_crypto.cpp
    net/test_game_messages.cpp
    net/test_item_messages.cpp
    net/test_session.cpp
    net/test_social_messages.cpp
    net/test_exchange_messages.cpp
    net/test_popup_messages.cpp
    net/test_misc_messages.cpp
    net/test_quest_messages.cpp
    physics/test_collision.cpp
    ui/test_ui_core.cpp
    ui/test_item_drag.cpp
    ui/test_text_input.cpp
    ui/test_widgets.cpp
    ui/test_text_wrap.cpp # phase 15 (help): wrapping after color escapes
    ui/test_message_frame.cpp
    ui/test_chat_dock.cpp
    game/test_chat_links.cpp
    game/test_unit_menus.cpp
    game/test_slash_commands.cpp
    net/test_player_command_messages.cpp
    net/test_spell_log_messages.cpp
    game/test_mover_control.cpp
    # phase 18 (motion): the units' movement states, transports, the change mask, the far sight
    net/test_spline_messages.cpp
    game/test_unit_motion.cpp
    game/test_spell_log.cpp
    # Phase 16 (raids): the roster, the raid tab, the pullouts, ready checks and meeting stones.
    game/test_raid_roster.cpp
    game/test_raid_frame.cpp
    game/test_raid_party.cpp
    ${PROJECT_SOURCE_DIR}/engine/src/app/RaidPulloutLayout.cpp
    app/test_raid_pullouts.cpp
    game/test_raid_alerts.cpp
    # Phase 14 (bodyfx): the body effects' tables, and the marks and shadows (world/BodyDecals, no Vulkan).
    game/test_body_data.cpp
    ${PROJECT_SOURCE_DIR}/engine/src/world/BodyDecals.cpp
    world/test_body_decals.cpp)
target_include_directories(vd_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(vd_tests PRIVATE vd_core vd_net vd_game vd_anim vd_fx vd_physics vd_ui vd_audio doctest::doctest)

if(TARGET vd_formats)
    target_sources(vd_tests PRIVATE
        formats/test_actors.cpp
        formats/test_actor_events.cpp
        formats/test_cinematics.cpp
        formats/test_action_layer.cpp
        formats/test_bake.cpp
        formats/test_collision_bake.cpp
        formats/test_dbc.cpp
        formats/test_doodad_anim.cpp # phase 14 (doodads)
        ${PROJECT_SOURCE_DIR}/engine/src/world/DoodadAnimation.cpp
        formats/test_dxt.cpp
        formats/test_effects.cpp
        formats/test_grass_bake.cpp # phase 14
        formats/test_interior_bake.cpp
        formats/test_lua_strings.cpp
        formats/test_client_text_encoding.cpp # the client's Windows-1252 bytes
        formats/test_locale_bake.cpp # phase 21: clients in other languages
        formats/test_models.cpp
        formats/test_moving_collision.cpp
        formats/test_mpq_blp.cpp
        formats/test_portraits.cpp
        formats/test_sky_models.cpp
        formats/test_sound_bake.cpp
        formats/test_terrain.cpp
        formats/test_ui_bake.cpp
        formats/test_svg_icon.cpp # phase 20: our own art (art/touch)
        formats/test_app_pack.cpp # phase 22 (apppack): app.vdpk and the texts' lookup over both packs
        formats/test_world_sound_bake.cpp # phase 14 (sound)
        formats/test_ranged_weapons.cpp
        formats/test_particle_meshes.cpp) # phase 14 (spellfx)
    target_link_libraries(vd_tests PRIVATE vd_formats)
    target_compile_definitions(vd_tests PRIVATE VD_ART_DIR="${PROJECT_SOURCE_DIR}/art") # phase 20
    target_compile_definitions(vd_tests PRIVATE VD_LOCALE_DIR="${PROJECT_SOURCE_DIR}/locale") # phase 22 (apppack)
    target_sources(vd_tests PRIVATE formats/test_portable.cpp) # phase 22 (phonebaker): the same packs everywhere
    target_include_directories(vd_tests PRIVATE "${PROJECT_SOURCE_DIR}/cmake/astcenc")
endif()
# Phase 22 (setup): VanillaDroid Setup's core (tools/setup), built with the tools.
if(TARGET vd_setup)
    target_sources(vd_tests PRIVATE
        setup/test_setup_settings.cpp
        setup/test_setup_http.cpp
        setup/test_setup_wow.cpp
        setup/test_setup_bake.cpp
        setup/test_setup_misc.cpp
        setup/test_pairing_download.cpp # phase 22 (pairing): the app's download against PhoneServer
        setup/test_setup_flow.cpp # phase 22 (gui): the window's steps (SetupFlow), the download's progress
        setup/test_setup_update.cpp) # phase 22 (updates)
    target_link_libraries(vd_tests PRIVATE vd_setup)
    target_sources(vd_tests PRIVATE setup/test_setup_polish.cpp) # phase 22 (polish): the window's and console's texts
endif()
vd_set_target_options(vd_tests)
# Phase 22 (release): test vectors (tests/data: the test release key, files signed with it).
target_compile_definitions(vd_tests PRIVATE VD_TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/tests/data")

add_test(NAME vd_tests COMMAND vd_tests)
# Tests that need the original client read it from here and skip themselves if it is missing.
# Tests of the runtime tables read the baked packs of the working tree (skipped without them).
set_tests_properties(vd_tests PROPERTIES ENVIRONMENT "VD_WOW_DIR=${VD_WOW_DIR};VD_PACKS_DIR=${PROJECT_SOURCE_DIR}/packs")

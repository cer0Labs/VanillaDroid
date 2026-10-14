#pragma once

#include "core/Types.h"
#include "db/DbcView.h"

#include <array>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace vd::pack {
class PackReader;
}

namespace vd::audio {

// A SoundEntries.dbc row, ready to play: up to 10 files (one is picked at random, weighted by its frequency),
// volume, 3D range. File hashes are soundPathHash() of DirectoryBase + File, the key of sound.vdpk.
struct SoundEntry {
    static constexpr u32 kMaxFiles = 10;
    u32 id = 0;
    u32 type = 0; // SoundType: 1 spell, 2 interface, 3 footsteps, 4 weapon impact, 6 swing, 10 creature, 28 music...
    std::string_view name;
    f32 volume = 1.0f;
    u32 flags = 0;
    static constexpr u32 kFlagLoop = 0x200; // loops (precasts, missiles, campfires: every *Loop row has it)
    f32 minDistance = 8.0f;  // full volume inside
    f32 maxDistance = 45.0f; // DistanceCutoff: silent beyond
    u32 fileCount = 0;
    std::array<u64, kMaxFiles> files{};
    std::array<u8, kMaxFiles> weights{}; // Freq
    // One of the files at random (weighted) from `random` (any 32-bit value); 0 when the entry has none.
    u64 pick(u32 random) const;
};

// ZoneMusic.dbc: the music set of a zone, by day and by night, and the silence between two tracks.
struct ZoneMusicSet {
    u32 id = 0;
    std::array<u32, 2> sound{};       // SoundEntries id [day, night]
    std::array<u32, 2> silenceMinMs{}; // [day, night]
    std::array<u32, 2> silenceMaxMs{};
};

// ZoneIntroMusicTable.dbc: the piece played when entering a zone (a city's intro), at most every few minutes.
struct ZoneIntro {
    u32 id = 0;
    u32 sound = 0;
    u32 priority = 0;
    u32 minDelayMinutes = 0;
};

// What an area (AreaTable.dbc) sounds like; subzones take what they lack from their zone.
struct AreaSound {
    u32 music = 0;          // ZoneMusic id
    u32 intro = 0;          // ZoneIntroMusicTable id
    std::array<u32, 2> ambience{}; // SoundEntries id [day, night] (from SoundAmbience)
};

// CreatureSoundData.dbc (a creature model's sounds): SoundEntries ids, 0 = none. The 1.12 table has 30 columns
// and no NPCSounds one (that is CreatureDisplayInfo's): 23 is the loop (FireElementalLoop, SlimeLoop...), 24 the
// impact type, 25-26 the jump sounds (empty in 1.12), 27-29 the warlock demons' attack / order / dismiss lines.
// Phase 14: four fidgets (14-17) and four custom attacks (18-21), as WoW.exe reads them for the $FD1..$FD4 and
// $AH0..$AH3 events (0x6230A0, 0x6247D0); 22 is empty in every row.
struct CreatureSounds {
    u32 id = 0;
    u32 exertion = 0;         // attacking
    u32 exertionCritical = 0;
    u32 injury = 0;           // being hit
    u32 injuryCritical = 0;
    u32 injuryCrushingBlow = 0;
    u32 death = 0;
    u32 stun = 0;
    u32 stand = 0;
    u32 footstep = 0;         // FootstepTerrainLookup's creature footstep id
    u32 aggro = 0;
    u32 wingFlap = 0;
    u32 wingGlide = 0;
    u32 alert = 0;
    std::array<u32, 4> fidget{};
    std::array<u32, 4> customAttack{};
    u32 loop = 0;
    u32 impactType = 0;       // 0 by its armor, 1 stone (the infernal and the stone golem: see impactColumn())
    u32 jumpStart = 0;
    u32 jumpEnd = 0;
    u32 petAttack = 0;
    u32 petOrder = 0;
    u32 petDismiss = 0;
};

// ItemGroupSounds.dbc through ItemDisplayInfo's group sound index: what an item sounds like picked up and put down.
struct ItemSounds {
    u32 pickup = 0;
    u32 drop = 0;
};

// GameObjectDisplayInfo.dbc's sounds, by the game object's animation (SoundEntries ids, 0 = none).
struct ObjectSounds {
    u32 stand = 0;
    u32 open = 0;
    u32 loop = 0; // loops while the object is there (campfires, torches, machines)
    u32 close = 0;
    u32 destroy = 0;
    u32 opened = 0;
    std::array<u32, 4> custom{};
    // What it sounds like standing there: the first looping sound (SoundEntry::kFlagLoop) of loop, stand and custom0
    // (the 1.12 rows put campfires and torches in the first, braziers, cauldrons and lamps in the last).
    u32 ambient = 0;
};

// NPCSounds.dbc: what an NPC says when a gossip window opens and closes.
struct NpcSounds {
    u32 greeting = 0;
    u32 farewell = 0;
    u32 pissed = 0;
};

// The sound tables of the client (phase 9), indexed from db.vdpk. Field numbers of the 1.12.1 layouts, checked
// against the real tables in tests/audio. Read-only after init(): lookups never allocate.
class SoundData {
public:
    bool init(const pack::PackReader& db);
    bool ready() const { return !m_entries.empty(); }

    const SoundEntry* entry(u32 id) const;
    // By its name, case-insensitive ("igMainMenuOpen", "LEVELUPSOUND": the names FrameXML's PlaySound uses).
    const SoundEntry* entryByName(std::string_view name) const;

    // --- Zones (AreaTable -> ZoneMusic / ZoneIntroMusicTable / SoundAmbience), parent zone as fallback.
    AreaSound areaSound(u32 areaId) const;
    const ZoneMusicSet* zoneMusic(u32 id) const;
    const ZoneIntro* zoneIntro(u32 id) const;

    // --- Creatures: the sounds of a creature display (CreatureDisplayInfo's own, else its model's).
    const CreatureSounds* creatureSounds(u32 displayId) const;
    // NPCSounds of a display (CreatureDisplayInfo's, else its creature sounds').
    const NpcSounds* npcSounds(u32 displayId) const;
    // The creature footstep id a display walks with (0 when it has none).
    u32 footstepId(u32 displayId) const;

    // --- Combat: WeaponImpactSounds by weapon (ItemSubClass of the weapon class; kUnarmed for fists) and the
    // victim's impact column (0 flesh, 1 chain, 2 plate, 3/4 shield metal/wood, 5/6 parry metal/wood, 7 wood,
    // 8 stone, 9 ethereal); WeaponSwingSounds2 by swing weight (0 light, 1 medium, 2 heavy).
    static constexpr u32 kUnarmed = 13; // fist weapons' sounds
    u32 weaponImpact(u32 weaponSubclass, u32 impactColumn, bool critical) const;
    u32 weaponSwing(u32 swingType, bool critical) const;
    static u32 swingType(u32 weaponSubclass);

    // --- Footsteps: FootstepTerrainLookup (creature footstep id, TerrainType's sound id); splash in water.
    u32 footstep(u32 footstepId, u32 terrainSoundId, bool splash) const;

    // --- Spells: Spell.dbc -> SpellVisual -> its kits' sounds (SpellVisualKit).
    struct SpellSounds {
        u32 precast = 0; // loops while casting
        u32 cast = 0;
        u32 impact = 0;
        u32 state = 0;   // the aura's state kit
        u32 channel = 0; // loops while channeling
        u32 missile = 0; // loops while the missile flies
        f32 speed = 0.0f; // the missile's, yards per second (Spell.dbc); 0 = no missile, the impact is at once
    };
    SpellSounds spellSounds(u32 spellId) const;
    // A SpellVisualKit's sound (SMSG_PLAY_SPELL_VISUAL / _IMPACT name a kit).
    u32 kitSound(u32 kitId) const;

    // --- Voices: EmotesTextSound (a text emote's voice by race and sex: 0 male, 1 female), VocalUISounds (the
    // character complaining: 0 "not enough mana"... by race and sex).
    u32 emoteSound(u32 emotesTextId, u32 race, u32 sex) const;
    u32 vocalError(u32 vocalUi, u32 race, u32 sex) const;

    // --- Parity pass: the character's body (ChrRaces, Sound\Character\PlayerExertions).
    u32 splashSound(u32 race) const;      // ChrRaces.m_SplashSoundID: entering the water
    u32 explorationSound(u32 race) const; // ChrRaces.m_ExplorationSoundId: a new area discovered
    // DeathThudLookups: a body falling on the ground (by the display's size class and the ground's TerrainType sound)
    // or into the water.
    u32 deathThud(u32 displayId, u32 terrainSoundId, bool water) const;
    // CreatureDisplayInfo.m_sizeClass: 0 small .. 4 colossal (-1 none: medium).
    i32 sizeClass(u32 displayId) const;
    // EnvironmentalDamage.dbc: SMSG_ENVIRONMENTALDAMAGELOG's type (0 exhausted, 1 drowning, 2 fall, 3 lava, 4 slime,
    // 5 fire) -> its visual kit's sound (0: the kit has none, as for falls).
    u32 environmentSound(u32 type) const;
    // SoundWaterType.dbc: the loop of a liquid (pack::LiquidKind: 0 water, 1 ocean, 2 magma, 3 slime) by its flow
    // (0 still, 4 slow, 8 fast).
    u32 waterSound(u32 liquidKind, u32 flow = 0) const;
    // Items: ItemDisplayInfo.m_groupSoundIndex -> ItemGroupSounds.
    ItemSounds itemSounds(u32 itemDisplayId) const;
    // SheatheSoundLookups: putting a weapon or shield away / taking it out (item class, subclass, Material.dbc id).
    u32 sheatheSound(u32 itemClass, u32 subclass, u32 material, bool sheathe) const;
    // Game objects: GameObjectDisplayInfo's sounds (nullptr: none known).
    const ObjectSounds* objectSounds(u32 displayId) const;
    // A ZoneMusic set by its name ("GhostMusic (DONOTRENAME)": the client's code names it).
    const ZoneMusicSet* zoneMusicByName(std::string_view name) const;

    // --- Phase 14 (the models' sounds, the buildings' rooms).
    // SoundAmbience.dbc: a WMOAreaTable row's ambience [day, night] (SoundEntries ids).
    std::array<u32, 2> ambience(u32 soundAmbienceId) const;
    // The sounds of an actor's model (actors.vdpk asset hash of its .m2) through CreatureModelData, for actors whose
    // display is not known (synthetic crowds, mounts): nullptr when the model has none.
    const CreatureSounds* modelSounds(u64 actorModelHash) const;
    // Emotes.dbc: the sound an emote state makes on its $ESD events (mining, chopping wood), 0 for none (only state
    // emotes, EmoteSpecProc 2, as WoW.exe 0x6239F0 reads it).
    u32 emoteStateSound(u32 emoteId) const;
    // WeaponImpactSounds column of a victim: a creature's impact type, else its body armor (ItemSubClass of the
    // armor class: 3 mail -> chain, 4 plate -> plate, else flesh).
    static u32 impactColumn(u32 creatureImpactType, u32 armorSubclass);

private:
    struct Table {
        db::DbcView view;
        std::unordered_map<u32, u32> rows; // id -> record
        bool load(const pack::PackReader& db, const char* name, u32 minFields);
        i32 find(u32 id) const;
    };
    void initParity(const pack::PackReader& db);

    std::vector<SoundEntry> m_entries;
    std::unordered_map<u32, u32> m_entryById;
    std::unordered_map<u64, u32> m_entryByName; // hash of the lowercase name
    std::vector<ZoneMusicSet> m_zoneMusic;
    std::vector<ZoneIntro> m_intros;
    std::unordered_map<u32, AreaSound> m_areas;
    std::unordered_map<u32, u32> m_areaParent;
    std::unordered_map<u32, CreatureSounds> m_creatureSounds;
    std::unordered_map<u32, NpcSounds> m_npcSounds;
    Table m_displays;
    Table m_models;
    Table m_impacts;
    Table m_spells;
    Table m_visuals;
    Table m_kits;
    std::unordered_map<u32, u32> m_impactBySubclass; // weapon subclass -> record (the metal row when there are two)
    std::array<u32, 6> m_swings{};                   // swing type * 2 + critical -> sound
    std::unordered_map<u64, std::array<u32, 2>> m_footsteps; // footstep id << 32 | terrain -> [step, splash]
    std::unordered_map<u64, u32> m_emoteSounds; // emote << 16 | race << 8 | sex -> sound
    std::unordered_map<u64, std::array<u32, 2>> m_vocal; // vocal << 16 | race -> [male, female]
    // Parity pass.
    struct RaceSounds {
        u32 splash = 0;
        u32 exploration = 0;
    };
    std::unordered_map<u32, RaceSounds> m_races;
    std::unordered_map<u32, std::array<u32, 2>> m_thuds; // size class << 8 | terrain sound -> [ground, water]
    std::array<u32, 7> m_environment{};                  // EnvironmentalDamage type -> sound
    std::unordered_map<u32, u32> m_water;                // liquid kind << 8 | flow -> sound
    Table m_itemDisplays;
    std::unordered_map<u32, ItemSounds> m_itemGroups;
    std::unordered_map<u64, std::array<u32, 2>> m_sheathes; // class << 40 | subclass << 8 | material -> [in, out]
    std::unordered_map<u32, ObjectSounds> m_objects;
    std::vector<std::pair<u64, u32>> m_zoneMusicByName; // name hash -> m_zoneMusic index
    // Phase 14.
    std::unordered_map<u32, std::array<u32, 2>> m_ambiences;   // SoundAmbience id -> [day, night]
    std::unordered_map<u64, u32> m_modelSoundIds;              // actor model hash -> CreatureSoundData id
    std::unordered_map<u32, u32> m_emoteStateSounds;           // Emotes id -> sound
};

} // namespace vd::audio

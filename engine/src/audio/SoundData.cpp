#include "audio/SoundData.h"

#include "audio/SoundAsset.h"
#include "core/Hash.h"
#include "core/Log.h"
#include "pack/PackReader.h"

#include <algorithm>
#include <string>

namespace vd::audio {
namespace {

// Field numbers of the 1.12.1 (5875) tables. vMaNGOS' DBCStructure.h names the AreaTable, CreatureDisplayInfo,
// CreatureModelData and SpellVisual columns; the others (sound tables the server never reads) were laid out from
// the client's own data and are checked row by row in tests/audio/test_sound_data.cpp.
namespace sound {
constexpr u32 kType = 1, kName = 2, kFile = 3, kFreq = 13, kDirectory = 23, kVolume = 24, kFlags = 25,
              kMinDistance = 26, kMaxDistance = 27, kFieldCount = 29;
}
namespace area {
constexpr u32 kParent = 2, kAmbience = 7, kZoneMusic = 8, kIntro = 9; // m_AmbienceID, m_ZoneMusic, m_IntroSound
}
constexpr u32 kZoneMusicSilenceMin = 2, kZoneMusicSilenceMax = 4, kZoneMusicSounds = 6; // [day, night] each
constexpr u32 kIntroSound = 2, kIntroPriority = 3, kIntroMinDelay = 4;                  // ZoneIntroMusicTable
constexpr u32 kAmbienceDay = 1, kAmbienceNight = 2;                                     // SoundAmbience
constexpr u32 kDisplayModel = 1, kDisplaySound = 2, kDisplayNpcSound = 11;              // CreatureDisplayInfo
constexpr u32 kModelPath = 2, kModelSound = 13;                                         // CreatureModelData
constexpr u32 kEmoteSpecProc = 4, kEmoteEventSound = 6; // Emotes.dbc (phase 14: WoW.exe 0x6239F0)
constexpr u32 kNpcGreeting = 1, kNpcFarewell = 2, kNpcPissed = 3;                       // NPCSounds
constexpr u32 kImpactSubclass = 1, kImpactParryType = 2, kImpactSounds = 3, kImpactCritSounds = 13; // [10] each
constexpr u32 kSwingType = 1, kSwingCrit = 2, kSwingSound = 3;                          // WeaponSwingSounds2
constexpr u32 kStepFootstep = 1, kStepTerrain = 2, kStepSound = 3, kStepSplash = 4;     // FootstepTerrainLookup
constexpr u32 kSpellSpeed = 37, kSpellVisual = 115;                                     // Spell.dbc (see GameData)
constexpr u32 kVisualPrecast = 1, kVisualCast = 2, kVisualImpact = 3, kVisualState = 4, kVisualChannel = 5,
              kVisualMissileSound = 10;                                                 // SpellVisual
constexpr u32 kKitSound = 13;                                                           // SpellVisualKit
constexpr u32 kEmoteText = 1, kEmoteRace = 2, kEmoteSex = 3, kEmoteSound = 4;           // EmotesTextSound
constexpr u32 kVocalEnum = 1, kVocalRace = 2, kVocalMale = 3, kVocalFemale = 4;         // VocalUISounds
// Parity pass. ChrRaces: vMaNGOS' DBCStructure.h names 3 m_ExplorationSoundId, 13 m_SplashSoundID and
// 15 m_clientFileString. CreatureDisplayInfo 9 m_sizeClass. The others were laid out from the client's rows (their
// names in SoundEntries) and are checked in tests/audio/test_sound_data.cpp.
namespace creature {
constexpr u32 kLoop = 23, kImpactType = 24, kJumpStart = 25, kJumpEnd = 26, kPetAttack = 27, kPetOrder = 28,
              kPetDismiss = 29, kFieldCount = 30;
}
constexpr u32 kRaceExploration = 3, kRaceSplash = 13, kRaceFileString = 15;            // ChrRaces
constexpr u32 kDisplaySizeClass = 9;                                                    // CreatureDisplayInfo
constexpr u32 kThudSize = 1, kThudTerrain = 2, kThudSound = 3, kThudWater = 4;          // DeathThudLookups
constexpr u32 kEnvironmentType = 1, kEnvironmentKit = 2;                                // EnvironmentalDamage
constexpr u32 kWaterLiquid = 1, kWaterFlow = 2, kWaterSound = 3;                        // SoundWaterType
constexpr u32 kItemDisplayGroupSound = 11;                                              // ItemDisplayInfo
constexpr u32 kItemGroupPickup = 1, kItemGroupDrop = 2;                                 // ItemGroupSounds
constexpr u32 kSheatheClass = 1, kSheatheSubclass = 2, kSheatheMaterial = 3, kSheatheCheckMaterial = 4,
              kSheatheIn = 5, kSheatheOut = 6;                                          // SheatheSoundLookups
constexpr u32 kObjectSounds = 2;                                                        // GameObjectDisplayInfo [10]
constexpr u32 kZoneMusicName = 1;                                                       // ZoneMusic
constexpr u8 kAnyMaterial = 0xFF;

bool open(const pack::PackReader& db, const char* name, u32 minFields, db::DbcView& view) {
    const std::optional<pack::PackAsset> asset = db.find(std::string("DBFilesClient\\") + name + ".dbc");
    if (!asset) {
        VD_LOG_WARN("%s.dbc missing: its sounds are off", name);
        return false;
    }
    Result<db::DbcView> parsed = db::DbcView::parse(asset->data, asset->size);
    if (!parsed || parsed.value().fieldCount() < minFields) {
        VD_LOG_WARN("%s.dbc: unexpected layout, its sounds are off", name);
        return false;
    }
    view = parsed.value();
    return true;
}

u64 nameHash(std::string_view name) {
    u64 hash = kFnvOffset;
    for (const char c : name) {
        hash ^= static_cast<u8>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        hash *= kFnvPrime;
    }
    return hash;
}

} // namespace

u64 SoundEntry::pick(u32 random) const {
    u32 total = 0;
    for (u32 i = 0; i < fileCount; ++i) {
        total += weights[i];
    }
    if (fileCount == 0) {
        return 0;
    }
    if (total == 0) {
        return files[random % fileCount];
    }
    u32 roll = random % total;
    for (u32 i = 0; i < fileCount; ++i) {
        if (roll < weights[i]) {
            return files[i];
        }
        roll -= weights[i];
    }
    return files[fileCount - 1];
}

bool SoundData::Table::load(const pack::PackReader& db, const char* name, u32 minFields) {
    if (!open(db, name, minFields, view)) {
        return false;
    }
    rows.reserve(view.recordCount());
    for (u32 r = 0; r < view.recordCount(); ++r) {
        rows.emplace(view.getU32(r, 0), r);
    }
    return true;
}

i32 SoundData::Table::find(u32 id) const {
    const auto it = rows.find(id);
    return it == rows.end() ? -1 : static_cast<i32>(it->second);
}

bool SoundData::init(const pack::PackReader& db) {
    db::DbcView view;
    if (!open(db, "SoundEntries", sound::kFieldCount, view)) {
        return false;
    }
    m_entries.resize(view.recordCount());
    m_entryById.reserve(view.recordCount());
    m_entryByName.reserve(view.recordCount());
    for (u32 r = 0; r < view.recordCount(); ++r) {
        SoundEntry& e = m_entries[r];
        e.id = view.getU32(r, 0);
        e.type = view.getU32(r, sound::kType);
        e.name = view.getString(r, sound::kName);
        e.volume = view.getF32(r, sound::kVolume);
        e.flags = view.getU32(r, sound::kFlags);
        e.minDistance = view.getF32(r, sound::kMinDistance);
        e.maxDistance = view.getF32(r, sound::kMaxDistance);
        const std::string_view directory = view.getString(r, sound::kDirectory);
        for (u32 f = 0; f < SoundEntry::kMaxFiles; ++f) {
            const std::string_view file = view.getString(r, sound::kFile + f);
            if (file.empty()) {
                continue;
            }
            e.files[e.fileCount] = soundPathHash(directory, file);
            e.weights[e.fileCount] = static_cast<u8>(std::min<u32>(view.getU32(r, sound::kFreq + f), 255));
            ++e.fileCount;
        }
        m_entryById.emplace(e.id, r);
        if (!e.name.empty()) {
            m_entryByName.emplace(nameHash(e.name), r); // the first row of a name wins
        }
    }

    if (open(db, "ZoneMusic", kZoneMusicSounds + 2, view)) {
        m_zoneMusic.resize(view.recordCount());
        for (u32 r = 0; r < view.recordCount(); ++r) {
            ZoneMusicSet& z = m_zoneMusic[r];
            z.id = view.getU32(r, 0);
            for (u32 n = 0; n < 2; ++n) {
                z.silenceMinMs[n] = view.getU32(r, kZoneMusicSilenceMin + n);
                z.silenceMaxMs[n] = view.getU32(r, kZoneMusicSilenceMax + n);
                z.sound[n] = view.getU32(r, kZoneMusicSounds + n);
            }
            if (const std::string_view name = view.getString(r, kZoneMusicName); !name.empty()) {
                m_zoneMusicByName.emplace_back(nameHash(name), r);
            }
        }
    }
    if (open(db, "ZoneIntroMusicTable", kIntroMinDelay + 1, view)) {
        m_intros.resize(view.recordCount());
        for (u32 r = 0; r < view.recordCount(); ++r) {
            m_intros[r] = {view.getU32(r, 0), view.getU32(r, kIntroSound), view.getU32(r, kIntroPriority),
                           view.getU32(r, kIntroMinDelay)};
        }
    }
    std::unordered_map<u32, std::array<u32, 2>>& ambiences = m_ambiences;
    if (open(db, "SoundAmbience", kAmbienceNight + 1, view)) {
        for (u32 r = 0; r < view.recordCount(); ++r) {
            ambiences[view.getU32(r, 0)] = {view.getU32(r, kAmbienceDay), view.getU32(r, kAmbienceNight)};
        }
    }
    if (open(db, "AreaTable", area::kIntro + 1, view)) {
        m_areas.reserve(view.recordCount());
        for (u32 r = 0; r < view.recordCount(); ++r) {
            AreaSound a;
            a.music = view.getU32(r, area::kZoneMusic);
            a.intro = view.getU32(r, area::kIntro);
            if (const auto it = ambiences.find(view.getU32(r, area::kAmbience)); it != ambiences.end()) {
                a.ambience = it->second;
            }
            m_areas.emplace(view.getU32(r, 0), a);
            m_areaParent.emplace(view.getU32(r, 0), view.getU32(r, area::kParent));
        }
    }

    if (open(db, "CreatureSoundData", creature::kFieldCount, view)) {
        m_creatureSounds.reserve(view.recordCount());
        for (u32 r = 0; r < view.recordCount(); ++r) {
            CreatureSounds c;
            c.id = view.getU32(r, 0);
            c.exertion = view.getU32(r, 1);
            c.exertionCritical = view.getU32(r, 2);
            c.injury = view.getU32(r, 3);
            c.injuryCritical = view.getU32(r, 4);
            c.injuryCrushingBlow = view.getU32(r, 5);
            c.death = view.getU32(r, 6);
            c.stun = view.getU32(r, 7);
            c.stand = view.getU32(r, 8);
            c.footstep = view.getU32(r, 9);
            c.aggro = view.getU32(r, 10);
            c.wingFlap = view.getU32(r, 11);
            c.wingGlide = view.getU32(r, 12);
            c.alert = view.getU32(r, 13);
            for (u32 i = 0; i < 4; ++i) {
                c.fidget[i] = view.getU32(r, 14 + i);
                c.customAttack[i] = view.getU32(r, 18 + i); // phase 14: 18-21 (WoW.exe's $AH0..$AH3)
            }
            c.loop = view.getU32(r, creature::kLoop);
            c.impactType = view.getU32(r, creature::kImpactType);
            c.jumpStart = view.getU32(r, creature::kJumpStart);
            c.jumpEnd = view.getU32(r, creature::kJumpEnd);
            c.petAttack = view.getU32(r, creature::kPetAttack);
            c.petOrder = view.getU32(r, creature::kPetOrder);
            c.petDismiss = view.getU32(r, creature::kPetDismiss);
            m_creatureSounds.emplace(c.id, c);
        }
    }
    if (open(db, "NPCSounds", kNpcPissed + 1, view)) {
        for (u32 r = 0; r < view.recordCount(); ++r) {
            m_npcSounds[view.getU32(r, 0)] = {view.getU32(r, kNpcGreeting), view.getU32(r, kNpcFarewell),
                                              view.getU32(r, kNpcPissed)};
        }
    }
    m_displays.load(db, "CreatureDisplayInfo", kDisplayNpcSound + 1);
    if (m_models.load(db, "CreatureModelData", kModelSound + 1)) {
        // Phase 14: by the model's path as actors.vdpk keys it (the table says .mdx, the client loads the .m2).
        for (u32 r = 0; r < m_models.view.recordCount(); ++r) {
            std::string path(m_models.view.getString(r, kModelPath));
            if (const usize dot = path.find_last_of('.'); dot != std::string::npos) {
                path.replace(dot, std::string::npos, ".m2");
            }
            if (const u32 id = m_models.view.getU32(r, kModelSound); id != 0 && path.size() > 3) {
                m_modelSoundIds.emplace(assetPathHash(path), id);
            }
        }
    }
    if (open(db, "Emotes", kEmoteEventSound + 1, view)) {
        for (u32 r = 0; r < view.recordCount(); ++r) {
            if (view.getU32(r, kEmoteSpecProc) == 2 && view.getU32(r, kEmoteEventSound) != 0) {
                m_emoteStateSounds.emplace(view.getU32(r, 0), view.getU32(r, kEmoteEventSound));
            }
        }
    }
    if (m_impacts.load(db, "WeaponImpactSounds", kImpactCritSounds + 10)) {
        for (u32 r = 0; r < m_impacts.view.recordCount(); ++r) {
            const u32 subclass = m_impacts.view.getU32(r, kImpactSubclass);
            // Two rows for some weapons: prefer the one with the metal parry sounds (parry type 1).
            const auto [it, inserted] = m_impactBySubclass.emplace(subclass, r);
            if (!inserted && m_impacts.view.getU32(r, kImpactParryType) == 1) {
                it->second = r;
            }
        }
    }
    if (open(db, "WeaponSwingSounds2", kSwingSound + 1, view)) {
        for (u32 r = 0; r < view.recordCount(); ++r) {
            const u32 type = view.getU32(r, kSwingType);
            const u32 crit = view.getU32(r, kSwingCrit) != 0 ? 1 : 0;
            if (type < 3) {
                m_swings[type * 2 + crit] = view.getU32(r, kSwingSound);
            }
        }
    }
    if (open(db, "FootstepTerrainLookup", kStepSplash + 1, view)) {
        for (u32 r = 0; r < view.recordCount(); ++r) {
            const u64 key = (static_cast<u64>(view.getU32(r, kStepFootstep)) << 32) | view.getU32(r, kStepTerrain);
            m_footsteps[key] = {view.getU32(r, kStepSound), view.getU32(r, kStepSplash)};
        }
    }
    m_spells.load(db, "Spell", kSpellVisual + 1);
    m_visuals.load(db, "SpellVisual", kVisualMissileSound + 1);
    m_kits.load(db, "SpellVisualKit", kKitSound + 1);
    if (open(db, "EmotesTextSound", kEmoteSound + 1, view)) {
        for (u32 r = 0; r < view.recordCount(); ++r) {
            const u64 key = (static_cast<u64>(view.getU32(r, kEmoteText)) << 16) |
                            (static_cast<u64>(view.getU32(r, kEmoteRace) & 0xFF) << 8) | (view.getU32(r, kEmoteSex) & 0xFF);
            m_emoteSounds[key] = view.getU32(r, kEmoteSound);
        }
    }
    if (open(db, "VocalUISounds", kVocalFemale + 1, view)) {
        for (u32 r = 0; r < view.recordCount(); ++r) {
            const u64 key = (static_cast<u64>(view.getU32(r, kVocalEnum)) << 16) | (view.getU32(r, kVocalRace) & 0xFFFF);
            m_vocal[key] = {view.getU32(r, kVocalMale), view.getU32(r, kVocalFemale)};
        }
    }
    initParity(db);
    VD_LOG_INFO("Sound tables: %zu sounds, %zu zone music sets, %zu areas, %zu creature sound sets", m_entries.size(),
                m_zoneMusic.size(), m_areas.size(), m_creatureSounds.size());
    return true;
}

void SoundData::initParity(const pack::PackReader& db) {
    db::DbcView view;
    // Races: splash and exploration sounds.
    if (open(db, "ChrRaces", kRaceFileString + 1, view)) {
        for (u32 r = 0; r < view.recordCount(); ++r) {
            RaceSounds& race = m_races[view.getU32(r, 0)];
            race.exploration = view.getU32(r, kRaceExploration);
            race.splash = view.getU32(r, kRaceSplash);
        }
    }
    if (open(db, "DeathThudLookups", kThudWater + 1, view)) {
        for (u32 r = 0; r < view.recordCount(); ++r) {
            m_thuds[(view.getU32(r, kThudSize) << 8) | (view.getU32(r, kThudTerrain) & 0xFF)] = {
                view.getU32(r, kThudSound), view.getU32(r, kThudWater)};
        }
    }
    if (open(db, "EnvironmentalDamage", kEnvironmentKit + 1, view)) {
        for (u32 r = 0; r < view.recordCount(); ++r) {
            if (const u32 type = view.getU32(r, kEnvironmentType); type < m_environment.size()) {
                m_environment[type] = kitSound(view.getU32(r, kEnvironmentKit));
            }
        }
    }
    if (open(db, "SoundWaterType", kWaterSound + 1, view)) {
        for (u32 r = 0; r < view.recordCount(); ++r) {
            m_water[(view.getU32(r, kWaterLiquid) << 8) | (view.getU32(r, kWaterFlow) & 0xFF)] =
                view.getU32(r, kWaterSound);
        }
    }
    m_itemDisplays.load(db, "ItemDisplayInfo", kItemDisplayGroupSound + 1);
    if (open(db, "ItemGroupSounds", kItemGroupDrop + 1, view)) {
        for (u32 r = 0; r < view.recordCount(); ++r) {
            m_itemGroups[view.getU32(r, 0)] = {view.getU32(r, kItemGroupPickup), view.getU32(r, kItemGroupDrop)};
        }
    }
    if (open(db, "SheatheSoundLookups", kSheatheOut + 1, view)) {
        for (u32 r = 0; r < view.recordCount(); ++r) {
            const u64 material = view.getU32(r, kSheatheCheckMaterial) != 0 ? (view.getU32(r, kSheatheMaterial) & 0xFF)
                                                                            : kAnyMaterial;
            const u64 key = (static_cast<u64>(view.getU32(r, kSheatheClass)) << 40) |
                            (static_cast<u64>(view.getU32(r, kSheatheSubclass) & 0xFFFFFFFF) << 8) | material;
            m_sheathes[key] = {view.getU32(r, kSheatheIn), view.getU32(r, kSheatheOut)};
        }
    }
    if (open(db, "GameObjectDisplayInfo", kObjectSounds + 10, view)) {
        for (u32 r = 0; r < view.recordCount(); ++r) {
            ObjectSounds o;
            o.stand = view.getU32(r, kObjectSounds);
            o.open = view.getU32(r, kObjectSounds + 1);
            o.loop = view.getU32(r, kObjectSounds + 2);
            o.close = view.getU32(r, kObjectSounds + 3);
            o.destroy = view.getU32(r, kObjectSounds + 4);
            o.opened = view.getU32(r, kObjectSounds + 5);
            for (u32 i = 0; i < 4; ++i) {
                o.custom[i] = view.getU32(r, kObjectSounds + 6 + i);
            }
            for (const u32 id : {o.loop, o.stand, o.custom[0]}) {
                const SoundEntry* e = id != 0 ? entry(id) : nullptr;
                if (o.ambient == 0 && e && (e->flags & SoundEntry::kFlagLoop) != 0) {
                    o.ambient = id;
                }
            }
            if (o.stand || o.open || o.loop || o.close || o.destroy || o.opened || o.custom[0] || o.custom[1] ||
                o.custom[2] || o.custom[3]) {
                m_objects.emplace(view.getU32(r, 0), o);
            }
        }
    }
}

const SoundEntry* SoundData::entry(u32 id) const {
    const auto it = m_entryById.find(id);
    return it == m_entryById.end() ? nullptr : &m_entries[it->second];
}

const SoundEntry* SoundData::entryByName(std::string_view name) const {
    const auto it = m_entryByName.find(nameHash(name));
    return it == m_entryByName.end() ? nullptr : &m_entries[it->second];
}

AreaSound SoundData::areaSound(u32 areaId) const {
    AreaSound result;
    // The subzone, then its zone (a parent chain is at most two deep in 1.12; four steps guard against loops).
    for (u32 depth = 0; depth < 4 && areaId != 0; ++depth) {
        const auto it = m_areas.find(areaId);
        if (it == m_areas.end()) {
            break;
        }
        const AreaSound& a = it->second;
        if (result.music == 0) {
            result.music = a.music;
        }
        if (result.intro == 0) {
            result.intro = a.intro;
        }
        if (result.ambience[0] == 0 && result.ambience[1] == 0) {
            result.ambience = a.ambience;
        }
        const auto parent = m_areaParent.find(areaId);
        areaId = parent == m_areaParent.end() ? 0 : parent->second;
    }
    return result;
}

const ZoneMusicSet* SoundData::zoneMusic(u32 id) const {
    for (const ZoneMusicSet& z : m_zoneMusic) {
        if (z.id == id) {
            return &z;
        }
    }
    return nullptr;
}

const ZoneIntro* SoundData::zoneIntro(u32 id) const {
    for (const ZoneIntro& z : m_intros) {
        if (z.id == id) {
            return &z;
        }
    }
    return nullptr;
}

const CreatureSounds* SoundData::creatureSounds(u32 displayId) const {
    const i32 display = m_displays.find(displayId);
    if (display < 0) {
        return nullptr;
    }
    u32 id = m_displays.view.getU32(static_cast<u32>(display), kDisplaySound);
    if (id == 0) {
        const i32 model = m_models.find(m_displays.view.getU32(static_cast<u32>(display), kDisplayModel));
        id = model < 0 ? 0 : m_models.view.getU32(static_cast<u32>(model), kModelSound);
    }
    const auto it = m_creatureSounds.find(id);
    return it == m_creatureSounds.end() ? nullptr : &it->second;
}

const NpcSounds* SoundData::npcSounds(u32 displayId) const {
    // Only CreatureDisplayInfo names them (CreatureSoundData's column 23, once read as NPC sounds, is its loop).
    const i32 display = m_displays.find(displayId);
    const u32 id = display < 0 ? 0 : m_displays.view.getU32(static_cast<u32>(display), kDisplayNpcSound);
    const auto it = m_npcSounds.find(id);
    return it == m_npcSounds.end() ? nullptr : &it->second;
}

u32 SoundData::footstepId(u32 displayId) const {
    const CreatureSounds* creature = creatureSounds(displayId);
    return creature ? creature->footstep : 0;
}

u32 SoundData::weaponImpact(u32 weaponSubclass, u32 impactColumn, bool critical) const {
    auto it = m_impactBySubclass.find(weaponSubclass);
    if (it == m_impactBySubclass.end()) {
        it = m_impactBySubclass.find(kUnarmed);
    }
    if (it == m_impactBySubclass.end() || impactColumn >= 10) {
        return 0;
    }
    return m_impacts.view.getU32(it->second, (critical ? kImpactCritSounds : kImpactSounds) + impactColumn);
}

u32 SoundData::weaponSwing(u32 swingType, bool critical) const {
    return swingType < 3 ? m_swings[swingType * 2 + (critical ? 1 : 0)] : 0;
}

u32 SoundData::swingType(u32 weaponSubclass) {
    switch (weaponSubclass) {
    case 1:  // two-handed axe
    case 5:  // two-handed mace
    case 6:  // polearm
    case 8:  // two-handed sword
    case 10: // staff
    case 17: // spear
        return 2;
    case 13: // fist weapon (and no weapon)
    case 15: // dagger
        return 0;
    default: return 1;
    }
}

u32 SoundData::footstep(u32 footstepId, u32 terrainSoundId, bool splash) const {
    const auto it = m_footsteps.find((static_cast<u64>(footstepId) << 32) | terrainSoundId);
    return it == m_footsteps.end() ? 0 : it->second[splash ? 1 : 0];
}

SoundData::SpellSounds SoundData::spellSounds(u32 spellId) const {
    SpellSounds s;
    const i32 spell = m_spells.find(spellId);
    if (spell < 0) {
        return s;
    }
    s.speed = m_spells.view.getF32(static_cast<u32>(spell), kSpellSpeed);
    const i32 visual = m_visuals.find(m_spells.view.getU32(static_cast<u32>(spell), kSpellVisual));
    if (visual < 0) {
        return s;
    }
    const auto kit = [&](u32 field) { return kitSound(m_visuals.view.getU32(static_cast<u32>(visual), field)); };
    s.precast = kit(kVisualPrecast);
    s.cast = kit(kVisualCast);
    s.impact = kit(kVisualImpact);
    s.state = kit(kVisualState);
    s.channel = kit(kVisualChannel);
    s.missile = m_visuals.view.getU32(static_cast<u32>(visual), kVisualMissileSound);
    return s;
}

u32 SoundData::kitSound(u32 kitId) const {
    const i32 kit = kitId == 0 ? -1 : m_kits.find(kitId);
    if (kit < 0) {
        return 0;
    }
    const i32 sound = m_kits.view.getI32(static_cast<u32>(kit), kKitSound);
    return sound > 0 ? static_cast<u32>(sound) : 0;
}

u32 SoundData::emoteSound(u32 emotesTextId, u32 race, u32 sex) const {
    const auto it = m_emoteSounds.find((static_cast<u64>(emotesTextId) << 16) | ((race & 0xFF) << 8) | (sex & 0xFF));
    return it == m_emoteSounds.end() ? 0 : it->second;
}

u32 SoundData::vocalError(u32 vocalUi, u32 race, u32 sex) const {
    const auto it = m_vocal.find((static_cast<u64>(vocalUi) << 16) | (race & 0xFFFF));
    return it == m_vocal.end() ? 0 : it->second[sex == 1 ? 1 : 0];
}

// --- Parity pass --------------------------------------------------------------------------------------------------

u32 SoundData::splashSound(u32 race) const {
    const auto it = m_races.find(race);
    return it == m_races.end() ? 0 : it->second.splash;
}

u32 SoundData::explorationSound(u32 race) const {
    const auto it = m_races.find(race);
    return it == m_races.end() ? 0 : it->second.exploration;
}

i32 SoundData::sizeClass(u32 displayId) const {
    const i32 display = m_displays.find(displayId);
    return display < 0 ? -1 : m_displays.view.getI32(static_cast<u32>(display), kDisplaySizeClass);
}

u32 SoundData::deathThud(u32 displayId, u32 terrainSoundId, bool water) const {
    const i32 size = sizeClass(displayId);
    const u32 key = (static_cast<u32>(size < 0 ? 1 : std::min(size, 4)) << 8) | (terrainSoundId & 0xFF);
    auto it = m_thuds.find(key);
    if (it == m_thuds.end()) {
        it = m_thuds.find((key & ~0xFFu) | 1); // unknown ground: dirt
    }
    return it == m_thuds.end() ? 0 : it->second[water ? 1 : 0];
}

u32 SoundData::environmentSound(u32 type) const {
    return type < m_environment.size() ? m_environment[type] : 0;
}

u32 SoundData::waterSound(u32 liquidKind, u32 flow) const {
    const auto it = m_water.find((liquidKind << 8) | (flow & 0xFF));
    return it == m_water.end() ? 0 : it->second;
}

ItemSounds SoundData::itemSounds(u32 itemDisplayId) const {
    const i32 display = itemDisplayId != 0 ? m_itemDisplays.find(itemDisplayId) : -1;
    if (display < 0) {
        return {};
    }
    const auto it = m_itemGroups.find(m_itemDisplays.view.getU32(static_cast<u32>(display), kItemDisplayGroupSound));
    return it == m_itemGroups.end() ? ItemSounds{} : it->second;
}

u32 SoundData::sheatheSound(u32 itemClass, u32 subclass, u32 material, bool sheathe) const {
    const u64 base = (static_cast<u64>(itemClass) << 40) | (static_cast<u64>(subclass) << 8);
    auto it = m_sheathes.find(base | (material & 0xFF));
    if (it == m_sheathes.end()) {
        it = m_sheathes.find(base | kAnyMaterial);
    }
    return it == m_sheathes.end() ? 0 : it->second[sheathe ? 0 : 1];
}

const ObjectSounds* SoundData::objectSounds(u32 displayId) const {
    const auto it = m_objects.find(displayId);
    return it == m_objects.end() ? nullptr : &it->second;
}

const ZoneMusicSet* SoundData::zoneMusicByName(std::string_view name) const {
    const u64 hash = nameHash(name);
    for (const auto& [h, index] : m_zoneMusicByName) {
        if (h == hash) {
            return &m_zoneMusic[index];
        }
    }
    return nullptr;
}

// --- Phase 14 -------------------------------------------------------------------------------------------------------

std::array<u32, 2> SoundData::ambience(u32 soundAmbienceId) const {
    const auto it = m_ambiences.find(soundAmbienceId);
    return it == m_ambiences.end() ? std::array<u32, 2>{} : it->second;
}

const CreatureSounds* SoundData::modelSounds(u64 actorModelHash) const {
    const auto id = m_modelSoundIds.find(actorModelHash);
    if (id == m_modelSoundIds.end()) {
        return nullptr;
    }
    const auto it = m_creatureSounds.find(id->second);
    return it == m_creatureSounds.end() ? nullptr : &it->second;
}

u32 SoundData::emoteStateSound(u32 emoteId) const {
    const auto it = m_emoteStateSounds.find(emoteId);
    return it == m_emoteStateSounds.end() ? 0 : it->second;
}

u32 SoundData::impactColumn(u32 creatureImpactType, u32 armorSubclass) {
    // WeaponImpactSounds' columns: 0 flesh, 1 chain, 2 plate, 3/4 shield metal/wood, 5/6 parry metal/wood, 7 wood,
    // 8 stone, 9 ethereal. Only two 1.12 creatures have an impact type, both of rock (Infernal.mdx,
    // GolemStone.mdx): type 1 is read as stone.
    if (creatureImpactType == 1) {
        return 8;
    }
    switch (armorSubclass) {
    case 3: return 1; // mail
    case 4: return 2; // plate
    default: return 0;
    }
}

} // namespace vd::audio

// GameAudio, parity pass: every other sound the 1.12 client plays that phase 9 did not. The character's body
// (jumps, landings, splashes), the environment (falls, lava, drowning), water (the camera under it, liquid loops),
// deaths (bodies hitting the ground), items (picked up, put down, looted, equipped), game objects (doors, chests,
// campfires), weapons drawn and put away, invitations, exploration and taxi discoveries, more vocal errors. The
// tables are read by audio/SoundData, the choices made by audio/SoundRules; this file follows the game state.
#include "app/GameAudio.h"

#include "app/GameAudioInternal.h"
#include "audio/SoundRules.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "game/Gameplay.h"
#include "game/UnitInfo.h"
#include "net/GameMessages.h"
#include "net/ItemMessages.h"
#include "net/TravelMessages.h"
#include "net/UpdateFields.gen.h"
#include "physics/CollisionWorld.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace vd {
namespace {

using namespace gameaudio;
using audio::Category;
namespace uf = net::uf;

constexpr u32 kEnvironmentFall = 2; // EnviromentalDamage (vMaNGOS SharedDefines.h): DAMAGE_FALL
constexpr u32 kGoStateReady = 1;    // GOState: GO_STATE_READY, a door closed (0 active: open)
constexpr u32 kSheathMelee = 1, kSheathRanged = 2; // SheathState (UNIT_FIELD_BYTES_2 byte 0)

constexpr u32 kWaterSampleMs = 250;  // liquids around the listener, sampled this often
constexpr f64 kWaterRange = 30.0;    // ...this far
constexpr u32 kObjectScanMs = 250;   // game objects around the listener
constexpr f64 kObjectRange = 50.0;
constexpr u32 kThudDelayMs = 900;    // a death cry, then the body on the ground (the death animations' $DTH)
constexpr u32 kEquipQuietMs = 3000;  // after entering the world: the equipment arrives, silently
constexpr u32 kItemWaitMs = 3000;    // an item sound waits this long for its template
constexpr u32 kFidgetMinMs = 8000;   // an idle creature around makes a sound every 8 to 20 s
constexpr u32 kFidgetMaxMs = 20000;
constexpr f64 kFidgetRange = 25.0;

} // namespace

void GameAudio::initParity() {
    const auto byName = [&](const char* name) {
        const audio::SoundEntry* e = m_data->entryByName(name);
        if (!e) {
            VD_LOG_WARN("Audio: no SoundEntries row named %s", name);
        }
        return e ? e->id : 0u;
    };
    // Rows the client's code plays by name (the strings are in WoW.exe; "(DONOTRENAME)" marks most of them).
    m_missSound = {byName("(DONOTRENAME)Combat Miss 1H"), byName("(DONOTRENAME)Combat Miss 2H")};
    m_absorbSound = byName("(DONOTRENAME)AbsorbGetHit");
    m_underwaterSound = byName("Underwater (DONOTRENAME)");
    m_ghostSound = byName("Ghost (DONOTRENAME)");
    const audio::ZoneMusicSet* ghost = m_data->zoneMusicByName("GhostMusic (DONOTRENAME)");
    m_ghostMusic = ghost ? ghost->id : 0;
    // Sound\Spells\PlayerDamage: the fall's (its EnvironmentalDamage kit has no sound).
    m_fallSound = byName("DamageFalling");
}

void GameAudio::resetParity(const GameAudioFrame& f) {
    const game::Gameplay& play = *f.play;
    m_inventoryVersion = 0;
    m_equipped = {};
    m_equippedKnown = false;
    m_equipQuietUntilMs = f.nowMs + kEquipQuietMs;
    for (ObjectMemory& o : m_objects) {
        m_audio->stop(o.loop, 0.5f);
        o = {};
    }
    m_items = {};
    m_sheath = 0xFF;
    m_fidgetAtMs = f.nowMs + kFidgetMinMs;
    m_invitePending = !play.social().group().pendingInviter.empty();
    m_duelPending = play.social().duel().phase == game::DuelPhase::Challenged;
}

GameAudio::Body GameAudio::bodyOf(const GameAudioFrame& f) const {
    Body b;
    b.display = f.display;
    if (f.play && f.entities) {
        if (const game::Entity* me = f.entities->find(f.player)) {
            b.race = me->byteValue(uf::UNIT_FIELD_BYTES_0, 0);
            b.sex = me->byteValue(uf::UNIT_FIELD_BYTES_0, 2);
            b.display = me->value(uf::UNIT_FIELD_DISPLAYID);
            b.mount = me->value(uf::UNIT_FIELD_MOUNTDISPLAYID);
        }
        b.ghost = f.play->death().ghost;
    }
    return b;
}

void GameAudio::playItem(u32 itemEntry, bool pickup) {
    if (!active() || itemEntry == 0) {
        return;
    }
    // Played once its template (the display, and so its ItemGroupSounds) is known: update() resolves it.
    PendingItem* slot = &m_items[0];
    for (PendingItem& p : m_items) {
        if (p.entry == 0) {
            slot = &p;
            break;
        }
        if (static_cast<i32>(p.sinceMs - slot->sinceMs) < 0) {
            slot = &p;
        }
    }
    *slot = {itemEntry, m_nowMs, pickup};
}

bool GameAudio::playedWithin(u32 soundEntry, u32 ms) const {
    for (const Recent& r : m_recent) {
        if (r.entry == soundEntry && r.atMs != 0 && !reached(m_nowMs, r.atMs + ms)) {
            return true;
        }
    }
    return false;
}

u32 GameAudio::vocalPowerType(const GameAudioFrame& f) const {
    const game::Entity* me = f.entities ? f.entities->find(f.player) : nullptr;
    return me ? me->byteValue(uf::UNIT_FIELD_BYTES_0, 3) : 0;
}

// --- The character's body ---------------------------------------------------------------------------------------

void GameAudio::bodySounds(const GameAudioFrame& f) {
    if (!f.world || f.loading || !f.character) {
        return;
    }
    // Jumping and landing make no sound of their own in 1.12 (no table links the PlayerExertions' MainJump / MainLand
    // rows, WoW.exe never names them, CreatureSoundData's jump columns are empty): landing is the step on the ground
    // (footsteps()), a fall that hurts brings SMSG_ENVIRONMENTALDAMAGELOG (environmentDamage()).
    // Into deep water (falling in or walking in): the race's splash (ChrRaces.m_SplashSoundID).
    if (f.startedSwim) {
        playSound(m_data->splashSound(m_body.race), Category::Effects, kPriorityPlayer, &f.feet);
    }
}

void GameAudio::environmentDamage(const GameAudioFrame& f, const game::CombatEvent& e) {
    const game::Entity* victim = f.entities->find(e.victim);
    glm::dvec3 at;
    if (!victim || !positionOf(f, e.victim, at)) {
        return;
    }
    const u8 priority = e.victim == f.player ? kPriorityPlayer : kPriorityWorld;
    // The damage's own sound: its EnvironmentalDamage visual kit's (drowning, lava, slime, fire), for a fall
    // the client's "DamageFalling".
    u32 sound = m_data->environmentSound(e.spellId);
    if (sound == 0 && e.spellId == kEnvironmentFall) {
        sound = m_fallSound;
    }
    playSound(sound, Category::Effects, priority, &at);
    // The victim's wound (drowning and lava hurt every couple of seconds: not every tick).
    const audio::CreatureSounds* hurt = soundsOf(*victim);
    if (hurt && e.amount > 0 && unitCooldown(e.victim, kWound, f.nowMs, 1500)) {
        playSound(hurt->injury, Category::Voices, priority, &at);
    }
    watchUnit(e.victim, f.nowMs); // a fall can kill
}

// --- Water ---------------------------------------------------------------------------------------------------------

void GameAudio::waterSounds(const GameAudioFrame& f) {
    const physics::CollisionWorld* world = f.world && !f.login && !f.loading ? f.collision : nullptr;
    // The camera under a liquid's surface: the client's underwater ambience (updateAmbience()). Phase 14: the
    // listener is the character's head; the camera says whether the view is under water.
    const glm::dvec3 eye = f.camera != glm::dvec3(0.0) ? f.camera : f.listener;
    m_underwater = false;
    if (world) {
        const std::optional<physics::LiquidSample> above = world->liquidAt(eye.x, eye.y, eye.z + 50.0, eye.z);
        m_underwater = above && static_cast<f64>(above->level) > eye.z;
    }
    if (!world) {
        if (m_water) {
            m_audio->stop(m_water, 1.0f);
            m_water = {};
        }
        m_waterEntry = 0;
        return;
    }
    if (!reached(f.nowMs, m_waterSampleMs)) {
        return;
    }
    m_waterSampleMs = f.nowMs + kWaterSampleMs;
    // The nearest liquid surface around the listener (the listener's point and two rings of eight): its loop there
    // (SoundWaterType by the liquid's kind; the baked liquids have no flow speed: still water's).
    static constexpr std::array<f64, 3> kRings = {0.0, kWaterRange * 0.5, kWaterRange};
    f64 best = kWaterRange * 2.0;
    u32 entry = 0;
    glm::dvec3 at{0.0};
    for (const f64 ring : kRings) {
        const u32 points = ring == 0.0 ? 1 : 8;
        for (u32 i = 0; i < points; ++i) {
            const f64 angle = static_cast<f64>(i) * (2.0 * 3.14159265358979 / 8.0);
            const f64 x = f.listener.x + std::cos(angle) * ring;
            const f64 y = f.listener.y + std::sin(angle) * ring;
            const std::optional<physics::LiquidSample> liquid =
                world->liquidAt(x, y, f.listener.z + 60.0, f.listener.z - 60.0);
            if (!liquid) {
                continue;
            }
            const glm::dvec3 surface(x, y, static_cast<f64>(liquid->level));
            const f64 distance = glm::length(surface - f.listener);
            const u32 sound = m_data->waterSound(liquid->kind);
            if (sound != 0 && distance < best) {
                best = distance;
                entry = sound;
                at = surface;
            }
        }
        if (entry != 0) {
            break; // the inner ring's water is the nearest
        }
    }
    if (entry != m_waterEntry || (entry != 0 && !m_audio->playing(m_water))) {
        m_audio->stop(m_water, 1.0f);
        m_water = entry != 0 ? playSound(entry, Category::Ambience, kPriorityAmbience, &at, true, 1.0f)
                             : audio::VoiceHandle{};
        m_waterEntry = m_water ? entry : 0;
    } else if (m_water) {
        m_audio->setPosition(m_water, at);
    }
    m_waterAt = at;
}

// --- Deaths and later sounds ---------------------------------------------------------------------------------------

void GameAudio::deathThud(const GameAudioFrame& f, const game::Entity& unit, const glm::dvec3& at) {
    if (!game::isUnit(unit)) {
        return;
    }
    // DeathThudLookups by the body's size and the ground under it, or its splash in the water.
    bool water = false;
    if (f.collision) {
        const std::optional<physics::LiquidSample> liquid = f.collision->liquidAt(at.x, at.y, at.z + 5.0, at.z - 1.0);
        water = liquid && static_cast<f64>(liquid->level) > at.z + 0.3;
    }
    const u32 ground = m_ground.soundAt(at.x, at.y);
    const u32 sound = m_data->deathThud(unit.value(uf::UNIT_FIELD_DISPLAYID), ground != 0 ? ground : 1, water);
    playLater(sound, kThudDelayMs, at, unit.guid == f.player ? kPriorityPlayer : kPriorityWorld);
}

void GameAudio::playLater(u32 soundEntry, u32 delayMs, const glm::dvec3& at, u8 priority) {
    if (soundEntry == 0) {
        return;
    }
    for (Delayed& d : m_delayed) {
        if (d.entry == 0) {
            d = {soundEntry, m_nowMs + delayMs, at, priority};
            return;
        }
    }
}

void GameAudio::delayedSounds(const GameAudioFrame& f) {
    for (Delayed& d : m_delayed) {
        if (d.entry != 0 && reached(f.nowMs, d.atMs)) {
            if (f.world && !f.loading) {
                playSound(d.entry, Category::Effects, d.priority, &d.position);
            }
            d = {};
        }
    }
}

// --- Events, items, objects, weapons --------------------------------------------------------------------------------

void GameAudio::parityEvent(const GameAudioFrame& f, const game::GameEvent& e) {
    switch (e.kind) {
    case game::GameEventKind::ItemReceived:
        // Looted or rewarded (not made by a spell: the spell sounds): the item's pick-up sound.
        if (e.value[2] == 0) {
            playItem(e.id, true);
        }
        break;
    case game::GameEventKind::AreaExplored: // a new area: the race's exploration sound (ChrRaces)
        playSound(m_data->explorationSound(m_body.race), Category::Interface, kPriorityUi);
        break;
    case game::GameEventKind::TaxiPathLearned: playUi(UiSound::TaxiNodeDiscovered); break;
    case game::GameEventKind::UnitClicked: clickVoice(f, e.guid); break; // ours (NPC voices)
    case game::GameEventKind::LootError:
        vocalError(f, audio::vocalForLootError(net::lootErrorName(static_cast<u8>(e.value[0]))));
        break;
    case game::GameEventKind::BuyFailed:
        vocalError(f, audio::vocalForBuyError(net::buyResultName(static_cast<u8>(e.value[0]))));
        break;
    case game::GameEventKind::BankSlotFailed:
        vocalError(f, audio::vocalForBankSlotError(net::bankSlotResultName(static_cast<u32>(e.value[0]))));
        break;
    case game::GameEventKind::TaxiReply:
        if (static_cast<net::TaxiReply>(e.value[0]) == net::TaxiReply::NotEnoughMoney) {
            vocalError(f, audio::vocal::kTaxiNoMoney);
        }
        break;
    default: break;
    }
}

void GameAudio::itemSounds(const GameAudioFrame& f) {
    game::Gameplay& play = *f.play;
    // What the player puts on sounds as put down (ItemGroupSounds); the equipment arriving after the login does not.
    const game::Inventory& inventory = play.inventory();
    if (inventory.version() != m_inventoryVersion) {
        m_inventoryVersion = inventory.version();
        for (u8 slot = 0; slot < m_equipped.size(); ++slot) {
            const game::ItemInstance* item = inventory.equipped(slot);
            const u32 entry = item ? item->entry : 0;
            if (m_equippedKnown && entry != 0 && entry != m_equipped[slot]) {
                playItem(entry, false);
            }
            m_equipped[slot] = entry;
        }
        m_equippedKnown = reached(f.nowMs, m_equipQuietUntilMs);
    }
    // Item sounds waiting for their template.
    for (PendingItem& p : m_items) {
        if (p.entry == 0) {
            continue;
        }
        if (const net::ItemQueryInfo* info = play.queries().item(p.entry)) {
            const audio::ItemSounds sounds = m_data->itemSounds(info->displayId);
            const u32 sound = p.pickup ? sounds.pickup : sounds.drop;
            if (sound != 0) {
                if (!playedWithin(sound, 300)) { // a drag onto a slot is also an equip: once
                    playSound(sound, Category::Interface, kPriorityUi);
                }
            } else {
                playUi(p.pickup ? UiSound::ItemGrab : UiSound::ItemPlace);
            }
            p = {};
        } else if (reached(f.nowMs, p.sinceMs + kItemWaitMs)) {
            p = {};
        }
    }
}

void GameAudio::objectSounds(const GameAudioFrame& f) {
    if (!reached(f.nowMs, m_objectScanMs)) {
        return;
    }
    m_objectScanMs = f.nowMs + kObjectScanMs;
    for (ObjectMemory& o : m_objects) {
        o.seen = false;
    }
    // Game objects near the listener with GameObjectDisplayInfo sounds: doors and chests opening and closing (their
    // GOState), campfires, torches and machines looping while near.
    f.entities->forEach([&](const game::Entity& e) {
        if (e.type != game::ObjectType::GameObject || !e.hasPosition) {
            return;
        }
        const audio::ObjectSounds* sounds = m_data->objectSounds(e.value(uf::GAMEOBJECT_DISPLAYID));
        if (!sounds) {
            return;
        }
        const glm::dvec3 at(e.positionAt(f.nowMs));
        if (glm::length(at - f.listener) > kObjectRange) {
            return;
        }
        ObjectMemory* slot = nullptr;
        ObjectMemory* free = nullptr;
        for (ObjectMemory& o : m_objects) {
            if (o.guid == e.guid) {
                slot = &o;
                break;
            }
            if (!free && o.guid == 0) {
                free = &o;
            }
        }
        const u32 state = e.value(uf::GAMEOBJECT_STATE);
        if (slot) {
            if (state != slot->state) {
                // Phase 13: GO_STATE_ACTIVE_ALTERNATIVE (2) is the Destroy animation, with its own sound.
                const u32 sound = state == kGoStateReady ? sounds->close : state == 2 ? sounds->destroy : sounds->open;
                playSound(sound, Category::Effects, kPriorityWorld, &at);
            }
        } else if (free) {
            slot = free; // first seen: its state is not news
            *slot = {};
            slot->guid = e.guid;
        } else {
            return; // too many around: the nearest ones already sound
        }
        slot->state = state;
        slot->seen = true;
        if (sounds->ambient != 0 && !m_audio->playing(slot->loop)) {
            slot->loop = playSound(sounds->ambient, Category::Ambience, kPriorityWorld, &at, true, 0.5f);
            slot->loopEntry = sounds->ambient; // phase 14
        }
    });
    for (ObjectMemory& o : m_objects) {
        if (o.guid != 0 && !o.seen) {
            m_audio->stop(o.loop, 0.5f);
            o = {};
        }
    }
}

void GameAudio::sheatheSounds(const GameAudioFrame& f) {
    // The player drawing its weapon or putting it away (UNIT_FIELD_BYTES_2's sheath state): SheatheSoundLookups by
    // the weapon's class, subclass and material.
    const game::Entity* me = f.entities->find(f.player);
    if (!me) {
        return;
    }
    const u32 sheath = me->byteValue(uf::UNIT_FIELD_BYTES_2, 0);
    if (m_sheath == 0xFF || sheath == m_sheath) {
        m_sheath = sheath;
        return;
    }
    const bool drawing = sheath == kSheathMelee || sheath == kSheathRanged;
    const u32 hand = drawing ? sheath : m_sheath; // the weapon drawn, or the one put away
    m_sheath = sheath;
    const u16 field = hand == kSheathRanged ? uf::PLAYER_VISIBLE_ITEM_18_0 : uf::PLAYER_VISIBLE_ITEM_16_0;
    const u32 entry = me->value(field);
    const net::ItemQueryInfo* item = entry != 0 ? f.play->queries().item(entry) : nullptr;
    glm::dvec3 at;
    if (item && positionOf(f, f.player, at)) {
        playSound(m_data->sheatheSound(item->itemClass, item->subclass, static_cast<u32>(std::max(item->material, 0)),
                                       !drawing),
                  Category::Effects, kPriorityPlayer, &at);
    }
}

void GameAudio::creatureFidgets(const GameAudioFrame& f) {
    // The original plays a creature's stand and fidget sounds (CreatureSoundData) on the $FD events of its idle
    // animations' variations (phase 14: app/GameAudioModels.cpp, from the actors' events); without them (no actors:
    // the tests), now and then one idle creature near the listener, at random (reservoir sampling, nothing
    // allocated), says one of them.
    if (!reached(f.nowMs, m_fidgetAtMs)) {
        return;
    }
    m_fidgetAtMs = f.nowMs + kFidgetMinMs + random() % (kFidgetMaxMs - kFidgetMinMs);
    const game::Entity* pick = nullptr;
    const audio::CreatureSounds* pickSounds = nullptr;
    u32 candidates = 0;
    f.entities->forEach([&](const game::Entity& e) {
        if (e.type != game::ObjectType::Unit || !e.hasPosition || e.moving || game::isDead(e) || game::inCombat(e)) {
            return;
        }
        const audio::CreatureSounds* sounds = soundsOf(e);
        if (!sounds || (sounds->stand == 0 && sounds->fidget == std::array<u32, 4>{})) {
            return;
        }
        if (glm::length(glm::dvec3(e.positionAt(f.nowMs)) - f.listener) > kFidgetRange) {
            return;
        }
        if (random() % ++candidates == 0) {
            pick = &e;
            pickSounds = sounds;
        }
    });
    if (!pick) {
        return;
    }
    std::array<u32, 5> pool{};
    u32 count = 0;
    for (const u32 id : pickSounds->fidget) {
        if (id != 0 && m_data->entry(id)) { // a few columns name rows the client does not have
            pool[count++] = id;
        }
    }
    if (pickSounds->stand != 0 && m_data->entry(pickSounds->stand)) {
        pool[count++] = pickSounds->stand;
    }
    if (count > 0) {
        const glm::dvec3 at(pick->positionAt(f.nowMs));
        playSound(pool[random() % count], Category::Voices, kPriorityWorld, &at);
    }
}

void GameAudio::socialSounds(const GameAudioFrame& f) {
    // StaticPopup's sound of PARTY_INVITE and DUEL_REQUESTED: the question appears.
    const game::Social& social = f.play->social();
    const bool invite = !social.group().pendingInviter.empty();
    const bool duel = social.duel().phase == game::DuelPhase::Challenged;
    if ((invite && !m_invitePending) || (duel && !m_duelPending)) {
        playUi(UiSound::PlayerInvite);
    }
    m_invitePending = invite;
    m_duelPending = duel;
}

} // namespace vd

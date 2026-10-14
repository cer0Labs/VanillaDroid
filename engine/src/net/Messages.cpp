#include "net/Messages.h"

namespace vd::net {
namespace {

Position readPosition(ByteReader& r) {
    Position p;
    p.x = r.f32v();
    p.y = r.f32v();
    p.z = r.f32v();
    p.o = r.f32v();
    return p;
}

void writePosition(ByteWriter& w, const Position& p) {
    w.f32v(p.x).f32v(p.y).f32v(p.z).f32v(p.o);
}

} // namespace

MovementInfo readMovementInfo(ByteReader& r) {
    MovementInfo info;
    info.flags = r.u32v();
    info.time = r.u32v();
    info.position = readPosition(r);
    if (info.flags & kMoveFlagOnTransport) {
        info.transportGuid = r.u64v(); // ObjectGuid: the full 64 bits
        info.transportPosition = readPosition(r);
    }
    if (info.flags & kMoveFlagSwimming) {
        info.pitch = r.f32v();
    }
    info.fallTime = r.u32v();
    if (info.flags & kMoveFlagJumping) {
        info.jumpVelocity = r.f32v(); // zspeed
        info.jumpCos = r.f32v();
        info.jumpSin = r.f32v();
        info.jumpXySpeed = r.f32v();
    }
    if (info.flags & kMoveFlagSplineElevation) {
        info.splineElevation = r.f32v();
    }
    return info;
}

void writeMovementInfo(ByteWriter& w, const MovementInfo& info) {
    w.u32v(info.flags).u32v(info.time);
    writePosition(w, info.position);
    if (info.flags & kMoveFlagOnTransport) {
        w.u64v(info.transportGuid);
        writePosition(w, info.transportPosition);
    }
    if (info.flags & kMoveFlagSwimming) {
        w.f32v(info.pitch);
    }
    w.u32v(info.fallTime);
    if (info.flags & kMoveFlagJumping) {
        w.f32v(info.jumpVelocity).f32v(info.jumpCos).f32v(info.jumpSin).f32v(info.jumpXySpeed);
    }
    if (info.flags & kMoveFlagSplineElevation) {
        w.f32v(info.splineElevation);
    }
}

std::optional<AuthResponse> parseAuthResponse(std::span<const u8> payload) {
    ByteReader r(payload);
    AuthResponse response;
    response.result = r.u8v();
    if (r.remaining() >= 4) {
        response.queuePosition = r.u32v();
    }
    if (r.failed()) {
        return std::nullopt;
    }
    return response;
}

std::optional<std::vector<CharacterInfo>> parseCharEnum(std::span<const u8> payload) {
    ByteReader r(payload);
    const u8 count = r.u8v();
    std::vector<CharacterInfo> characters;
    characters.reserve(count);
    for (u32 i = 0; i < count && r.ok(); ++i) {
        CharacterInfo c;
        c.guid = r.u64v();
        c.name = std::string(r.cstring());
        c.race = r.u8v();
        c.cls = r.u8v();
        c.gender = r.u8v();
        c.skin = r.u8v();
        c.face = r.u8v();
        c.hairStyle = r.u8v();
        c.hairColor = r.u8v();
        c.facialHair = r.u8v();
        c.level = r.u8v();
        c.zone = r.u32v();
        c.map = r.u32v();
        c.x = r.f32v();
        c.y = r.f32v();
        c.z = r.f32v();
        c.guild = r.u32v();
        c.flags = r.u32v();
        c.firstLogin = r.u8v() != 0;
        c.petDisplayId = r.u32v();
        c.petLevel = r.u32v();
        c.petFamily = r.u32v();
        for (u32 slot = 0; slot < 20; ++slot) {
            c.equipmentDisplay[slot] = r.u32v();
            c.equipmentType[slot] = r.u8v();
        }
        characters.push_back(std::move(c));
    }
    if (r.failed()) {
        return std::nullopt;
    }
    return characters;
}

std::optional<LoginVerifyWorld> parseLoginVerifyWorld(std::span<const u8> payload) {
    ByteReader r(payload);
    LoginVerifyWorld login;
    login.map = r.u32v();
    login.position = readPosition(r);
    if (r.failed()) {
        return std::nullopt;
    }
    return login;
}

std::optional<ItemQueryInfo> parseItemQueryResponse(std::span<const u8> payload) {
    ByteReader r(payload);
    ItemQueryInfo item;
    const u32 entry = r.u32v();
    if (r.failed()) {
        return std::nullopt;
    }
    item.entry = entry & 0x7FFFFFFFu;
    if (entry & 0x80000000u) {
        return item; // unknown to the server
    }
    item.found = true;
    item.itemClass = r.u32v();
    item.subclass = r.u32v();
    item.name = std::string(r.cstring());
    for (u32 i = 0; i < 3; ++i) {
        r.cstring(); // name2..4, always empty
    }
    item.displayId = r.u32v();
    item.quality = r.u32v();
    item.flags = r.u32v();
    item.buyPrice = r.u32v();
    item.sellPrice = r.u32v();
    item.inventoryType = r.u32v();
    item.allowableClass = r.u32v();
    item.allowableRace = r.u32v();
    item.itemLevel = r.u32v();
    item.requiredLevel = r.u32v();
    item.requiredSkill = r.u32v();
    item.requiredSkillRank = r.u32v();
    item.requiredSpell = r.u32v();
    item.requiredHonorRank = r.u32v();
    item.requiredCityRank = r.u32v();
    item.requiredReputationFaction = r.u32v();
    item.requiredReputationRank = r.u32v(); // 0 unless a faction is set
    item.maxCount = r.u32v();
    item.stackable = r.u32v();
    item.containerSlots = r.u32v();
    for (ItemQueryInfo::Stat& stat : item.stats) {
        stat.type = r.u32v();
        stat.value = r.i32v();
    }
    for (ItemQueryInfo::Damage& damage : item.damages) {
        damage.min = r.f32v();
        damage.max = r.f32v();
        damage.school = r.u32v();
    }
    item.armor = r.i32v();
    for (i32& resistance : item.resistances) {
        resistance = r.i32v();
    }
    item.delayMs = r.u32v();
    item.ammoType = r.u32v();
    item.rangedModRange = r.f32v();
    for (ItemQueryInfo::Spell& spell : item.spells) {
        // A spell the server does not know comes as 0, 0, 0, -1, 0, -1.
        spell.id = r.u32v();
        spell.trigger = r.u32v();
        spell.charges = r.i32v();
        spell.cooldownMs = r.i32v();
        spell.category = r.u32v();
        spell.categoryCooldownMs = r.i32v();
    }
    item.bonding = r.u32v();
    item.description = std::string(r.cstring());
    item.pageText = r.u32v();
    item.language = r.u32v();
    item.pageMaterial = r.u32v();
    item.startQuest = r.u32v();
    item.lockId = r.u32v();
    item.material = r.i32v();
    item.sheath = r.u32v();
    item.randomProperty = r.u32v();
    item.block = r.u32v();
    item.itemSet = r.u32v();
    item.maxDurability = r.u32v();
    item.area = r.u32v();
    item.map = r.u32v();
    item.bagFamily = r.u32v();
    if (r.failed()) {
        return std::nullopt;
    }
    return item;
}

std::optional<NameQueryInfo> parseNameQueryResponse(std::span<const u8> payload) {
    ByteReader r(payload);
    NameQueryInfo info;
    info.guid = r.u64v();
    info.name = std::string(r.cstring());
    r.cstring(); // realm name
    info.race = r.u32v();
    info.gender = r.u32v();
    info.cls = r.u32v();
    if (r.failed()) {
        return std::nullopt;
    }
    return info;
}

std::optional<CreatureQueryInfo> parseCreatureQueryResponse(std::span<const u8> payload) {
    ByteReader r(payload);
    CreatureQueryInfo info;
    const u32 entry = r.u32v();
    if (r.failed()) {
        return std::nullopt;
    }
    info.entry = entry & 0x7FFFFFFFu;
    if (entry & 0x80000000u) {
        return info;
    }
    info.found = true;
    info.name = std::string(r.cstring());
    for (u32 i = 0; i < 3; ++i) {
        r.cstring(); // name2..4
    }
    info.subname = std::string(r.cstring());
    info.typeFlags = r.u32v();
    info.type = r.u32v();
    info.family = r.u32v();
    info.rank = r.u32v();
    if (r.failed()) {
        return std::nullopt;
    }
    // Phase 15 (pets): the model (a stabled pet's, PetStable's SetPetStablePaperdoll): after a reserved u32
    // and the pet spell list, display_id[0] (an older, shorter answer leaves it 0).
    if (r.remaining() >= 12) {
        r.u32v();
        r.u32v();
        info.displayId = r.u32v();
    }
    return info;
}

} // namespace vd::net

#include "game/SpellKitFx.h"

#include "core/Hash.h"
#include "core/Log.h"
#include "db/DbcView.h"
#include "fx/EffectSet.h"
#include "pack/PackReader.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>

namespace vd::game {
namespace {

// Field numbers of the 1.12.1 (5875) files.
// SpellVisualKit.dbc: 14 camera shake (SpellEffectCameraShakes), 15-18 procs, 19-34 their parameters.
constexpr u32 kKitShake = 14, kKitProcs = 15, kKitParams = 19, kKitFields = 35;
// SpellChainEffects.dbc: id, avgSegLen, width, noiseScale, texCoordScale, segDuration (ms), segDelay (ms),
// texture.
constexpr u32 kChainSegment = 1, kChainWidth = 2, kChainNoise = 3, kChainScroll = 4, kChainDuration = 5,
              kChainDelay = 6, kChainTexture = 7, kChainFields = 8;
// CameraShakes.dbc: id, type, direction, amplitude, frequency, duration, phase, coefficient.
constexpr u32 kShakeFields = 8;
// SpellEffectCameraShakes.dbc: id, three CameraShakes.
constexpr u32 kEffectShakeFields = 4;
// SpellVisualPrecastTransitions.dbc: id, load and hold animation names (AnimationData.dbc: id, name, ...).
constexpr u32 kTransitionLoad = 1, kTransitionHold = 2, kTransitionFields = 3;
constexpr u32 kAnimationName = 1, kAnimationFields = 2;
// CreatureModelData.dbc: model path 2, FootstepShakeSize 11, DeathThudShakeSize 12 (CameraShakes ids).
constexpr u32 kModelPath = 2, kModelFootstepShake = 11, kModelDeathShake = 12, kModelFields = 16;

// CGCamera::AddShake (WoW.exe 0x511D40) scales the DBC's amplitude by 1/36.
constexpr f32 kAmplitudeScale = 1.0f / 36.0f;

std::optional<db::DbcView> table(const pack::PackReader& db, const char* name, u32 fields) {
    const std::optional<pack::PackAsset> asset = db.find(std::string("DBFilesClient\\") + name + ".dbc");
    if (!asset) {
        VD_LOG_WARN("%s.dbc missing: no spell effects from it", name);
        return std::nullopt;
    }
    Result<db::DbcView> parsed = db::DbcView::parse(asset->data, asset->size);
    if (!parsed || parsed.value().fieldCount() < fields) {
        VD_LOG_WARN("%s.dbc: unexpected layout, no spell effects from it", name);
        return std::nullopt;
    }
    return parsed.value();
}

// The client converts the float parameters with _ftol (truncation) or the +512 trick (rounding to a byte).
u32 truncated(f32 v) {
    return v > 0.0f && v < 4294967040.0f ? static_cast<u32>(v) : 0;
}
u32 roundedByte(f32 v) {
    return static_cast<u32>(std::clamp(std::lround(v), 0l, 255l));
}

bool sameName(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (usize i = 0; i < a.size(); ++i) {
        const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
        if (lower(a[i]) != lower(b[i])) {
            return false;
        }
    }
    return true;
}

} // namespace

CameraShake cameraShakeFromRow(u32 id, u32 type, u32 direction, f32 amplitude, f32 frequency, f32 duration,
                               f32 phase, f32 coefficient) {
    CameraShake s;
    s.id = id;
    s.decays = type == 1;
    s.direction = direction == 0   ? CameraShake::Direction::Forward
                  : direction == 1 ? CameraShake::Direction::Side
                                   : CameraShake::Direction::Up;
    s.amplitude = amplitude * kAmplitudeScale;
    s.frequency = frequency;
    s.duration = duration;
    s.phase = phase;
    s.coefficient = coefficient;
    return s;
}

KitFx kitFxFromProcs(u32 kitId, const std::array<i32, 4>& procs, const std::array<f32, 16>& params,
                     u32 shake) {
    KitFx fx;
    fx.kit = kitId;
    fx.shake = shake;
    for (u32 i = 0; i < 4; ++i) {
        const f32 p0 = params[i], p1 = params[4 + i], p2 = params[8 + i];
        switch (procs[i]) {
        case static_cast<i32>(SpellKitFx::kProcChain):
        case static_cast<i32>(SpellKitFx::kProcCastChain):
            // WoW.exe 0x60DA79 -> 0x6ECB90: the chain effect, how many beams (at most 3) and whether the kit
            // holds them (a channel) or each jump fades on its own.
            if (roundedByte(p0) != 0 && roundedByte(p1) != 0) {
                fx.chainEffect = roundedByte(p0);
                fx.chainBeams = static_cast<u8>(std::min(3u, roundedByte(p1)));
                fx.chainHeld = roundedByte(p2) != 0;
            }
            break;
        case static_cast<i32>(SpellKitFx::kProcTint):
            // 0x60D840: the color goes on the unit's list of kit colors; the newest colors its model (only
            // the first parameter is read).
            fx.tints = true;
            fx.tint = truncated(p0) & 0xFFFFFF;
            break;
        case static_cast<i32>(SpellKitFx::kProcFlash):
            // 0x60D8DF -> 0x60CAA0: the color for p1 seconds, then back to the model's own over p2 seconds.
            fx.flashes = true;
            fx.flashColor = truncated(p0) & 0xFFFFFF;
            fx.flashHoldSeconds = std::max(0.0f, p1);
            fx.flashFadeSeconds = std::max(0.0f, p2);
            break;
        case static_cast<i32>(SpellKitFx::kProcLine): fx.line = true; break;
        default: break;
        }
    }
    return fx;
}

bool SpellKitFx::init(const pack::PackReader& db) {
    *this = SpellKitFx{};
    if (const std::optional<db::DbcView> kits = table(db, "SpellVisualKit", kKitFields)) {
        for (u32 r = 0; r < kits->recordCount(); ++r) {
            std::array<i32, 4> procs{};
            std::array<f32, 16> params{};
            for (u32 i = 0; i < 4; ++i) {
                procs[i] = kits->getI32(r, kKitProcs + i);
            }
            for (u32 p = 0; p < 16; ++p) {
                params[p] = kits->getF32(r, kKitParams + p);
            }
            const KitFx fx = kitFxFromProcs(kits->getU32(r, 0), procs, params, kits->getU32(r, kKitShake));
            if (fx.chainEffect != 0 || fx.tints || fx.flashes || fx.line || fx.shake != 0) {
                addKit(fx);
            }
        }
    }
    if (const std::optional<db::DbcView> chains = table(db, "SpellChainEffects", kChainFields)) {
        for (u32 r = 0; r < chains->recordCount(); ++r) {
            ChainEffect c;
            c.id = chains->getU32(r, 0);
            c.segmentLength = std::max(0.1f, chains->getF32(r, kChainSegment));
            c.width = chains->getF32(r, kChainWidth);
            c.noise = chains->getF32(r, kChainNoise);
            c.scroll = chains->getF32(r, kChainScroll);
            c.segmentMs = chains->getU32(r, kChainDuration);
            c.delayMs = chains->getU32(r, kChainDelay);
            const std::string_view texture = chains->getString(r, kChainTexture);
            c.texture = texture.empty() ? 0 : assetPathHash(normalizeAssetPath(texture));
            addChainEffect(c);
        }
    }
    if (const std::optional<db::DbcView> shakes = table(db, "CameraShakes", kShakeFields)) {
        for (u32 r = 0; r < shakes->recordCount(); ++r) {
            addCameraShake(cameraShakeFromRow(
                shakes->getU32(r, 0), shakes->getU32(r, 1), shakes->getU32(r, 2), shakes->getF32(r, 3),
                shakes->getF32(r, 4), shakes->getF32(r, 5), shakes->getF32(r, 6), shakes->getF32(r, 7)));
        }
    }
    if (const std::optional<db::DbcView> effects = table(db, "SpellEffectCameraShakes", kEffectShakeFields)) {
        for (u32 r = 0; r < effects->recordCount(); ++r) {
            addEffectShakes(effects->getU32(r, 0),
                            {effects->getU32(r, 1), effects->getU32(r, 2), effects->getU32(r, 3)});
        }
    }
    // The precast transitions name AnimationData rows (WoW.exe never reads this table: the same pairs are its
    // animation-finished switch at 0x5FC3F0, LoadBow -> HoldBow...).
    const std::optional<db::DbcView> animations = table(db, "AnimationData", kAnimationFields);
    const std::optional<db::DbcView> transitions =
        table(db, "SpellVisualPrecastTransitions", kTransitionFields);
    if (animations && transitions) {
        const auto animationId = [&](std::string_view name) -> std::optional<u16> {
            for (u32 r = 0; r < animations->recordCount(); ++r) {
                if (sameName(animations->getString(r, kAnimationName), name)) {
                    return static_cast<u16>(animations->getU32(r, 0));
                }
            }
            return std::nullopt;
        };
        for (u32 r = 0; r < transitions->recordCount(); ++r) {
            const std::optional<u16> load = animationId(transitions->getString(r, kTransitionLoad));
            const std::optional<u16> hold = animationId(transitions->getString(r, kTransitionHold));
            if (load && hold) {
                addTransition(*load, *hold);
            }
        }
    }
    if (const std::optional<db::DbcView> models = table(db, "CreatureModelData", kModelFields)) {
        for (u32 r = 0; r < models->recordCount(); ++r) {
            const u32 footstep = models->getU32(r, kModelFootstepShake);
            const u32 death = models->getU32(r, kModelDeathShake);
            const std::string_view path = models->getString(r, kModelPath);
            if ((footstep != 0 || death != 0) && !path.empty()) {
                addModelShakes(fx::effectModelHash(path), footstep, death);
            }
        }
    }
    m_ready = !m_kits.empty();
    VD_LOG_INFO("Spell kit effects: %zu kits, %zu chain effects, %zu camera shakes, %zu precast transitions, "
                "%zu creature models that shake",
                m_kits.size(), m_chains.size(), m_shakes.size(), m_transitions.size(), m_modelShakes.size());
    return m_ready;
}

const KitFx* SpellKitFx::kit(u32 id) const {
    const auto it = id == 0 ? m_kits.end() : m_kits.find(id);
    return it == m_kits.end() ? nullptr : &it->second;
}

const ChainEffect* SpellKitFx::chainEffect(u32 id) const {
    const auto it = id == 0 ? m_chains.end() : m_chains.find(id);
    return it == m_chains.end() ? nullptr : &it->second;
}

const CameraShake* SpellKitFx::cameraShake(u32 id) const {
    const auto it = id == 0 ? m_shakes.end() : m_shakes.find(id);
    return it == m_shakes.end() ? nullptr : &it->second;
}

std::array<u32, 3> SpellKitFx::effectShakes(u32 id) const {
    const auto it = id == 0 ? m_effectShakes.end() : m_effectShakes.find(id);
    return it == m_effectShakes.end() ? std::array<u32, 3>{} : it->second;
}

u16 SpellKitFx::holdAfter(u16 load) const {
    for (const auto& [from, hold] : m_transitions) {
        if (from == load) {
            return hold;
        }
    }
    return kNoAnimation;
}

u32 SpellKitFx::footstepShake(u64 model) const {
    const auto it = m_modelShakes.find(model);
    return it == m_modelShakes.end() ? 0 : it->second.footstep;
}

u32 SpellKitFx::deathShake(u64 model) const {
    const auto it = m_modelShakes.find(model);
    return it == m_modelShakes.end() ? 0 : it->second.death;
}

void SpellKitFx::addKit(const KitFx& kit) {
    m_kits[kit.kit] = kit;
    m_ready = true;
}

void SpellKitFx::addChainEffect(const ChainEffect& chain) {
    m_chains[chain.id] = chain;
}

void SpellKitFx::addCameraShake(const CameraShake& shake) {
    m_shakes[shake.id] = shake;
}

void SpellKitFx::addEffectShakes(u32 id, const std::array<u32, 3>& shakes) {
    m_effectShakes[id] = shakes;
}

void SpellKitFx::addTransition(u16 load, u16 hold) {
    m_transitions.emplace_back(load, hold);
}

void SpellKitFx::addModelShakes(u64 model, u32 footstep, u32 death) {
    m_modelShakes[model] = {footstep, death};
}

} // namespace vd::game

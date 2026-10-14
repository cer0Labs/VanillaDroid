#include "world/Lighting.h"

#include "core/Log.h"
#include "pack/PackReader.h"
#include "pack/TerrainFormat.h"
#include "world/Celestial.h"

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace vd {
namespace {

// 1.12.1 (5875) layouts, verified against the client's tables.
namespace light {
constexpr u32 kMap = 1;
constexpr u32 kX = 2; // placement axes x 36: (kMapHalfSize - Y, Z, kMapHalfSize - X)
constexpr u32 kHeight = 3;
constexpr u32 kZ = 4;
constexpr u32 kInner = 5; // x 36
constexpr u32 kOuter = 6;
constexpr u32 kParamsClear = 7; // then clear underwater, storm, storm underwater, death
} // namespace light
namespace params {
constexpr u32 kHighlightSky = 1;
constexpr u32 kSkybox = 2; // LightSkybox.dbc; field 3 is always 0
constexpr u32 kGlow = 4;
constexpr u32 kWaterShallowAlpha = 5; // then water deep, ocean shallow, ocean deep
} // namespace params
namespace skybox {
constexpr u32 kModel = 1; // string
} // namespace skybox
namespace band {
constexpr u32 kCount = 1;
constexpr u32 kTimes = 2;  // 16 entries, half-minutes
constexpr u32 kValues = 18; // 16 entries
} // namespace band
constexpr f32 kDbcUnits = 36.0f; // Light.dbc distances are in 1/36 yard
constexpr f32 kMaxFogBehind = 4.0f; // fog start no further behind the camera than 4 fog ends (the tables reach 2)

// LightIntBand order per LightParams (18 bands).
constexpr u32 kDirect = 0;
constexpr u32 kAmbient = 1;
constexpr u32 kSky0 = 2; // .. kSky0 + 4
constexpr u32 kFog = 7;
constexpr u32 kShadow = 8;
constexpr u32 kSun = 9;
constexpr u32 kSunHalo = 10;
constexpr u32 kCloudEmissive = 11;
constexpr u32 kCloudLayer1 = 12;
constexpr u32 kCloudLayer2 = 13;
constexpr u32 kOceanClose = 14;
constexpr u32 kOceanFar = 15;
constexpr u32 kRiverClose = 16;
constexpr u32 kRiverFar = 17;
// LightFloatBand order (6 bands).
constexpr u32 kFogEnd = 0;
constexpr u32 kFogScale = 1;
constexpr u32 kCelestialGlow = 2;
constexpr u32 kCloudDensity = 3;

constexpr u32 kAbsent = ~0u;

Result<db::DbcView> openTable(const pack::PackReader& dbPack, const char* path) {
    const std::optional<pack::PackAsset> asset = dbPack.find(path);
    if (!asset) {
        return Error{std::string("Missing ") + path};
    }
    return db::DbcView::parse(asset->data, asset->size);
}

std::vector<u32> indexById(const db::DbcView& table) {
    u32 maxId = 0;
    for (u32 r = 0; r < table.recordCount(); ++r) {
        maxId = std::max(maxId, table.getU32(r, 0));
    }
    std::vector<u32> index(maxId + 1, kAbsent);
    for (u32 r = 0; r < table.recordCount(); ++r) {
        index[table.getU32(r, 0)] = r;
    }
    return index;
}

glm::vec3 unpackColor(u32 rgb) {
    return glm::vec3(static_cast<f32>((rgb >> 16) & 0xFF), static_cast<f32>((rgb >> 8) & 0xFF),
                     static_cast<f32>(rgb & 0xFF)) /
           255.0f;
}

// Keyframes a, b around `time` and the blend factor between them, wrapping at midnight. False for an
// empty band.
bool sampleBand(const db::DbcView& table, u32 record, f32 time, f32& outT, u32& a, u32& b) {
    const u32 count = std::min<u32>(table.getU32(record, band::kCount), 16);
    if (count == 0) {
        return false;
    }
    a = count - 1;
    for (u32 k = 0; k < count; ++k) {
        if (static_cast<f32>(table.getU32(record, band::kTimes + k)) <= time) {
            a = k;
        }
    }
    b = (a + 1) % count;
    const f32 ta = static_cast<f32>(table.getU32(record, band::kTimes + a));
    f32 tb = static_cast<f32>(table.getU32(record, band::kTimes + b));
    f32 t = time;
    if (tb <= ta) {
        tb += WorldLighting::kDayLength; // wraps past midnight
    }
    if (t < ta) {
        t += WorldLighting::kDayLength;
    }
    outT = tb > ta ? (t - ta) / (tb - ta) : 0.0f;
    return true;
}

} // namespace

Result<void> WorldLighting::init(const pack::PackReader& dbPack) {
    Result<db::DbcView> lights = openTable(dbPack, "DBFilesClient\\Light.dbc");
    Result<db::DbcView> params = openTable(dbPack, "DBFilesClient\\LightParams.dbc");
    Result<db::DbcView> intBand = openTable(dbPack, "DBFilesClient\\LightIntBand.dbc");
    Result<db::DbcView> floatBand = openTable(dbPack, "DBFilesClient\\LightFloatBand.dbc");
    if (!lights || !params || !intBand || !floatBand) {
        return Error{"Light tables missing from db.vdpk"};
    }
    m_light = lights.value();
    m_params = params.value();
    m_intBand = intBand.value();
    m_floatBand = floatBand.value();
    m_paramsIndex = indexById(m_params);
    m_intIndex = indexById(m_intBand);
    m_floatIndex = indexById(m_floatBand);
    if (Result<db::DbcView> skyboxes = openTable(dbPack, "DBFilesClient\\LightSkybox.dbc")) {
        m_skybox = skyboxes.value();
    }
    m_ready = true;
    return {};
}

std::string_view WorldLighting::skyboxModel(u32 skybox) const {
    if (skybox == 0 || m_skybox.fieldCount() <= skybox::kModel) {
        return {};
    }
    for (u32 r = 0; r < m_skybox.recordCount(); ++r) {
        if (m_skybox.getU32(r, 0) == skybox) {
            return m_skybox.getString(r, skybox::kModel);
        }
    }
    return {};
}

void WorldLighting::setMap(u32 mapId) {
    m_lights.clear();
    m_globalParams = {};
    if (!m_ready) {
        return;
    }
    for (u32 r = 0; r < m_light.recordCount(); ++r) {
        if (m_light.getU32(r, light::kMap) != mapId) {
            continue;
        }
        const f32 x = m_light.getF32(r, light::kX) / kDbcUnits;
        const f32 height = m_light.getF32(r, light::kHeight) / kDbcUnits;
        const f32 z = m_light.getF32(r, light::kZ) / kDbcUnits;
        const f32 outer = m_light.getF32(r, light::kOuter) / kDbcUnits;
        std::array<u32, 5> params{};
        for (u32 i = 0; i < params.size(); ++i) {
            params[i] = m_light.getU32(r, light::kParamsClear + i);
        }
        if (x == 0.0f && z == 0.0f && outer == 0.0f) {
            m_globalParams = params; // the map-wide default
            continue;
        }
        Light l;
        l.xy = glm::vec2(pack::kMapHalfSize - z, pack::kMapHalfSize - x);
        l.z = height;
        l.inner = m_light.getF32(r, light::kInner) / kDbcUnits;
        l.outer = outer;
        l.params = params;
        m_lights.push_back(l);
    }
    VD_LOG_INFO("Lighting: map %u, global params %u, %zu local lights", mapId, m_globalParams[kClear],
                m_lights.size());
}

WorldLighting::Bands WorldLighting::evaluateParams(u32 paramsId, f32 time) const {
    Bands bands{};
    bands.floats = {500.0f * kDbcUnits, 0.25f, 1.0f, 0.5f, 0.95f, 1.0f};
    bands.waterAlphas = {0.5f, 1.0f, 0.75f, 1.0f};
    for (u32 i = 0; i < 18; ++i) {
        const u32 id = paramsId * 18 - 17 + i;
        if (paramsId == 0 || id >= m_intIndex.size() || m_intIndex[id] == kAbsent) {
            continue;
        }
        const u32 record = m_intIndex[id];
        f32 t = 0.0f;
        u32 a = 0, b = 0;
        if (sampleBand(m_intBand, record, time, t, a, b)) {
            const glm::vec3 ca = unpackColor(m_intBand.getU32(record, band::kValues + a));
            const glm::vec3 cb = unpackColor(m_intBand.getU32(record, band::kValues + b));
            bands.colors[i] = glm::mix(ca, cb, t);
        }
    }
    for (u32 i = 0; i < 6; ++i) {
        const u32 id = paramsId * 6 - 5 + i;
        if (paramsId == 0 || id >= m_floatIndex.size() || m_floatIndex[id] == kAbsent) {
            continue;
        }
        const u32 record = m_floatIndex[id];
        f32 t = 0.0f;
        u32 a = 0, b = 0;
        if (sampleBand(m_floatBand, record, time, t, a, b)) {
            const f32 va = m_floatBand.getF32(record, band::kValues + a);
            const f32 vb = m_floatBand.getF32(record, band::kValues + b);
            bands.floats[i] = va + (vb - va) * t;
        }
    }
    if (paramsId < m_paramsIndex.size() && m_paramsIndex[paramsId] != kAbsent) {
        for (u32 i = 0; i < 4; ++i) {
            bands.waterAlphas[i] = m_params.getF32(m_paramsIndex[paramsId], params::kWaterShallowAlpha + i);
        }
    }
    return bands;
}

u32 WorldLighting::dominantParams(const std::array<u32, 5>& params, const LightConditions& conditions) {
    if (conditions.dead) {
        return params[kDeath];
    }
    const bool storm = conditions.storm >= 0.5f;
    if (conditions.underwater) {
        return params[storm ? kStormUnderwater : kClearUnderwater];
    }
    return params[storm ? kStorm : kClear];
}

WorldLighting::Bands WorldLighting::evaluateLight(const std::array<u32, 5>& params, f32 time,
                                                  const LightConditions& conditions) const {
    if (conditions.dead) {
        return evaluateParams(params[kDeath], time);
    }
    Bands bands = evaluateParams(params[conditions.underwater ? kClearUnderwater : kClear], time);
    const f32 storm = std::clamp(conditions.storm, 0.0f, 1.0f);
    if (storm > 0.0f) {
        blend(bands, evaluateParams(params[conditions.underwater ? kStormUnderwater : kStorm], time), storm);
    }
    return bands;
}

void WorldLighting::blend(Bands& into, const Bands& from, f32 weight) {
    for (u32 i = 0; i < into.colors.size(); ++i) {
        into.colors[i] = glm::mix(into.colors[i], from.colors[i], weight);
    }
    for (u32 i = 0; i < into.floats.size(); ++i) {
        into.floats[i] += (from.floats[i] - into.floats[i]) * weight;
    }
    for (u32 i = 0; i < into.waterAlphas.size(); ++i) {
        into.waterAlphas[i] += (from.waterAlphas[i] - into.waterAlphas[i]) * weight;
    }
}

LightingState WorldLighting::evaluate(const glm::dvec3& position, f32 timeOfDay,
                                      const LightConditions& conditions) const {
    LightingState state;
    const f32 time = std::fmod(std::fmod(timeOfDay, kDayLength) + kDayLength, kDayLength);
    Bands bands = evaluateLight(m_globalParams, time, conditions);
    u32 dominant = dominantParams(m_globalParams, conditions);
    f32 dominantWeight = 0.5f; // a local light takes over the skybox and the glow once it weighs more
    const glm::vec3 p(static_cast<f32>(position.x), static_cast<f32>(position.y), static_cast<f32>(position.z));
    for (const Light& l : m_lights) {
        const f32 distance = glm::length(glm::vec3(l.xy, l.z) - p);
        if (distance >= l.outer) {
            continue;
        }
        const f32 weight =
            distance <= l.inner || l.outer <= l.inner ? 1.0f : 1.0f - (distance - l.inner) / (l.outer - l.inner);
        blend(bands, evaluateLight(l.params, time, conditions), weight);
        if (weight > dominantWeight) {
            dominantWeight = weight;
            dominant = dominantParams(l.params, conditions);
        }
    }

    state.direct = bands.colors[kDirect];
    state.ambient = bands.colors[kAmbient];
    for (u32 i = 0; i < 5; ++i) {
        state.sky[i] = bands.colors[kSky0 + i];
    }
    state.fog = bands.colors[kFog];
    state.shadow = bands.colors[kShadow];
    state.sun = bands.colors[kSun];
    state.sunHalo = bands.colors[kSunHalo];
    state.cloudEmissive = bands.colors[kCloudEmissive];
    state.cloudLayer1 = bands.colors[kCloudLayer1];
    state.cloudLayer2 = bands.colors[kCloudLayer2];
    state.riverClose = bands.colors[kRiverClose];
    state.riverFar = bands.colors[kRiverFar];
    state.oceanClose = bands.colors[kOceanClose];
    state.oceanFar = bands.colors[kOceanFar];
    state.fogEnd = bands.floats[kFogEnd] / kDbcUnits;
    // Phase 11: negative in 241 of the client's LightParams (124 of its 134 underwater sets, foggy zones): the fog
    // starts behind the camera, so even what is next to it is tinted (-0.5 under water: a third of the fog at once).
    state.fogStartScale = std::clamp(bands.floats[kFogScale], -kMaxFogBehind, 0.99f);
    state.celestialGlow = bands.floats[kCelestialGlow];
    state.cloudDensity = bands.floats[kCloudDensity];
    state.riverShallowAlpha = bands.waterAlphas[0];
    state.riverDeepAlpha = bands.waterAlphas[1];
    state.oceanShallowAlpha = bands.waterAlphas[2];
    state.oceanDeepAlpha = bands.waterAlphas[3];
    if (dominant < m_paramsIndex.size() && m_paramsIndex[dominant] != kAbsent) {
        const u32 record = m_paramsIndex[dominant];
        state.highlightSky = m_params.getU32(record, params::kHighlightSky) != 0;
        state.skybox = m_params.getU32(record, params::kSkybox);
        state.glow = m_params.getF32(record, params::kGlow);
    }

    // Phase 11: the light's direction of WoW.exe 1.12 (world/Celestial.h): from the north-west, 20 to 37
    // degrees high, day and night. The client does not read World\dnc.db.
    state.lightDirection = celestial::lightDirection(time / kDayLength);
    return state;
}

} // namespace vd

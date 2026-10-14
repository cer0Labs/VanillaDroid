#include "world/Sky.h"

#include "core/DevEnv.h"
#include "core/Hash.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "pack/PackFormat.h"
#include "physics/CollisionWorld.h"
#include "world/Celestial.h"
#include "world/WorldObjects.h"

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace vd {
namespace {

// The cloud noise (sky_clouds.frag): kNoiseSize square, tiling, two independent fields in r and g, each
// octaves of value noise (lattice values in -1..1, kNoiseCells cells across at the first octave, every octave
// twice the cells and half the amplitude) stored as 128 + 64 * n, as WoW.exe's CPU clouds.
constexpr u32 kNoiseSize = 256;
constexpr u32 kNoiseCells = 4;
constexpr u32 kNoiseOctaves = 5;
constexpr f32 kCloudSharpness = 0.96f; // WoW.exe's
// How fast the two layers drift (texture tiles per second): slowly, and not together, so their sum changes
// shape.
constexpr glm::vec2 kCloudDrift1(0.0030f, 0.0011f);
constexpr glm::vec2 kCloudDrift2(0.0017f, -0.0026f);
// The glare's occlusion: a ray a few times a second, never farther than the collision world reaches, in legs
// of kOcclusionStep (one per frame); the doodads that hide the sun are those of kOcclusionCrown yards or
// more.
constexpr f32 kOcclusionPeriod = 0.2f;
constexpr f32 kOcclusionReach = 150.0f;
constexpr f32 kOcclusionStep = 24.0f;
constexpr f32 kOcclusionCrown = 3.0f;
constexpr u32 kOcclusionDoodads = 8192; // instance slots tested per frame
// The moons' discs fill this fraction of their quads' side (Textures\moon.blp, moon02.blp).
constexpr f32 kMoonDiscRadius = 0.47f;

u32 hashLattice(u32 x, u32 y, u32 seed) {
    u32 h = x * 0x8DA6B343u ^ y * 0xD8163841u ^ seed * 0xCB1AB31Fu;
    h ^= h >> 13;
    h *= 0x5BD1E995u;
    h ^= h >> 15;
    return h;
}

// Lattice value in -1..1 of a tiling grid of `cells` cells.
f32 latticeValue(u32 x, u32 y, u32 cells, u32 seed) {
    return static_cast<f32>(hashLattice(x % cells, y % cells, seed) & 0xFFFFu) / 32767.5f - 1.0f;
}

f32 valueNoise(f32 u, f32 v, u32 cells, u32 seed) {
    const f32 x = u * static_cast<f32>(cells);
    const f32 y = v * static_cast<f32>(cells);
    const u32 x0 = static_cast<u32>(std::floor(x));
    const u32 y0 = static_cast<u32>(std::floor(y));
    f32 fx = x - std::floor(x);
    f32 fy = y - std::floor(y);
    fx = fx * fx * (3.0f - 2.0f * fx);
    fy = fy * fy * (3.0f - 2.0f * fy);
    const f32 a = latticeValue(x0, y0, cells, seed);
    const f32 b = latticeValue(x0 + 1, y0, cells, seed);
    const f32 c = latticeValue(x0, y0 + 1, cells, seed);
    const f32 d = latticeValue(x0 + 1, y0 + 1, cells, seed);
    return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fy;
}

u8 cloudField(u32 px, u32 py, u32 seed) {
    const f32 u = (static_cast<f32>(px) + 0.5f) / kNoiseSize;
    const f32 v = (static_cast<f32>(py) + 0.5f) / kNoiseSize;
    f32 sum = 0.0f;
    f32 amplitude = 1.0f;
    u32 cells = kNoiseCells;
    for (u32 octave = 0; octave < kNoiseOctaves; ++octave) {
        sum += valueNoise(u, v, cells, seed + octave * 101u) * amplitude;
        amplitude *= 0.5f;
        cells *= 2;
    }
    return static_cast<u8>(std::clamp(128.0f + 64.0f * sum, 0.0f, 255.0f));
}

f32 envFloat(const char* name, f32 fallback) {
    const char* value = VD_DEV_ENV(name);
    return value && *value ? static_cast<f32>(std::atof(value)) : fallback;
}

} // namespace

Result<void> Sky::open(const std::filesystem::path& packsDir, const pack::PackReader& textures,
                       const WorldLighting& lighting, Renderer& renderer) {
    VD_PROFILE_ZONE("Sky::open");
    m_forcedSkybox = static_cast<u32>(std::max(0.0f, envFloat("VD_SKYBOX", 0.0f)));
    m_forcedStorm = envFloat("VD_STORM", -1.0f);
    m_glareEnabled = envFloat("VD_SUN_GLARE", 1.0f) != 0.0f;
    m_dayOffset = static_cast<u32>(std::max(0.0f, envFloat("VD_SKY_DAY", 0.0f)));
    m_cloudsEnabled = envFloat("VD_SKY_CLOUDS", 1.0f) != 0.0f;

    // The sun, the moons and their glares (WoW.exe's textures).
    m_sun = loadTexture(textures, "Textures\\sunCenter.blp", renderer);
    m_sunGlare = loadTexture(textures, "Textures\\sunGlare.blp", renderer);
    m_moon = loadTexture(textures, "Textures\\moon.blp", renderer);
    m_blueChild = loadTexture(textures, "Textures\\moon02.blp", renderer);
    m_moonGlare = loadTexture(textures, "Textures\\moonGlare.blp", renderer);
    if (Result<void> clouds = makeCloudNoise(renderer); !clouds) {
        VD_LOG_WARN("Sky: no clouds (%s)", clouds.error().message.c_str());
    }

    // Every sky model of models.vdpk (a handful): the stars and the skyboxes of LightSkybox.dbc and of the
    // WMOs.
    pack::PackReader models;
    if (Result<void> opened = models.open(packsDir / "models.vdpk"); !opened) {
        VD_LOG_WARN("Sky: no models.vdpk, no stars nor skyboxes (%s)", opened.error().message.c_str());
    } else {
        for (u32 i = 0; i < models.entryCount(); ++i) {
            const pack::PackAsset asset = models.entry(i);
            if (asset.type != pack::AssetType::SkyModel) {
                continue;
            }
            if (Result<void> loaded = loadModel(asset, models.entryHash(i), textures, renderer); !loaded) {
                VD_LOG_WARN("Sky model %.*s: %s", static_cast<int>(asset.name.size()), asset.name.data(),
                            loaded.error().message.c_str());
            }
        }
    }
    m_starsHash = assetPathHash(pack::skyModelPath("Environments\\Stars\\Stars.mdx"));
    for (u32 id = 0; id < 64; ++id) {
        const std::string path = pack::skyModelPath(lighting.skyboxModel(id));
        m_lightSkyboxes.push_back(path.empty() ? 0 : assetPathHash(path));
    }
    m_open = true;
    VD_LOG_INFO("Sky: %zu sky models (stars %s), clouds %s", m_models.size(),
                findModel(m_starsHash) ? "yes" : "no", m_clouds.valid() ? "yes" : "no");
    return {};
}

TextureHandle Sky::loadTexture(const pack::PackReader& textures, std::string_view path, Renderer& renderer) {
    const std::optional<pack::PackAsset> asset = textures.find(path);
    if (!asset || asset->type != pack::AssetType::Texture) {
        VD_LOG_WARN("Sky: texture %.*s missing", static_cast<int>(path.size()), path.data());
        return {};
    }
    std::vector<u8> unpacked;
    const u8* data = asset->data;
    usize size = asset->size;
    if (asset->compression != pack::Compression::None) {
        unpacked.resize(asset->rawSize);
        if (!pack::PackReader::read(*asset, unpacked.data(), unpacked.size())) {
            return {};
        }
        data = unpacked.data();
        size = unpacked.size();
    }
    Result<TextureHandle> texture = renderer.createTexture(data, size, nullptr, false);
    if (!texture) {
        VD_LOG_WARN("Sky: texture %.*s: %s", static_cast<int>(path.size()), path.data(),
                    texture.error().message.c_str());
        return {};
    }
    return texture.value();
}

Result<void> Sky::loadModel(const pack::PackAsset& asset, u64 hash, const pack::PackReader& textures,
                            Renderer& renderer) {
    if (asset.compression != pack::Compression::None || asset.size < sizeof(pack::SkyModelHeader)) {
        return Error{"unexpected blob"};
    }
    pack::SkyModelHeader header;
    std::memcpy(&header, asset.data, sizeof(header));
    if (header.version != pack::kSkyModelFormatVersion ||
        header.partsOffset + u64{header.partCount} * sizeof(pack::SkyPart) > asset.size ||
        header.rotationKeysOffset + u64{header.rotationKeyCount} * sizeof(pack::SkyRotationKey) >
            asset.size) {
        return Error{"bad header (re-bake models.vdpk)"};
    }
    std::vector<TextureHandle> partTextures(header.partCount);
    for (u32 p = 0; p < header.partCount; ++p) {
        pack::SkyPart part;
        std::memcpy(&part, asset.data + header.partsOffset + p * sizeof(part), sizeof(part));
        const std::optional<pack::PackAsset> texture =
            part.texture != 0 ? textures.find(part.texture) : std::nullopt;
        if (texture && texture->compression == pack::Compression::None) {
            if (Result<TextureHandle> created =
                    renderer.createTexture(texture->data, texture->size, nullptr, false)) {
                partTextures[p] = created.value();
            }
        }
    }
    Result<SkyModelHandle> handle = renderer.createSkyModel(asset.data, asset.size, partTextures);
    if (!handle) {
        return handle.error();
    }
    Model model;
    model.hash = hash;
    model.handle = handle.value();
    model.loop = header.rotationLoop;
    model.rotation.resize(header.rotationKeyCount);
    if (header.rotationKeyCount > 0) {
        std::memcpy(model.rotation.data(), asset.data + header.rotationKeysOffset,
                    header.rotationKeyCount * sizeof(pack::SkyRotationKey));
    }
    m_models.push_back(std::move(model));
    return {};
}

Result<void> Sky::makeCloudNoise(Renderer& renderer) {
    // A pack texture blob in memory (pack::TextureHeader + RGBA8 mips), made once: two fields of WoW.exe's
    // kind of noise, downsampled by halves for the mips.
    u32 mipCount = 0;
    for (u32 s = kNoiseSize; s > 0; s /= 2) {
        ++mipCount;
    }
    pack::TextureHeader header{};
    header.format = static_cast<u16>(pack::TextureFormat::Rgba8);
    header.width = kNoiseSize;
    header.height = kNoiseSize;
    header.mipCount = static_cast<u16>(mipCount);
    u32 offset = (sizeof(header) + 15) / 16 * 16;
    for (u32 m = 0, s = kNoiseSize; m < mipCount; ++m, s /= 2) {
        header.mipOffsets[m] = offset;
        header.mipSizes[m] = s * s * 4;
        offset += (header.mipSizes[m] + 15) / 16 * 16;
    }
    std::vector<u8> blob(offset, 0);
    std::memcpy(blob.data(), &header, sizeof(header));
    u8* base = blob.data() + header.mipOffsets[0];
    for (u32 y = 0; y < kNoiseSize; ++y) {
        for (u32 x = 0; x < kNoiseSize; ++x) {
            u8* texel = base + (y * kNoiseSize + x) * 4;
            texel[0] = cloudField(x, y, 17u);
            texel[1] = cloudField(x, y, 9001u);
            texel[2] = 128;
            texel[3] = 255;
        }
    }
    for (u32 m = 1, s = kNoiseSize / 2; m < mipCount; ++m, s /= 2) {
        const u8* above = blob.data() + header.mipOffsets[m - 1];
        u8* mip = blob.data() + header.mipOffsets[m];
        const u32 wide = s * 2;
        for (u32 y = 0; y < s; ++y) {
            for (u32 x = 0; x < s; ++x) {
                for (u32 c = 0; c < 4; ++c) {
                    const u32 sum = above[((2 * y) * wide + 2 * x) * 4 + c] +
                                    above[((2 * y) * wide + 2 * x + 1) * 4 + c] +
                                    above[((2 * y + 1) * wide + 2 * x) * 4 + c] +
                                    above[((2 * y + 1) * wide + 2 * x + 1) * 4 + c];
                    mip[(y * s + x) * 4 + c] = static_cast<u8>((sum + 2) / 4);
                }
            }
        }
    }
    Result<TextureHandle> texture = renderer.createTexture(blob.data(), blob.size(), nullptr, false);
    if (!texture) {
        return texture.error();
    }
    m_clouds = texture.value();
    return {};
}

const Sky::Model* Sky::findModel(u64 hash) const {
    if (hash == 0) {
        return nullptr;
    }
    for (const Model& model : m_models) {
        if (model.hash == hash) {
            return &model;
        }
    }
    return nullptr;
}

glm::vec4 Sky::rotationAt(const Model& model) const {
    if (model.loop == 0 || model.rotation.size() < 2) {
        return glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    }
    const u32 ms = static_cast<u32>(std::fmod(m_time * 1000.0, static_cast<f64>(model.loop)));
    const std::vector<pack::SkyRotationKey>& keys = model.rotation;
    usize next = 0;
    while (next < keys.size() && keys[next].time <= ms) {
        ++next;
    }
    if (next == 0 || next == keys.size()) {
        const pack::SkyRotationKey& key = next == 0 ? keys.front() : keys.back();
        return glm::vec4(key.rotation[0], key.rotation[1], key.rotation[2], key.rotation[3]);
    }
    const pack::SkyRotationKey& a = keys[next - 1];
    const pack::SkyRotationKey& b = keys[next];
    const f32 t = b.time > a.time ? static_cast<f32>(ms - a.time) / static_cast<f32>(b.time - a.time) : 0.0f;
    glm::vec4 qa(a.rotation[0], a.rotation[1], a.rotation[2], a.rotation[3]);
    glm::vec4 qb(b.rotation[0], b.rotation[1], b.rotation[2], b.rotation[3]);
    if (glm::dot(qa, qb) < 0.0f) {
        qb = -qb;
    }
    return glm::normalize(glm::mix(qa, qb, t)); // the client's normalized lerp
}

void Sky::overrideConditions(LightConditions& conditions) const {
    if (m_forcedStorm >= 0.0f) {
        conditions.storm = std::clamp(m_forcedStorm, 0.0f, 1.0f);
    }
}

void Sky::probeOcclusion(f32 dt, const glm::dvec3& camera, const std::array<glm::vec3, 2>& directions,
                         const std::array<f32, 2>& fades, const WorldObjects* objects,
                         const physics::CollisionWorld* collision, f32 viewDistance) {
    // WoW.exe asks the GPU how much of the sun's quad was drawn (an occlusion query), or casts a ray through
    // the world: here a ray through the collision world (terrain, buildings, trunks and rocks) and past the
    // doodads' bounding spheres (the crowns of the trees, which collide with nothing).
    m_occlusionTimer -= dt;
    if (!m_probe.running) {
        if (m_occlusionTimer > 0.0f) {
            return;
        }
        m_occlusionTimer = kOcclusionPeriod;
        const u32 glare = fades[1] > fades[0] ? 1 : 0;
        if (fades[glare] <= 0.0f) {
            return; // neither glare shows: nothing to probe
        }
        m_probe = {true, glare, camera, directions[glare], std::min(viewDistance, kOcclusionReach), 0, 0.5f};
        return;
    }
    // The doodads first, a slice of their spheres per frame.
    if (objects && m_probe.nextDoodad < objects->instanceSlots()) {
        if (objects->segmentHitsDoodad(m_probe.from, m_probe.direction, m_probe.reach, kOcclusionCrown,
                                       m_probe.nextDoodad, kOcclusionDoodads)) {
            m_glares[m_probe.glare].occluded = true;
            m_probe.running = false;
        }
        m_probe.nextDoodad += kOcclusionDoodads;
        return;
    }
    // One leg against the triangles of its own box, so no gather gets large.
    bool hit = false;
    if (collision && collision->isOpen()) {
        const f32 length = std::min(kOcclusionStep, m_probe.reach - m_probe.next);
        const glm::dvec3 a = m_probe.from + glm::dvec3(m_probe.direction) * static_cast<f64>(m_probe.next);
        const glm::dvec3 b = a + glm::dvec3(m_probe.direction) * static_cast<f64>(length);
        m_rayTriangles.reset(a);
        collision->gather(glm::min(a, b) - glm::dvec3(0.5), glm::max(a, b) + glm::dvec3(0.5), m_rayTriangles);
        physics::Hit contact;
        hit = physics::raycast(m_rayTriangles, glm::vec3(0.0f), m_probe.direction * length, contact);
    }
    m_probe.next += kOcclusionStep;
    if (hit || m_probe.next >= m_probe.reach || !collision || !collision->isOpen()) {
        m_glares[m_probe.glare].occluded = hit;
        m_probe.running = false;
    }
}

void Sky::update(f32 dt, f32 dayFraction, u32 day, const Camera& camera, const SceneLighting& lighting,
                 const WorldObjects* objects, const physics::CollisionWorld* collision, f32 viewDistance) {
    VD_PROFILE_ZONE("Sky::update");
    m_frame = SkyFrame{};
    if (!m_open) {
        return;
    }
    m_time += static_cast<f64>(dt);
    SkyFrame& frame = m_frame;
    const f32 t = dayFraction - std::floor(dayFraction);
    const glm::vec3 forward = camera.forward();
    const glm::vec3 right = camera.right();
    const glm::vec3 up = glm::normalize(glm::cross(right, forward));

    // A skybox: forced, the WMO's around the camera, else the one Light.dbc's parameters name. While it is
    // opaque the normal sky is not drawn (WoW.exe skips it from 0.99).
    u64 skyboxHash = m_forcedSkybox == 0 && objects ? objects->skyboxAt(camera.position) : 0;
    if (skyboxHash == 0) {
        const u32 id = m_forcedSkybox != 0 ? m_forcedSkybox : lighting.skybox;
        skyboxHash = id < m_lightSkyboxes.size() ? m_lightSkyboxes[id] : 0;
    }
    f32 skyboxAlpha = 0.0f;
    if (const Model* skybox = findModel(skyboxHash)) {
        skyboxAlpha = 1.0f;
        frame.skybox = {skybox->handle, rotationAt(*skybox), skyboxAlpha};
    }
    frame.dome = skyboxAlpha < 0.99f;

    // The sun and the moons: LightIntBand 9's color, gone with the storm (the client's alpha is 1 - its
    // weather's intensity) and with LightFloatBand 2.
    const f32 storm = std::clamp(lighting.storm, 0.0f, 1.0f);
    const f32 shown = (1.0f - storm) * std::clamp(lighting.celestialGlow, 0.0f, 1.0f);
    const celestial::Body bodies[kMaxSkyBodies] = {celestial::sun(t), celestial::whiteLady(t),
                                                   celestial::blueChild(t, day + m_dayOffset)};
    const TextureHandle textures[kMaxSkyBodies] = {m_sun, m_moon, m_blueChild};
    for (u32 i = 0; i < kMaxSkyBodies && frame.dome && shown > 0.0f; ++i) {
        const celestial::Body& body = bodies[i];
        if (body.direction.z * celestial::kDistance + body.size * 0.5f <= 0.0f || !textures[i].valid()) {
            continue; // below the horizon
        }
        SkySprite& sprite = frame.bodies[frame.bodyCount++];
        sprite.texture = textures[i];
        sprite.center = body.direction * celestial::kDistance;
        sprite.right = right * (body.size * 0.5f);
        sprite.up = up * (body.size * 0.5f);
        sprite.color = glm::vec4(lighting.sunColor, shown);
        if (i > 0 && shown >= 0.5f) { // a moon: no stars through its disc
            const f32 radius = std::atan(kMoonDiscRadius * body.size / celestial::kDistance);
            frame.starMasks[i - 1] = glm::vec4(body.direction, std::cos(radius));
        }
    }

    // The stars: WoW.exe's alpha byte is 1 + 254 * the curve, drawn from 2.
    const f32 stars = std::floor(celestial::starsAlpha(t) * 254.0f + 1.0f);
    if (const Model* model = findModel(m_starsHash); model && frame.dome && stars >= 2.0f) {
        frame.stars = {model->handle, glm::vec4(0.0f, 0.0f, 0.0f, 1.0f), stars / 255.0f};
    }

    // The clouds, lit by the sun by day and by the White Lady by night.
    if (frame.dome && m_clouds.valid() && m_cloudsEnabled) {
        const glm::vec2 drift1 = glm::fract(kCloudDrift1 * static_cast<f32>(std::fmod(m_time, 1.0e5)));
        const glm::vec2 drift2 = glm::fract(kCloudDrift2 * static_cast<f32>(std::fmod(m_time, 1.0e5)));
        frame.clouds = m_clouds;
        frame.cloudDrift = glm::vec4(drift1, drift2);
        const f32 light = celestial::cloudLightFade(t) * (1.0f - 0.75f * storm);
        frame.cloudShape =
            glm::vec4(1.0f - std::clamp(lighting.cloudDensity, 0.0f, 1.0f), kCloudSharpness, light, storm);
        frame.cloudLight = celestial::cloudsLitBySun(t) ? bodies[0].direction : bodies[1].direction;
    }

    // The glares over the scene: grow and brighten as the camera looks at them, fade with the time of day,
    // while something stands in front of their body (probeOcclusion) and under an opaque skybox.
    const std::array<f32, 2> fades{m_glareEnabled ? celestial::sunGlareFade(t) : 0.0f,
                                   m_glareEnabled ? celestial::moonGlareFade(t) : 0.0f};
    probeOcclusion(dt, camera.position, {bodies[0].direction, bodies[1].direction}, fades, objects, collision,
                   viewDistance);
    const celestial::Glare* kinds[2] = {&celestial::kSunGlare, &celestial::kMoonGlare};
    const TextureHandle glareTextures[2] = {m_sunGlare, m_moonGlare};
    for (u32 i = 0; i < 2; ++i) {
        Glare& glare = m_glares[i];
        const celestial::Body& body = bodies[i];
        const f32 timeFade = fades[i];
        const f32 target = (glare.occluded ? 0.0f : 1.0f) * timeFade * (1.0f - skyboxAlpha) *
                           std::clamp(lighting.celestialGlow, 0.0f, 1.0f);
        glare.visibility = target > glare.visibility
                               ? std::min(target, glare.visibility + dt * kinds[i]->rise)
                               : std::max(target, glare.visibility - dt * celestial::kGlareFall);
        const f32 facing = celestial::glareFacing(forward, body.direction);
        const f32 alpha = (kinds[i]->alphaAway + (kinds[i]->alphaAt - kinds[i]->alphaAway) * facing) *
                          glare.visibility * (1.0f - storm);
        if (alpha <= 0.0f || !glareTextures[i].valid()) {
            continue;
        }
        const f32 scale = kinds[i]->bodyScale > 0.0f
                              ? body.size * kinds[i]->bodyScale
                              : kinds[i]->scaleAway + (kinds[i]->scaleAt - kinds[i]->scaleAway) * facing;
        SkySprite& sprite = frame.glares[frame.glareCount++];
        sprite.texture = glareTextures[i];
        sprite.center = body.direction * celestial::kDistance;
        sprite.right = right * (scale * 0.5f);
        sprite.up = up * (scale * 0.5f);
        sprite.color = glm::vec4(lighting.sunColor, alpha);
    }
}

} // namespace vd

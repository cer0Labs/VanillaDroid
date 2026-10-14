#include "BakerCommon.h"

#include "core/Clock.h"
#include "core/Hash.h"
#include "core/Log.h"
#include "formats/M2.h"
#include "jobs/JobSystem.h"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <type_traits>
#include <vector>

namespace vd::baker {
namespace {

// Counters and value ranges of a scan, merged across jobs.
struct Census {
    std::map<std::string, u64> counts;
    std::map<std::string, std::pair<f64, f64>> ranges;

    void count(const std::string& key, u64 n = 1) { counts[key] += n; }
    void range(const std::string& key, f64 value) {
        const auto [it, inserted] = ranges.try_emplace(key, value, value);
        if (!inserted) {
            it->second.first = std::min(it->second.first, value);
            it->second.second = std::max(it->second.second, value);
        }
    }
    void merge(const Census& other) {
        for (const auto& [key, n] : other.counts) {
            counts[key] += n;
        }
        for (const auto& [key, r] : other.ranges) {
            range(key, r.first);
            range(key, r.second);
        }
    }
};

template <typename T>
void track(Census& c, const std::string& name, const formats::M2Track<T>& t, usize sequences) {
    if (t.times.empty()) {
        c.count(name + ": no keys");
        return;
    }
    const bool constant =
        std::all_of(t.values.begin(), t.values.end(), [&](const T& v) { return v == t.values[0]; });
    c.count(name + (constant ? ": constant" : ": animated"));
    c.count(name + ": interpolation " + std::to_string(t.interpolation));
    if (t.globalSequence >= 0) {
        c.count(name + ": global sequence");
    } else if (!constant && t.ranges.size() < sequences) {
        c.count(name + ": fewer ranges than sequences");
    }
    if constexpr (std::is_arithmetic_v<T>) {
        for (const T& v : t.values) {
            c.range(name, static_cast<f64>(v));
        }
    } else {
        for (const auto& v : t.values) {
            for (usize i = 0; i < v.size(); ++i) {
                c.range(name + "[" + std::to_string(i) + "]", static_cast<f64>(v[i]));
            }
        }
    }
}

std::string hex(u32 value) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%x", value);
    return text;
}

void scanModel(const formats::M2Model& m, const std::string& path, Census& c, bool verbose) {
    const usize sequences = m.sequences.size();
    c.count(m.particles.empty() ? "models without particles" : "models with particles");
    c.count(m.ribbons.empty() ? "models without ribbons" : "models with ribbons");
    c.count(m.textureTransforms.empty() ? "models without texture transforms"
                                        : "models with texture transforms");
    if (m.textureFlipbookCount > 0) {
        c.count("models with texture flipbooks");
    }
    if (!m.particles.empty() && m.vertices.empty()) {
        c.count("models with particles and no vertices");
    }
    c.count("particle emitters", m.particles.size());
    c.count("ribbon emitters", m.ribbons.size());
    c.count("texture transforms", m.textureTransforms.size());
    c.range("emitters per model", static_cast<f64>(m.particles.size()));
    for (const formats::M2Batch& b : m.batches) {
        if (b.textureTransform >= 0) {
            c.count("batches with a texture transform");
        }
    }
    if (verbose && !m.textureTransforms.empty()) {
        std::printf("%s: %zu texture transforms (translation keys %zu, rotation keys %zu)\n", path.c_str(),
                    m.textureTransforms.size(), m.textureTransforms[0].translation.times.size(),
                    m.textureTransforms[0].rotation.times.size());
    }
    for (const formats::M2TextureTransform& t : m.textureTransforms) {
        track(c, "uv translation", t.translation, sequences);
        track(c, "uv rotation", t.rotation, sequences);
        track(c, "uv scale", t.scale, sequences);
    }
    for (const formats::M2ParticleEmitter& e : m.particles) {
        c.count("emitter blend " + std::to_string(e.blend));
        c.count("emitter type " + std::to_string(e.emitterType));
        c.count("emitter particleType " + std::to_string(e.particleType));
        c.count("emitter headOrTail " + std::to_string(e.headOrTail));
        c.count("emitter cells " + std::to_string(e.rows) + "x" + std::to_string(e.columns));
        c.count("emitter tileRotation " + std::to_string(e.tileRotation));
        c.count("emitter id " + std::to_string(e.id));
        c.count("emitter texture type " + std::to_string(m.textures[e.texture].type));
        for (u32 bit = 0; bit < 32; ++bit) {
            if (e.flags & (1u << bit)) {
                c.count("emitter flag " + hex(1u << bit));
            }
        }
        if (!e.geometryModel.empty()) {
            c.count("emitter with geometry model");
            // Phase 14 (spellfx): few, always listed: which models emit which models.
            const f32 rate = e.emissionRate.values.empty() ? 0.0f : e.emissionRate.values[0];
            const f32 life = e.lifespan.values.empty() ? 0.0f : e.lifespan.values[0];
            std::printf("GEOMETRY %s: emits %s (bone %u flags 0x%x type %u rate %.2f life %.2f scales "
                        "%.2f/%.2f/%.2f tumble %.2f %.2f %.2f .. %.2f %.2f %.2f spin %.2f blend %u)\n",
                        path.c_str(), e.geometryModel.c_str(), e.bone, e.flags, e.emitterType, rate, life,
                        e.scales[0], e.scales[1], e.scales[2], e.tumbleMin[0], e.tumbleMin[1], e.tumbleMin[2],
                        e.tumbleMax[0], e.tumbleMax[1], e.tumbleMax[2], e.spin, e.blend);
        }
        if (!e.recursionModel.empty()) {
            c.count("emitter with recursion model");
            std::printf("RECURSION %s: emitters of %s\n", path.c_str(), e.recursionModel.c_str());
        }
        if (!e.splinePoints.empty()) {
            c.count("emitter with spline points");
            c.range("spline points", static_cast<f64>(e.splinePoints.size()));
        }
        track(c, "speed", e.speed, sequences);
        track(c, "speedVariation", e.speedVariation, sequences);
        track(c, "verticalRange", e.verticalRange, sequences);
        track(c, "horizontalRange", e.horizontalRange, sequences);
        track(c, "gravity", e.gravity, sequences);
        track(c, "lifespan", e.lifespan, sequences);
        track(c, "emissionRate", e.emissionRate, sequences);
        track(c, "areaLength", e.areaLength, sequences);
        track(c, "areaWidth", e.areaWidth, sequences);
        track(c, "zSource", e.zSource, sequences);
        track(c, "enabled", e.enabled, sequences);
        if (e.emitterType == formats::kM2EmitterSphere && !e.areaLength.values.empty() &&
            !e.areaWidth.values.empty()) {
            c.count(e.areaLength.values[0] <= e.areaWidth.values[0] ? "sphere length <= width"
                                                                    : "sphere length > width");
        }
        c.range("midPoint", e.midPoint);
        for (u32 i = 0; i < 3; ++i) {
            c.range("scale " + std::to_string(i), e.scales[i]);
            c.range("alpha " + std::to_string(i), static_cast<f64>(e.colors[i] >> 24));
            c.range("headLifeCells " + std::to_string(i), e.headLifeCells[i]);
            c.range("headDecayCells " + std::to_string(i), e.headDecayCells[i]);
        }
        for (u32 i = 0; i < 2; ++i) {
            c.range("tailLifeCells " + std::to_string(i), e.tailLifeCells[i]);
            c.range("tailDecayCells " + std::to_string(i), e.tailDecayCells[i]);
        }
        const u32 cells = u32{e.rows} * e.columns;
        if (std::max({e.headLifeCells[0], e.headLifeCells[1], e.headLifeCells[2], e.headDecayCells[0],
                      e.headDecayCells[1], e.headDecayCells[2]}) >= std::max(cells, 1u)) {
            c.count("emitter head cell >= rows*columns");
        }
        char twinkle[96];
        std::snprintf(twinkle, sizeof(twinkle), "twinkle speed %g pct %g scale %g..%g", e.twinkleSpeed,
                      e.twinklePercent, e.twinkleMin, e.twinkleMax);
        c.count(twinkle);
        c.range("tailLength", e.tailLength);
        c.range("twinkleSpeed", e.twinkleSpeed);
        c.range("twinklePercent", e.twinklePercent);
        c.range("twinkleMin", e.twinkleMin);
        c.range("twinkleMax", e.twinkleMax);
        c.range("burstMultiplier", e.burstMultiplier);
        c.range("drag", e.drag);
        c.range("spin", e.spin);
        c.range("windTime", e.windTime);
        for (u32 i = 0; i < 3; ++i) {
            c.range("wind " + std::to_string(i), e.wind[i]);
            c.range("tumbleMin " + std::to_string(i), e.tumbleMin[i]);
            c.range("tumbleMax " + std::to_string(i), e.tumbleMax[i]);
        }
        c.range("followSpeed1", e.followSpeed1);
        c.range("followScale1", e.followScale1);
        c.range("followSpeed2", e.followSpeed2);
        c.range("followScale2", e.followScale2);
        if (verbose) {
            const f32 rate = e.emissionRate.values.empty() ? 0.0f : e.emissionRate.values[0];
            const f32 life = e.lifespan.values.empty() ? 0.0f : e.lifespan.values[0];
            const f32 speed = e.speed.values.empty() ? 0.0f : e.speed.values[0];
            std::printf(
                "%s: emitter bone %u flags 0x%x blend %u type %u pt %u hot %u cells %ux%u rate %.2f life "
                "%.2f "
                "speed %.2f mid %.2f scales %.2f/%.2f/%.2f colors %08x/%08x/%08x cells %u-%u-%u/%u-%u-%u "
                "tail %.2f drag %.2f spin %.2f texture %s\n",
                path.c_str(), e.bone, e.flags, e.blend, e.emitterType, e.particleType, e.headOrTail, e.rows,
                e.columns, rate, life, speed, e.midPoint, e.scales[0], e.scales[1], e.scales[2], e.colors[0],
                e.colors[1], e.colors[2], e.headLifeCells[0], e.headLifeCells[1], e.headLifeCells[2],
                e.headDecayCells[0], e.headDecayCells[1], e.headDecayCells[2], e.tailLength, e.drag, e.spin,
                m.textures[e.texture].path.c_str());
            if (std::getenv("VD_SCAN_KEYS")) {
                std::printf("  rate keys:");
                for (usize k = 0; k < e.emissionRate.times.size() && k < 24; ++k) {
                    std::printf(" %u:%.1f", e.emissionRate.times[k], e.emissionRate.values[k]);
                }
                std::printf(" | ranges %zu, life keys %zu, enabled keys:", e.emissionRate.ranges.size(),
                            e.lifespan.times.size());
                for (usize k = 0; k < e.enabled.times.size() && k < 12; ++k) {
                    std::printf(" %u:%u", e.enabled.times[k], e.enabled.values[k]);
                }
                std::printf("\n");
            }
        }
    }
    for (const formats::M2RibbonEmitter& r : m.ribbons) {
        c.count("ribbon textures " + std::to_string(r.textures.size()));
        c.count("ribbon materials " + std::to_string(r.materials.size()));
        for (u16 material : r.materials) {
            c.count("ribbon material blend " + std::to_string(static_cast<u32>(m.materials[material].blend)));
        }
        c.count("ribbon cells " + std::to_string(r.rows) + "x" + std::to_string(r.columns));
        c.range("ribbon edgesPerSecond", r.edgesPerSecond);
        c.range("ribbon edgeLifetime", r.edgeLifetime);
        c.range("ribbon gravity", r.gravity);
        track(c, "ribbon color", r.color, sequences);
        track(c, "ribbon alpha", r.alpha, sequences);
        track(c, "ribbon above", r.above, sequences);
        track(c, "ribbon below", r.below, sequences);
        track(c, "ribbon textureSlot", r.textureSlot, sequences);
        track(c, "ribbon visibility", r.visibility, sequences);
        if (verbose) {
            std::printf("%s: ribbon bone %u edges/s %.1f life %.2f gravity %.2f textures %zu materials %zu\n",
                        path.c_str(), r.bone, r.edgesPerSecond, r.edgeLifetime, r.gravity, r.textures.size(),
                        r.materials.size());
        }
    }
}

} // namespace

// Parses the effects of every M2 of the client (particle emitters, ribbons, texture transforms) and reports
// what the data contains: the layout of the 1.12 records and the assumptions of the baked format are checked
// here.
// --filter <path prefix> with VD_SCAN_VERBOSE=1 prints every emitter of the matching models.
int commandScanParticles(const Options& options) {
    formats::MpqArchiveSet mpq;
    if (!openArchives(options, mpq)) {
        return 1;
    }
    const Clock::time_point start = Clock::now();
    const std::vector<std::string> paths = mpq.list("*.m2");
    const std::string filter = normalizeAssetPath(option(options, "filter"));
    const bool verbose = std::getenv("VD_SCAN_VERBOSE") != nullptr;
    std::mutex mutex;
    std::vector<std::string> failures;
    Census census;
    JobSystem jobs;
    jobs.init();
    jobs.parallelFor(static_cast<u32>(paths.size()), 8, [&](u32 begin, u32 end) {
        for (u32 i = begin; i < end; ++i) {
            if (!matchesFilter(normalizeAssetPath(paths[i]), filter)) {
                continue;
            }
            Result<std::vector<u8>> bytes = mpq.read(paths[i]);
            Result<formats::M2Model> parsed =
                bytes ? formats::parseM2(bytes.value()) : Result<formats::M2Model>(bytes.error());
            Census local;
            if (parsed && parsed.value().effectsError.empty()) {
                scanModel(parsed.value(), paths[i], local, verbose);
            }
            std::lock_guard<std::mutex> lock(mutex);
            if (!parsed) {
                failures.push_back(paths[i] + ": " + parsed.error().message);
            } else if (!parsed.value().effectsError.empty()) {
                failures.push_back(paths[i] + ": " + parsed.value().effectsError);
            } else {
                census.count("models");
                census.merge(local);
            }
        }
    });
    jobs.shutdown();
    for (const auto& [key, value] : census.counts) {
        std::printf("%-44s %10llu\n", key.c_str(), static_cast<unsigned long long>(value));
    }
    for (const auto& [key, range] : census.ranges) {
        std::printf("%-44s %12.4g .. %.4g\n", key.c_str(), range.first, range.second);
    }
    for (const std::string& failure : failures) {
        std::printf("FAILED %s\n", failure.c_str());
    }
    VD_LOG_INFO("%llu M2 scanned, %zu failed, %.1f s",
                static_cast<unsigned long long>(census.counts["models"]), failures.size(),
                secondsBetween(start, Clock::now()));
    return failures.empty() ? 0 : 1;
}

} // namespace vd::baker

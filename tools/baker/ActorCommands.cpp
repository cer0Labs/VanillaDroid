#include "BakerCommon.h"

#include "bake/ActorBaker.h"
#include "bake/EffectBaker.h"
#include "bake/PackWriter.h"
#include "core/Clock.h"
#include "core/Hash.h"
#include "core/Log.h"
#include "formats/Dbc.h"
#include "formats/M2.h"
#include "jobs/JobSystem.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace vd::baker {
namespace {

// CreatureModelData.dbc (5875): field 2 is the model path, with the old ".mdx" extension.
constexpr u32 kCreatureModelPathField = 2;
// SpellVisualEffectName.dbc (5875): 0 id, 1 name, 2 model path (".mdx"/".mdl"), 3 area effect size, 4 scale.
constexpr u32 kSpellEffectPathField = 2;

// A model path as SpellVisualEffectName spells it (leading backslashes, a few absolute paths of Blizzard's build
// machines "...\Data\Spells\X.mdx") -> its path in the client.
std::string spellEffectPath(std::string path) {
    std::string lower = path;
    for (char& c : lower) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (const usize data = lower.rfind("\\data\\"); data != std::string::npos) {
        path.erase(0, data + 6);
    }
    while (!path.empty() && (path[0] == '\\' || path[0] == '/')) {
        path.erase(0, 1);
    }
    return path;
}

std::string toM2Path(std::string path) {
    const usize dot = path.find_last_of('.');
    if (dot != std::string::npos) {
        path.resize(dot);
    }
    return path + ".m2";
}

// Phase 14 (spellfx): the models particle emitters emit (Cone of Cold's shards, Cyclone's rocks), read from
// the client's archives once each, for the bake's jobs.
class ArchiveParticleModels final : public bake::ParticleModelSource {
public:
    explicit ArchiveParticleModels(const formats::MpqArchiveSet& mpq) : m_mpq(mpq) {}
    const formats::M2Model* particleModel(const std::string& path) const override {
        const std::string m2 = toM2Path(path);
        std::lock_guard<std::mutex> lock(m_mutex);
        auto [it, inserted] = m_models.try_emplace(normalizeAssetPath(m2));
        if (inserted) {
            Result<std::vector<u8>> bytes = m_mpq.read(m2);
            Result<formats::M2Model> parsed =
                bytes ? formats::parseM2(bytes.value()) : Result<formats::M2Model>(bytes.error());
            if (parsed) {
                it->second = std::make_unique<formats::M2Model>(std::move(parsed.value()));
            } else {
                VD_LOG_WARN("Particle model %s: %s", m2.c_str(), parsed.error().message.c_str());
            }
        }
        return it->second.get();
    }
    usize count() const { return m_models.size(); }

private:
    const formats::MpqArchiveSet& m_mpq;
    mutable std::mutex m_mutex;
    mutable std::map<std::string, std::unique_ptr<formats::M2Model>> m_models;
};

// Phase 13: whether a game object's M2 has animations of its own (Spawn .. Despawn of AnimationData.dbc: doors and
// chests opening, levers, the fishing bobber's Custom0): only those are actors, the rest stay static doodads.
bool animatesAsObject(const formats::M2Model& m2) {
    constexpr u16 kSpawn = 145, kDespawn = 157;
    return std::any_of(m2.sequences.begin(), m2.sequences.end(), [](const formats::M2Sequence& s) {
        return s.animationId >= kSpawn && s.animationId <= kDespawn;
    });
}

// Every model an actor can use: the race models, the creature models, the item models that attach to them, and the
// spell effect models (Spells\, and what SpellVisualEffectName.dbc names: they attach to actors and have bones), and
// the client's own effects (Particles\: the loot sparkles, footstep sprays, blood spurts...). Phase 13: also the game
// objects' models (GameObjectDisplayInfo.dbc), whose canonical paths go to `objectOnly` when nothing else uses them
// (the bake keeps those only when they animate).
// Original spellings (as the MPQ lists them), deduplicated by canonical path.
std::vector<std::string> actorModelPaths(const formats::MpqArchiveSet& mpq, u32& missing,
                                         std::set<std::string>& objectOnly) {
    std::map<std::string, std::string> byCanonical;
    auto add = [&](const std::string& path) { byCanonical.emplace(normalizeAssetPath(path), path); };
    // Phase 18: the glue screens' scenes (Interface\Glues\Models: the Dark Portal, the races' places). Ours: the marks
    // over the NPCs (Interface\Buttons\TalkToMe*.m2: app/QuestMarks).
    for (const char* mask : {"Character\\*.m2", "Item\\ObjectComponents\\*.m2", "Spells\\*.m2", "Particles\\*.m2",
                             "Interface\\Glues\\Models\\*.m2", "Interface\\Buttons\\TalkToMe*.m2"}) {
        for (const std::string& path : mpq.list(mask)) {
            add(path);
        }
    }
    missing = 0;
    if (Result<std::vector<u8>> bytes = mpq.read("DBFilesClient\\CreatureModelData.dbc"); bytes) {
        if (Result<formats::DbcFile> table = formats::DbcFile::parse(std::move(bytes.value())); table) {
            for (u32 r = 0; r < table.value()->recordCount(); ++r) {
                const std::string path = toM2Path(std::string(table.value()->getString(r, kCreatureModelPathField)));
                if (path.size() <= 3) {
                    continue;
                }
                if (mpq.contains(path)) {
                    add(path);
                } else {
                    ++missing;
                }
            }
        }
    }
    if (Result<std::vector<u8>> bytes = mpq.read("DBFilesClient\\SpellVisualEffectName.dbc"); bytes) {
        if (Result<formats::DbcFile> table = formats::DbcFile::parse(std::move(bytes.value())); table) {
            for (u32 r = 0; r < table.value()->recordCount(); ++r) {
                const std::string path =
                    toM2Path(spellEffectPath(std::string(table.value()->getString(r, kSpellEffectPathField))));
                if (path.size() <= 3) {
                    continue;
                }
                if (mpq.contains(path)) {
                    add(path);
                } else {
                    ++missing;
                }
            }
        }
    }
    // Phase 13: game objects (doors, chests, levers, the fishing bobber...); field 1 is the model (".mdx", or a WMO).
    if (Result<std::vector<u8>> bytes = mpq.read("DBFilesClient\\GameObjectDisplayInfo.dbc"); bytes) {
        if (Result<formats::DbcFile> table = formats::DbcFile::parse(std::move(bytes.value())); table) {
            for (u32 r = 0; r < table.value()->recordCount(); ++r) {
                std::string model(table.value()->getString(r, 1));
                const usize dot = model.find_last_of('.');
                if (model.size() <= 4 || dot == std::string::npos ||
                    (model.size() - dot == 4 && std::tolower(static_cast<unsigned char>(model[dot + 1])) == 'w')) {
                    continue; // none, or a WMO (transports)
                }
                const std::string path = toM2Path(model);
                if (mpq.contains(path) && byCanonical.emplace(normalizeAssetPath(path), path).second) {
                    objectOnly.insert(normalizeAssetPath(path));
                }
            }
        }
    }
    std::vector<std::string> paths;
    for (auto& [canonical, path] : byCanonical) {
        paths.push_back(path);
    }
    return paths;
}

} // namespace

int bakeActors(const formats::MpqArchiveSet& mpq, const std::filesystem::path& outPath) {
    const Clock::time_point start = Clock::now();
    u32 missing = 0;
    std::set<std::string> objectOnly; // phase 13: game object models nothing else uses
    const std::vector<std::string> paths = actorModelPaths(mpq, missing, objectOnly);
    u32 objectModels = 0, staticObjects = 0;
    bake::PackWriter writer;
    if (Result<void> created = writer.create(outPath, kBakerVersion); !created) {
        VD_LOG_ERROR("%s", created.error().message.c_str());
        return 1;
    }
    std::mutex mutex;
    std::vector<std::string> failures;
    u64 rawBytes = 0;
    u32 maxBones = 0, maxVertices = 0;
    u32 unitModels = 0, framedPortraits = 0; // creatures and races; those without a portrait camera
    const ArchiveParticleModels particleModels(mpq); // phase 14
    u32 meshEmitters = 0;
    JobSystem jobs;
    jobs.init();
    jobs.parallelFor(static_cast<u32>(paths.size()), 4, [&](u32 begin, u32 end) {
        for (u32 i = begin; i < end; ++i) {
            Result<std::vector<u8>> bytes = mpq.read(paths[i]);
            Result<formats::M2Model> m2 = bytes ? formats::parseM2(bytes.value()) : Result<formats::M2Model>(bytes.error());
            const bool object = objectOnly.contains(normalizeAssetPath(paths[i]));
            if (object && (!m2 || !animatesAsObject(m2.value()))) {
                std::lock_guard<std::mutex> lock(mutex);
                ++staticObjects; // a game object without animations of its own: a static doodad (models.vdpk)
                continue;
            }
            Result<std::vector<u8>> blob =
                m2 ? bake::bakeActor(m2.value(), &particleModels) : Result<std::vector<u8>>(m2.error());
            Result<void> added = blob ? writer.add(paths[i], pack::AssetType::ActorModel, blob.value(), true)
                                      : Result<void>(blob.error());
            std::lock_guard<std::mutex> lock(mutex);
            if (!added) {
                failures.push_back(paths[i] + ": " + added.error().message);
                continue;
            }
            rawBytes += blob.value().size();
            objectModels += object ? 1 : 0;
            const std::string canonical = normalizeAssetPath(paths[i]);
            if (canonical.starts_with("creature/") || canonical.starts_with("character/")) { // what units are
                ++unitModels;
                framedPortraits += m2.value().camera(formats::kM2CameraPortrait) ? 0 : 1;
            }
            maxBones = std::max(maxBones, static_cast<u32>(m2.value().bones.size()));
            for (const formats::M2ParticleEmitter& e : m2.value().particles) {
                meshEmitters += e.geometryModel.empty() ? 0 : 1; // phase 14: emitters of models
            }
            maxVertices = std::max(maxVertices, static_cast<u32>(m2.value().vertices.size()));
        }
    });
    jobs.shutdown();
    for (const std::string& failure : failures) {
        VD_LOG_ERROR("FAILED %s", failure.c_str());
    }
    if (Result<void> finished = writer.finish(); !finished) {
        VD_LOG_ERROR("%s", finished.error().message.c_str());
        return 1;
    }
    VD_LOG_INFO("actors: %u emitters of models (%zu particle models read)", meshEmitters,
                particleModels.count());
    VD_LOG_INFO("actors: %zu models (%.1f MB raw, %.1f MB in pack), most bones %u, most vertices %u, %u creature "
                "and spell models missing from the client, %u of %u creature and race models framed by the baker "
                "(no portrait camera), %u animated game objects (%u static ones left to models.vdpk), %zu failed | "
                "%.1f s -> %s",
                paths.size() - failures.size() - staticObjects, static_cast<double>(rawBytes) / 1e6,
                static_cast<double>(writer.bytesWritten()) / 1e6, maxBones, maxVertices, missing, framedPortraits,
                unitModels, objectModels, staticObjects, failures.size(), secondsBetween(start, Clock::now()),
                outPath.string().c_str());
    return failures.empty() ? 0 : 1;
}

// Parses every M2 of the client with the full (animated) parser and reports what the data contains: the
// assumptions of the baked format (bone order, interpolation kinds, texture types) are checked here.
int commandScanActors(const Options& options) {
    formats::MpqArchiveSet mpq;
    if (!openArchives(options, mpq)) {
        return 1;
    }
    const Clock::time_point start = Clock::now();
    const std::vector<std::string> paths = mpq.list("*.m2");
    std::mutex mutex;
    std::vector<std::string> failures;
    std::map<std::string, u32> counts;
    u32 maxBones = 0, maxSequences = 0, maxAttachments = 0, maxGlobal = 0;
    std::string mostBones;
    JobSystem jobs;
    jobs.init();
    jobs.parallelFor(static_cast<u32>(paths.size()), 8, [&](u32 begin, u32 end) {
        for (u32 i = begin; i < end; ++i) {
            Result<std::vector<u8>> bytes = mpq.read(paths[i]);
            Result<formats::M2Model> parsed =
                bytes ? formats::parseM2(bytes.value()) : Result<formats::M2Model>(bytes.error());
            std::map<std::string, u32> local;
            if (parsed) {
                const formats::M2Model& m = parsed.value();
                auto channel = [&](const char* name, u16 interpolation, i16 global, usize ranges, usize keys) {
                    if (keys == 0) {
                        return;
                    }
                    ++local[std::string(name) + " interpolation " + std::to_string(interpolation)];
                    if (global >= 0) {
                        ++local[std::string(name) + " global sequence"];
                        if (static_cast<usize>(global) >= m.globalSequences.size()) {
                            ++local["global sequence out of range"];
                        }
                    } else if (ranges < m.sequences.size()) {
                        ++local[std::string(name) + " fewer ranges than sequences"];
                    }
                };
                for (const formats::M2Bone& b : m.bones) {
                    channel("T", b.translation.interpolation, b.translation.globalSequence, b.translation.ranges.size(),
                            b.translation.times.size());
                    channel("R", b.rotation.interpolation, b.rotation.globalSequence, b.rotation.ranges.size(),
                            b.rotation.times.size());
                    channel("S", b.scale.interpolation, b.scale.globalSequence, b.scale.ranges.size(),
                            b.scale.times.size());
                    local["billboard bones"] += (b.flags & formats::kM2BoneSphericalBillboard) ? 1 : 0;
                }
                for (const formats::M2Texture& t : m.textures) {
                    ++local["texture type " + std::to_string(t.type)];
                }
                for (const formats::M2Batch& b : m.batches) {
                    local["batches"] += 1;
                    local["batches alpha 0"] += b.alpha <= 0.001f ? 1 : 0;
                    if (b.alpha <= 0.001f && std::getenv("VD_SCAN_VERBOSE")) {
                        std::printf("alpha 0: %s geoset %u\n", paths[i].c_str(), b.geosetId);
                    }
                    local["batches alpha (0,1)"] += b.alpha > 0.001f && b.alpha < 0.999f ? 1 : 0;
                }
                local["models without bones"] += m.bones.empty() ? 1 : 0;
                local["models with more than 256 bones"] += m.bones.size() > 256 ? 1 : 0;
                // Cameras (the portraits' framing): types, animated tracks, each model's portrait camera.
                for (const formats::M2Camera& c : m.cameras) {
                    ++local["camera type " + std::to_string(c.type)];
                    local["camera animated position/target"] +=
                        c.position.times.size() > 1 || c.target.times.size() > 1;
                }
                if (!m.bones.empty()) {
                    ++local[m.camera(formats::kM2CameraPortrait) ? "models with a portrait camera"
                                                                 : "models with bones, no portrait camera"];
                }
                if (const formats::M2Camera* c = m.camera(formats::kM2CameraPortrait);
                    c && std::getenv("VD_SCAN_VERBOSE")) {
                    const formats::Vec3 p =
                        c->position.values.empty() ? formats::Vec3{} : c->position.values[0];
                    const formats::Vec3 t = c->target.values.empty() ? formats::Vec3{} : c->target.values[0];
                    std::printf("portrait %s: fov %.3f near %.3f far %.2f position (%.3f %.3f %.3f) + %zu keys "
                                "(%.3f %.3f %.3f) target (%.3f %.3f %.3f) + %zu keys (%.3f %.3f %.3f) roll %zu "
                                "keys | bounds z %.2f..%.2f\n",
                                paths[i].c_str(), c->fov, c->nearClip, c->farClip, c->positionBase[0],
                                c->positionBase[1], c->positionBase[2], c->position.times.size(), p[0], p[1],
                                p[2], c->targetBase[0], c->targetBase[1], c->targetBase[2],
                                c->target.times.size(), t[0], t[1], t[2], c->roll.times.size(),
                                m.boundsMin[2], m.boundsMax[2]);
                }
            }
            std::lock_guard<std::mutex> lock(mutex);
            if (!parsed) {
                failures.push_back(paths[i] + ": " + parsed.error().message);
                continue;
            }
            for (const auto& [key, value] : local) {
                counts[key] += value;
            }
            const formats::M2Model& m = parsed.value();
            if (m.bones.size() > maxBones) {
                maxBones = static_cast<u32>(m.bones.size());
                mostBones = paths[i];
            }
            maxSequences = std::max(maxSequences, static_cast<u32>(m.sequences.size()));
            maxAttachments = std::max(maxAttachments, static_cast<u32>(m.attachments.size()));
            maxGlobal = std::max(maxGlobal, static_cast<u32>(m.globalSequences.size()));
        }
    });
    jobs.shutdown();
    for (const auto& [key, value] : counts) {
        std::printf("%-34s %8u\n", key.c_str(), value);
    }
    for (const std::string& failure : failures) {
        std::printf("FAILED %s\n", failure.c_str());
    }
    std::printf("most bones %u (%s) | most sequences %u | most attachments %u | most global sequences %u\n", maxBones,
                mostBones.c_str(), maxSequences, maxAttachments, maxGlobal);
    u32 missing = 0;
    std::set<std::string> objectOnly;
    const std::vector<std::string> actors = actorModelPaths(mpq, missing, objectOnly);
    std::printf("actor models: %zu (%u creature models missing from the client; %zu of them game objects, baked only "
                "when they animate)\n",
                actors.size(), missing, objectOnly.size());
    VD_LOG_INFO("%zu M2, %zu failed, %.1f s", paths.size(), failures.size(), secondsBetween(start, Clock::now()));
    return failures.empty() ? 0 : 1;
}

// Phase 14: the M2 events of every model (or the --filter ones): per event id, the models and events carrying it,
// how many fire at times (else always on), how many loop over a global sequence, and their data words.
// VD_SCAN_VERBOSE=1: every event of the filtered models, with its times in the first sequences.
int commandScanEvents(const Options& options) {
    formats::MpqArchiveSet mpq;
    if (!openArchives(options, mpq)) {
        return 1;
    }
    const Clock::time_point start = Clock::now();
    const std::vector<std::string> paths = mpq.list("*.m2");
    const std::string filter = normalizeAssetPath(option(options, "filter"));
    const bool verbose = std::getenv("VD_SCAN_VERBOSE") != nullptr;
    struct IdStats {
        u32 models = 0;
        u32 events = 0;
        u32 timed = 0;
        u32 global = 0;
        u32 times = 0;
        std::map<u32, u32> data; // data word -> events
        std::string example;
    };
    std::mutex mutex;
    std::map<std::string, IdStats> stats;
    std::vector<std::string> failures;
    u32 scanned = 0, withEvents = 0;
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
            std::lock_guard<std::mutex> lock(mutex);
            if (!parsed) {
                failures.push_back(paths[i] + ": " + parsed.error().message);
                continue;
            }
            const formats::M2Model& m = parsed.value();
            ++scanned;
            withEvents += m.events.empty() ? 0 : 1;
            std::set<std::string> seen;
            for (const formats::M2Event& e : m.events) {
                std::string id(4, ' ');
                for (u32 k = 0; k < 4; ++k) {
                    const char c = static_cast<char>((e.id >> (8 * k)) & 0xFF);
                    id[k] = std::isprint(static_cast<unsigned char>(c)) ? c : '?';
                }
                IdStats& s = stats[id];
                s.models += seen.insert(id).second ? 1 : 0;
                ++s.events;
                s.timed += e.times.empty() ? 0 : 1;
                s.global += e.globalSequence >= 0 ? 1 : 0;
                s.times += static_cast<u32>(e.times.size());
                ++s.data[e.data];
                if (s.example.empty()) {
                    s.example = paths[i];
                }
                if (verbose) {
                    std::printf("%s: %s data %u bone %u pos (%.2f %.2f %.2f) global %d, %zu times:", paths[i].c_str(),
                                id.c_str(), e.data, e.bone, e.position[0], e.position[1], e.position[2],
                                e.globalSequence, e.times.size());
                    // By sequence window (the ranges of 1.12 event tracks are not reliable: the baker uses windows).
                    for (usize q = 0; q < m.sequences.size() && q < 16; ++q) {
                        const formats::M2Sequence& sq = m.sequences[q];
                        bool any = false;
                        for (const u32 t : e.times) {
                            if (t >= sq.start && t <= sq.end) {
                                if (!any) {
                                    std::printf(" [seq %zu anim %u/%u, %u ms:", q, sq.animationId, sq.variation,
                                                sq.end - sq.start);
                                }
                                std::printf(" %u", t - sq.start);
                                any = true;
                            }
                        }
                        std::printf(any ? "]" : "");
                    }
                    std::printf("\n");
                }
            }
        }
    });
    jobs.shutdown();
    std::printf("%-6s %7s %7s %7s %7s %8s  %s\n", "id", "models", "events", "timed", "global", "times",
                "data words (events) | example");
    for (const auto& [id, s] : stats) {
        std::string data;
        u32 shown = 0;
        for (const auto& [word, count] : s.data) {
            if (shown++ == 6) {
                data += " ... (" + std::to_string(s.data.size()) + " values)";
                break;
            }
            data += " " + std::to_string(word) + "(" + std::to_string(count) + ")";
        }
        std::printf("%-6s %7u %7u %7u %7u %8u %s | %s\n", id.c_str(), s.models, s.events, s.timed, s.global, s.times,
                    data.c_str(), s.example.c_str());
    }
    for (const std::string& failure : failures) {
        std::printf("FAILED %s\n", failure.c_str());
    }
    VD_LOG_INFO("%u M2 scanned, %u with events, %zu failed, %.1f s", scanned, withEvents, failures.size(),
                secondsBetween(start, Clock::now()));
    return failures.empty() ? 0 : 1;
}

} // namespace vd::baker

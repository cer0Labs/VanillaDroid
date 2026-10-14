#include "BakerCommon.h"

#include "bake/CinematicBaker.h"
#include "bake/GlueBaker.h"
#include "bake/PackWriter.h"
#include "core/Clock.h"
#include "core/Log.h"
#include "formats/M2.h"
#include "pack/GlueFormat.h"

#include <cstdio>
#include <string>
#include <vector>

namespace vd::baker {

// Phase 14: every camera model of the client (Cameras\*.m2, the ones CinematicCamera.dbc names) -> cinematics.vdpk.
// Phase 18: also the glue scenes (pack/GlueFormat.h).
int bakeCinematics(const formats::MpqArchiveSet& mpq, const std::filesystem::path& outPath) {
    const Clock::time_point start = Clock::now();
    bake::PackWriter writer;
    if (Result<void> created = writer.create(outPath, kBakerVersion); !created) {
        VD_LOG_ERROR("%s", created.error().message.c_str());
        return 1;
    }
    u32 failed = 0;
    for (const std::string& path : mpq.list("Cameras\\*.m2")) {
        Result<std::vector<u8>> bytes = mpq.read(path);
        Result<formats::M2Model> model =
            bytes ? formats::parseM2(bytes.value()) : Result<formats::M2Model>(bytes.error());
        Result<std::vector<u8>> blob =
            model ? bake::bakeCinematicCamera(model.value()) : Result<std::vector<u8>>(model.error());
        Result<void> added =
            blob ? writer.add(path, pack::AssetType::CinematicCamera, blob.value()) : Result<void>(blob.error());
        if (!added) {
            VD_LOG_WARN("%s: %s", path.c_str(), added.error().message.c_str());
            ++failed;
        }
    }
    // Phase 18: the glue screens' scenes (AccountLogin's Dark Portal, the races' places): their camera and lights.
    u32 glueScenes = 0;
    for (const std::string& path : mpq.list("Interface\\Glues\\Models\\*.m2")) {
        Result<std::vector<u8>> bytes = mpq.read(path);
        Result<formats::M2Model> model =
            bytes ? formats::parseM2(bytes.value()) : Result<formats::M2Model>(bytes.error());
        Result<std::vector<u8>> blob =
            model ? bake::bakeGlueScene(model.value()) : Result<std::vector<u8>>(model.error());
        Result<void> added =
            blob ? writer.add(path, pack::AssetType::GlueScene, blob.value()) : Result<void>(blob.error());
        if (!added) {
            VD_LOG_WARN("%s: %s", path.c_str(), added.error().message.c_str());
            ++failed;
            continue;
        }
        ++glueScenes;
    }
    if (Result<void> finished = writer.finish(); !finished) {
        VD_LOG_ERROR("%s", finished.error().message.c_str());
        return 1;
    }
    VD_LOG_INFO("cinematics.vdpk: %u cameras and %u glue scenes (%u failed), %.1f KB, %.1f s",
                writer.entryCount() - glueScenes, glueScenes, failed,
                static_cast<double>(writer.bytesWritten()) / 1e3, secondsBetween(start, Clock::now()));
    return 0;
}

// Phase 18: what the glue scenes hold (cameras, lights, sequences), to place the character and light the scene.
int commandScanGlue(const Options& options) {
    formats::MpqArchiveSet mpq;
    if (!openArchives(options, mpq)) {
        return 1;
    }
    for (const std::string& path : mpq.list("Interface\\Glues\\Models\\*.m2")) {
        Result<std::vector<u8>> bytes = mpq.read(path);
        Result<formats::M2Model> parsed =
            bytes ? formats::parseM2(bytes.value()) : Result<formats::M2Model>(bytes.error());
        if (!parsed) {
            VD_LOG_WARN("%s: %s", path.c_str(), parsed.error().message.c_str());
            continue;
        }
        const formats::M2Model& m = parsed.value();
        std::printf("%s: %zu vertices, %zu bones, %zu sequences (first %u..%u), radius %.1f, %zu attachments\n",
                    path.c_str(), m.vertices.size(), m.bones.size(), m.sequences.size(),
                    m.sequences.empty() ? 0 : m.sequences[0].start, m.sequences.empty() ? 0 : m.sequences[0].end,
                    m.boundsRadius, m.attachments.size());
        for (const formats::M2Camera& c : m.cameras) {
            const formats::Vec3 p0 = formats::sampleM2Track(c.position, 0, 0, formats::Vec3{0, 0, 0});
            const formats::Vec3 t0 = formats::sampleM2Track(c.target, 0, 0, formats::Vec3{0, 0, 0});
            std::printf("  camera type %d fov %.3f near %.2f far %.1f base (%.2f %.2f %.2f) -> (%.2f %.2f %.2f) "
                        "keys %zu/%zu first (%.2f %.2f %.2f) -> (%.2f %.2f %.2f)\n",
                        c.type, c.fov, c.nearClip, c.farClip, c.positionBase[0], c.positionBase[1],
                        c.positionBase[2], c.targetBase[0], c.targetBase[1], c.targetBase[2], c.position.values.size(),
                        c.target.values.size(), p0[0], p0[1], p0[2], t0[0], t0[1], t0[2]);
        }
        Result<std::vector<u8>> blob = bake::bakeGlueScene(m);
        if (blob) {
            const auto* h = reinterpret_cast<const pack::GlueSceneHeader*>(blob.value().data());
            const auto* lights = reinterpret_cast<const pack::GlueLight*>(blob.value().data() + h->lightsOffset);
            if (h->flags & pack::kGlueHasStand) {
                std::printf("  stand (%.2f %.2f %.2f) facing %.2f\n", h->stand[0], h->stand[1], h->stand[2],
                            h->standFacing);
            }
            for (u32 i = 0; i < h->lightCount; ++i) {
                const pack::GlueLight& l = lights[i];
                std::printf("  light %u type %u at (%.2f %.2f %.2f) dir (%.2f %.2f %.2f) ambient (%.2f %.2f %.2f) "
                            "diffuse (%.2f %.2f %.2f) attenuation %.1f..%.1f visible %u\n",
                            i, l.type, l.position[0], l.position[1], l.position[2], l.direction[0], l.direction[1],
                            l.direction[2], l.ambient[0], l.ambient[1], l.ambient[2], l.diffuse[0], l.diffuse[1],
                            l.diffuse[2], l.attenuationStart, l.attenuationEnd, l.visible);
            }
        }
        for (const formats::M2Attachment& a : m.attachments) {
            std::printf("  attachment %u bone %u at (%.2f %.2f %.2f)\n", a.id, a.bone, a.position[0], a.position[1],
                        a.position[2]);
        }
    }
    return 0;
}

} // namespace vd::baker

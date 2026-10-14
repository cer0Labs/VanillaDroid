#include "bake/GlueBaker.h"

#include "bake/ActorBaker.h"
#include "bake/BakeUtil.h"
#include "bake/CinematicBaker.h"
#include "bake/Portable.h"
#include "pack/GlueFormat.h"

#include <glm/glm.hpp>

#include <cmath>
#include <cstring>

namespace vd::bake {

Result<std::vector<u8>> bakeGlueScene(const formats::M2Model& model) {
    pack::GlueSceneHeader header{};
    header.version = pack::kGlueFormatVersion;
    header.sequenceMs = model.sequences.empty() ? 0 : model.sequences[0].end - model.sequences[0].start;
    header.boundsRadius = model.boundsRadius;
    const u32 start = model.sequences.empty() ? 0 : model.sequences[0].start;
    const std::vector<glm::mat4> pose = standPose(model);
    std::vector<pack::GlueLight> lights;
    for (const formats::M2Light& l : model.lights) {
        const auto timeOf = [&](i16 globalSequence) { return globalSequence >= 0 ? 0u : start; };
        pack::GlueLight out{};
        out.type = l.type;
        const glm::mat4 bone = l.bone >= 0 && static_cast<usize>(l.bone) < pose.size()
                                   ? pose[static_cast<usize>(l.bone)]
                                   : glm::mat4(1.0f);
        const glm::vec3 position = glm::vec3(bone * glm::vec4(l.position[0], l.position[1], l.position[2], 1.0f));
        const glm::vec3 direction = glm::normalize(glm::vec3(bone * glm::vec4(0.0f, 0.0f, 1.0f, 0.0f)));
        std::memcpy(out.position, &position[0], sizeof(out.position));
        std::memcpy(out.direction, &direction[0], sizeof(out.direction));
        const formats::Vec3 ambient = formats::sampleM2Track(l.ambientColor, 0, timeOf(l.ambientColor.globalSequence),
                                                             formats::Vec3{0.0f, 0.0f, 0.0f});
        const f32 ambientIntensity =
            formats::sampleM2Track(l.ambientIntensity, 0, timeOf(l.ambientIntensity.globalSequence), 0.0f);
        const formats::Vec3 diffuse = formats::sampleM2Track(l.diffuseColor, 0, timeOf(l.diffuseColor.globalSequence),
                                                             formats::Vec3{0.0f, 0.0f, 0.0f});
        const f32 diffuseIntensity =
            formats::sampleM2Track(l.diffuseIntensity, 0, timeOf(l.diffuseIntensity.globalSequence), 0.0f);
        for (u32 c = 0; c < 3; ++c) {
            out.ambient[c] = ambient[c] * ambientIntensity;
            out.diffuse[c] = diffuse[c] * diffuseIntensity;
        }
        out.attenuationStart =
            formats::sampleM2Track(l.attenuationStart, 0, timeOf(l.attenuationStart.globalSequence), 0.0f);
        out.attenuationEnd = formats::sampleM2Track(l.attenuationEnd, 0, timeOf(l.attenuationEnd.globalSequence), 0.0f);
        out.visible = l.visibility.values.empty() ? 1u : (l.visibility.values[0] != 0 ? 1u : 0u);
        lights.push_back(out);
    }
    // Attachment 0: where the character stands (CharacterSelect, CharacterCreate).
    for (const formats::M2Attachment& a : model.attachments) {
        if (a.id != 0) {
            continue;
        }
        const glm::mat4 bone = a.bone < pose.size() ? pose[a.bone] : glm::mat4(1.0f);
        const glm::vec3 stand = glm::vec3(bone * glm::vec4(a.position[0], a.position[1], a.position[2], 1.0f));
        const glm::vec3 front = glm::vec3(bone * glm::vec4(1.0f, 0.0f, 0.0f, 0.0f));
        std::memcpy(header.stand, &stand[0], sizeof(header.stand));
        header.standFacing = portableAtan2(front.y, front.x); // phase 22: alike on every platform
        header.flags |= pack::kGlueHasStand;
        break;
    }
    BlobWriter writer(sizeof(header));
    header.lightCount = static_cast<u32>(lights.size());
    header.lightsOffset = writer.append(lights);
    if (!model.cameras.empty()) {
        Result<std::vector<u8>> camera = bakeCinematicCamera(model);
        if (!camera) {
            return camera.error();
        }
        header.cameraSize = static_cast<u32>(camera.value().size());
        header.cameraOffset = writer.append(camera.value());
    }
    return writer.finish(header);
}

} // namespace vd::bake

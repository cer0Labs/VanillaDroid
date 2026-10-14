#include "app/Cinematics.h"

#include "core/Hash.h"
#include "core/Log.h"
#include "core/Profile.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <string>

namespace vd {
namespace {

// CinematicSequences.dbc (5875): 0 id, 1 sound, 2-9 cameras. CinematicCamera.dbc: 0 id, 1 model (".mdx"),
// 2 voice-over (SoundEntries), 3-5 origin, 6 facing.
constexpr u32 kSequenceFields = 10;
constexpr u32 kSequenceSoundField = 1;
constexpr u32 kSequenceFirstCameraField = 2;
constexpr u32 kCameraFields = 7;
constexpr u32 kCameraModelField = 1;
constexpr u32 kCameraVoiceField = 2;
constexpr u32 kCameraOriginField = 3;
constexpr u32 kCameraFacingField = 6;

bool table(const pack::PackReader& db, const char* name, u32 fields, db::DbcView& out) {
    const std::optional<pack::PackAsset> asset = db.find(std::string("DBFilesClient\\") + name + ".dbc");
    Result<db::DbcView> view = asset ? db::DbcView::parse(asset->data, asset->size) : Result<db::DbcView>(Error{"missing"});
    if (!view || view.value().fieldCount() != fields) {
        VD_LOG_WARN("%s.dbc: %s", name, view ? "unexpected layout" : view.error().message.c_str());
        return false;
    }
    out = view.value();
    return true;
}

// "Cameras\FlyByHuman.mdx" -> the baked camera's asset hash (its ".m2" path).
u64 cameraHash(std::string_view model) {
    std::string path(model);
    if (const usize dot = path.rfind('.'); dot != std::string::npos) {
        path.resize(dot);
    }
    path += ".m2";
    return assetPathHash(path);
}

} // namespace

void Cinematics::open(const std::filesystem::path& packsDir, const pack::PackReader& db) {
    close();
    const std::filesystem::path path = packsDir / "cinematics.vdpk";
    if (Result<void> opened = m_cameras.open(path); !opened) {
        VD_LOG_INFO("No cinematics (%s): intro cinematics are skipped", opened.error().message.c_str());
        return;
    }
    if (!table(db, "CinematicSequences", kSequenceFields, m_sequences) ||
        !table(db, "CinematicCamera", kCameraFields, m_cameraRows)) {
        m_cameras.close();
    }
}

void Cinematics::close() {
    m_active = false;
    m_shotCount = 0;
    for (Shot& shot : m_shots) {
        shot = Shot{};
    }
    m_cameras.close();
    m_sequences = {};
    m_cameraRows = {};
}

bool Cinematics::load(Shot& shot, u32 cameraId) const {
    const u32 row = m_cameraRows.findById(cameraId);
    if (row >= m_cameraRows.recordCount()) {
        return false;
    }
    const std::optional<pack::PackAsset> asset = m_cameras.find(cameraHash(m_cameraRows.getString(row, kCameraModelField)));
    if (!asset || asset->compression != pack::Compression::None ||
        !shot.camera.bind(asset->data, asset->size).ok() || shot.camera.durationMs() == 0) {
        VD_LOG_WARN("Cinematic camera %u (%.*s) not baked", cameraId,
                    static_cast<int>(m_cameraRows.getString(row, kCameraModelField).size()),
                    m_cameraRows.getString(row, kCameraModelField).data());
        return false;
    }
    shot.origin = glm::dvec3(m_cameraRows.getF32(row, kCameraOriginField), m_cameraRows.getF32(row, kCameraOriginField + 1),
                             m_cameraRows.getF32(row, kCameraOriginField + 2));
    shot.facing = m_cameraRows.getF32(row, kCameraFacingField);
    shot.voice = m_cameraRows.getU32(row, kCameraVoiceField);
    return true;
}

bool Cinematics::start(u32 sequence) {
    m_active = false;
    m_shotCount = 0;
    if (!available()) {
        return false;
    }
    const u32 row = m_sequences.findById(sequence);
    if (row >= m_sequences.recordCount()) {
        VD_LOG_WARN("Unknown cinematic sequence %u", sequence);
        return false;
    }
    for (u32 c = 0; c < kMaxCameras; ++c) {
        const u32 camera = m_sequences.getU32(row, kSequenceFirstCameraField + c);
        if (camera != 0 && load(m_shots[m_shotCount], camera)) {
            ++m_shotCount;
        }
    }
    if (m_shotCount == 0) {
        return false;
    }
    m_sequence = sequence;
    m_sequenceSound = m_sequences.getU32(row, kSequenceSoundField);
    m_shot = 0;
    m_elapsed = 0.0f;
    m_waited = 0.0f;
    m_flying = false;
    m_stopRequested = false;
    m_active = true;
    m_view = m_shots[0].camera.sample(0, m_shots[0].origin, m_shots[0].facing);
    VD_LOG_INFO("Cinematic %u: %u camera(s), %.1f s", sequence, m_shotCount,
                static_cast<f64>(m_shots[0].camera.durationMs()) / 1000.0);
    return true;
}

Cinematics::Step Cinematics::update(f32 dt, bool worldReady) {
    Step step;
    if (!m_active) {
        return step;
    }
    VD_PROFILE_ZONE("Cinematics::update");
    if (m_stopRequested) {
        m_active = false;
        step.finished = true;
        return step;
    }
    const Shot& shot = m_shots[m_shot];
    if (!m_flying) {
        // The camera waits at its first point while the world there streams in, then flies with its voice.
        m_waited += dt;
        if (worldReady || m_waited >= kMaxWaitSeconds) {
            m_flying = true;
            m_elapsed = 0.0f;
            step.voice = shot.voice != 0 ? shot.voice : (m_shot == 0 ? m_sequenceSound : 0);
            step.nextCamera = m_shot > 0;
        }
    } else {
        m_elapsed += dt;
    }
    const u32 ms = static_cast<u32>(m_elapsed * 1000.0f);
    if (m_flying && ms >= shot.camera.durationMs()) {
        if (m_shot + 1 >= m_shotCount) {
            m_active = false;
            step.finished = true;
            return step;
        }
        ++m_shot;
        m_flying = false;
        m_waited = 0.0f;
        m_elapsed = 0.0f;
        const Shot& next = m_shots[m_shot];
        m_view = next.camera.sample(0, next.origin, next.facing);
        return step;
    }
    m_view = shot.camera.sample(m_flying ? ms : 0, shot.origin, shot.facing);
    return step;
}

void Cinematics::apply(Camera& camera, f32 aspect) const {
    const glm::dvec3 toward = m_view.target - m_view.eye;
    const f64 length = glm::length(toward);
    camera.position = m_view.eye;
    if (length > 1e-6) {
        const glm::dvec3 d = toward / length;
        camera.yaw = static_cast<f32>(std::atan2(d.y, d.x));
        camera.pitch = static_cast<f32>(std::asin(std::clamp(d.z, -1.0, 1.0)));
    }
    camera.roll = m_view.roll;
    camera.fovY = verticalFov(m_view.fovDiagonal, aspect);
}

f32 Cinematics::barHeight(f32 width, f32 height) {
    if (height <= 0.0f || width / height <= 4.0f / 3.0f) {
        return 0.0f;
    }
    const f32 picture = std::min(width / 2.0f, height);
    return (height - picture) * 0.5f;
}

} // namespace vd

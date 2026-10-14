#pragma once

#include "core/Types.h"
#include "db/DbcView.h"
#include "pack/PackReader.h"
#include "world/Camera.h"
#include "world/CinematicCamera.h"

#include <glm/vec3.hpp>

#include <array>
#include <filesystem>

namespace vd {

// Phase 14: the cinematics the server starts (SMSG_TRIGGER_CINEMATIC: each race's intro flyby for a new character,
// a camera object's view), played like 1.12's CinematicFrame: CinematicSequences.dbc names up to eight
// CinematicCamera.dbc rows, each a camera model of cinematics.vdpk with its voice-over flown at the row's origin and
// facing, one after another; the interface is hidden and black bars cut the picture to 2:1 on wider screens; Escape
// (here also the phone's Back and a skip button that a tap shows) stops it. The game reports each camera after the
// first (CMSG_NEXT_CINEMATIC_CAMERA) and the end (CMSG_COMPLETE_CINEMATIC). Game thread; no allocation once open.
class Cinematics {
public:
    // cinematics.vdpk (missing: nothing ever plays) and the two tables of db.vdpk, which must stay open.
    void open(const std::filesystem::path& packsDir, const pack::PackReader& db);
    void close();
    bool available() const { return m_cameras.entryCount() > 0 && m_sequences.recordCount() > 0; }

    // Plays CinematicSequences `sequence`; false (nothing plays) when it is unknown or none of its cameras is baked.
    bool start(u32 sequence);
    // The player stopped it (Escape): the next update() finishes it.
    void stop() { m_stopRequested = m_active; }
    bool active() const { return m_active; }
    u32 sequence() const { return m_sequence; }

    struct Step {
        u32 voice = 0;           // a SoundEntries id to start now (a camera's voice-over, the sequence's sound)
        bool nextCamera = false; // a camera after the first began: CMSG_NEXT_CINEMATIC_CAMERA
        bool finished = false;   // it ended or was stopped: CMSG_COMPLETE_CINEMATIC, and the voice stops
    };
    // Advances by dt seconds. `worldReady`: the terrain under the camera is loaded; each camera's flight (and its
    // voice) waits for it, up to kMaxWaitSeconds.
    Step update(f32 dt, bool worldReady);
    // Where the camera is now (while active).
    const CinematicView& view() const { return m_view; }
    // `camera` set to the view, for a picture `aspect` (width / height) wide.
    void apply(Camera& camera, f32 aspect) const;

    // CinematicFrame_OnLoad: on a screen wider than 4:3 the picture is cut to 2:1 (or to the screen), by two black
    // bars of this height at the top and the bottom.
    static f32 barHeight(f32 width, f32 height);

    static constexpr u32 kMaxCameras = 8; // CinematicSequences' camera columns
    static constexpr f32 kMaxWaitSeconds = 4.0f;

private:
    struct Shot {
        CinematicCamera camera;
        glm::dvec3 origin{0.0};
        f32 facing = 0.0f;
        u32 voice = 0;
    };
    bool load(Shot& shot, u32 cameraId) const;

    pack::PackReader m_cameras;
    db::DbcView m_sequences;    // CinematicSequences.dbc: 0 id, 1 sound, 2-9 cameras
    db::DbcView m_cameraRows;   // CinematicCamera.dbc: 0 id, 1 model, 2 voice, 3-5 origin, 6 facing
    std::array<Shot, kMaxCameras> m_shots{};
    u32 m_shotCount = 0;
    u32 m_shot = 0;
    u32 m_sequence = 0;
    u32 m_sequenceSound = 0;
    f32 m_elapsed = 0.0f; // seconds into the current shot
    f32 m_waited = 0.0f;  // seconds waiting for the world before the shot's flight starts
    bool m_flying = false; // the current shot's flight started (its voice too)
    bool m_active = false;
    bool m_stopRequested = false;
    CinematicView m_view;
};

} // namespace vd

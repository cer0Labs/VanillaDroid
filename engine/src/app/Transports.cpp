#include "app/Transports.h"

#include "app/GameSession.h"
#include "core/DevEnv.h"
#include "core/Hash.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "game/UnitInfo.h"
#include "net/UpdateFields.gen.h"
#include "pack/ObjectCollision.h"
#include "physics/CollisionWorld.h"
#include "world/WorldObjects.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace vd {
namespace {

namespace uf = net::uf;

f32 floatValue(const game::Entity& e, u16 index) {
    const u32 bits = e.value(index);
    f32 value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// Model -> world 3x4 (rows) of a rotation about Z and a translation, scaled (WorldObjects' instance
// transform).
void transform(const glm::vec3& p, f32 orientation, f32 scale, f32 (&out)[12]) {
    const f32 c = std::cos(orientation) * scale, s = std::sin(orientation) * scale;
    const f32 m[12] = {c, -s, 0.0f, p.x, s, c, 0.0f, p.y, 0.0f, 0.0f, scale, p.z};
    std::memcpy(out, m, sizeof(m));
}

} // namespace

bool Transports::init(const pack::PackReader& db, const game::TaxiData* taxi,
                      const std::filesystem::path& packsDir) {
    m_taxi = taxi;
    m_animations.init(db);
    if (const std::optional<pack::PackAsset> asset = db.find("DBFilesClient\\GameObjectDisplayInfo.dbc")) {
        if (Result<db::DbcView> parsed = db::DbcView::parse(asset->data, asset->size)) {
            m_displays = parsed.value();
            for (u32 r = 0; r < m_displays.recordCount(); ++r) {
                m_displayRecords.emplace(m_displays.getU32(r, 0), r);
            }
        }
    }
    if (Result<void> opened = m_collisionPack.open(packsDir / pack::kObjectCollisionPack); !opened) {
        VD_LOG_WARN("No transport collision (vd_baker bake --only objects): %s",
                    opened.error().message.c_str());
    } else {
        m_reader.start("vd-objects");
        m_readerStarted = true;
    }
    m_transports.reserve(32);
    m_trace = VD_DEV_ENV("VD_TRACE_TRANSPORTS") != nullptr;
    if (const char* preview = VD_DEV_ENV("VD_TRANSPORT_PREVIEW"); preview && m_taxi) {
        u32 pathId = 0, displayId = 0, speed = 30, accel = 1;
        std::sscanf(preview, "%u:%u:%u:%u", &pathId, &displayId, &speed, &accel);
        const game::TaxiPathInfo* path = m_taxi->pathById(pathId);
        if (path &&
            m_preview.path.build(m_taxi->points(*path), static_cast<f32>(speed), static_cast<f32>(accel))) {
            m_preview.active = true;
            m_preview.displayId = displayId;
            // Start docked: the first time it stands still.
            for (u32 t = 0; t < m_preview.path.period(); t += 100) {
                if (!m_preview.path.at(t).moving) {
                    m_preview.firstStop = t;
                    break;
                }
            }
            const game::TransportPlacement docked = m_preview.path.at(m_preview.firstStop);
            VD_LOG_INFO(
                "Transport preview: path %u, display %u, cycle %u ms, docked at (%.1f, %.1f, %.1f) map %u",
                pathId, displayId, m_preview.path.period(), docked.position.x, docked.position.y,
                docked.position.z, docked.map);
        } else {
            VD_LOG_WARN("VD_TRANSPORT_PREVIEW=%s: no such path", preview);
        }
    }
    return !m_displayRecords.empty();
}

void Transports::shutdown(WorldObjects& objects, physics::CollisionWorld& collision) {
    clear(objects, collision);
    if (m_readerStarted) {
        m_reader.stop();
        m_readerStarted = false;
    }
    m_collisionPack.close();
}

void Transports::forget(Transport& t, WorldObjects& objects, physics::CollisionWorld& collision) {
    if (t.instance != WorldObjects::kNoInstance) {
        objects.removeDynamic(t.instance);
    }
    if (t.collision != physics::CollisionWorld::kNoObject) {
        collision.removeObject(t.collision);
    }
    t.instance = WorldObjects::kNoInstance;
    t.collision = physics::CollisionWorld::kNoObject;
}

void Transports::clear(WorldObjects& objects, physics::CollisionWorld& collision) {
    for (auto& [guid, t] : m_transports) {
        forget(t, objects, collision);
    }
    m_transports.clear();
}

void Transports::show(u64 guid, Transport& t, u32 displayId, f32 scale, WorldObjects& objects,
                      physics::CollisionWorld& collision) {
    f32 m[12];
    transform(t.placement.position, t.placement.orientation, scale, m);
    if (t.instance == WorldObjects::kNoInstance || t.displayId != displayId) {
        if (t.instance != WorldObjects::kNoInstance) {
            objects.removeDynamic(t.instance);
        }
        t.instance = objects.addDynamic(modelOf(displayId), m, isWmo(displayId));
        t.displayId = displayId;
    } else {
        objects.moveDynamic(t.instance, m);
    }
    // Its collision: asked once from the IO thread, then moved with it.
    if (!t.collisionAsked && m_readerStarted) {
        t.collisionAsked = true;
        if (const std::optional<pack::PackAsset> asset = m_collisionPack.find(collisionOf(displayId))) {
            if (!m_reader.request(guid, *asset)) {
                t.collisionAsked = false; // the queue is full: next frame
            }
        }
    }
    if (t.collision != physics::CollisionWorld::kNoObject) {
        collision.placeObject(t.collision, glm::dvec3(t.placement.position), t.placement.orientation);
    }
}

void Transports::updatePreview(WorldObjects& objects, physics::CollisionWorld& collision, u32 nowMs) {
    if (m_preview.startMs == 0) {
        m_preview.startMs = nowMs;
    }
    Transport& t = m_transports[kPreviewGuid];
    t.seen = m_frame;
    t.type = game::kGameObjectMoTransport;
    t.placement = m_preview.path.at(m_preview.firstStop + (nowMs - m_preview.startMs));
    t.placed = true;
    show(kPreviewGuid, t, m_preview.displayId, 1.0f, objects, collision);
}

u64 Transports::modelOf(u32 displayId) const {
    const auto found = m_displayRecords.find(displayId);
    return found == m_displayRecords.end()
               ? 0
               : assetPathHash(pack::objectModelPath(m_displays.getString(found->second, 1)));
}

bool Transports::isWmo(u32 displayId) const {
    const auto found = m_displayRecords.find(displayId);
    if (found == m_displayRecords.end()) {
        return false;
    }
    const std::string path = pack::objectModelPath(m_displays.getString(found->second, 1));
    return path.size() > 4 && path.compare(path.size() - 4, 4, ".wmo") == 0;
}

u64 Transports::collisionOf(u32 displayId) const {
    const auto found = m_displayRecords.find(displayId);
    return found == m_displayRecords.end()
               ? 0
               : assetPathHash(pack::objectCollisionPath(m_displays.getString(found->second, 1)));
}

bool Transports::place(GameSession& session, const game::Entity& e, Transport& t, u32 nowMs) {
    if (!e.hasTransportTime) {
        return false;
    }
    const u32 time = e.transportTime + (nowMs - e.transportTimeAt);
    t.timeBase = e.transportTime - e.transportTimeAt; // phase 18 (motion)
    if (t.type == game::kGameObjectMoTransport) {
        m_timeBases[t.entry] = e.transportTime - e.transportTimeAt;
        auto path = m_paths.find(t.entry);
        if (path == m_paths.end()) {
            // Its path, speed and acceleration are in its template (GAMEOBJECT_TYPE_MO_TRANSPORT data 0..2).
            const game::GameObjectInfo* info = session.gameObject(t.entry);
            const game::TaxiPathInfo* taxiPath = info && m_taxi ? m_taxi->pathById(info->data[0]) : nullptr;
            if (!info) {
                return false; // asked: placed once it arrives
            }
            game::TransportPath built;
            if (!taxiPath || !built.build(m_taxi->points(*taxiPath), static_cast<f32>(info->data[1]),
                                          static_cast<f32>(info->data[2]))) {
                VD_LOG_WARN("Transport %u: no usable path %u (speed %u, acceleration %u)", t.entry,
                            info->data[0], info->data[1], info->data[2]);
            } else {
                VD_LOG_INFO("Transport %u (%s): path %u, %u frames, cycle %u ms", t.entry, info->name.c_str(),
                            info->data[0], built.keyFrameCount(), built.period());
            }
            path = m_paths.emplace(t.entry, std::move(built)).first;
        }
        if (!path->second.valid()) {
            return false;
        }
        t.placement = path->second.at(time);
        return t.placement.map == session.map();
    }
    // An elevator: its offset from its own place (ElevatorTransport::Update: the y axis flipped, "vanilla/tbc
    // only").
    glm::vec3 offset(0.0f);
    if (!m_animations.offsetAt(t.entry, time, offset)) {
        return false;
    }
    offset.y = -offset.y;
    t.origin = e.position; // phase 18 (motion)
    t.facing = floatValue(e, uf::GAMEOBJECT_FACING);
    t.placement.map = session.map();
    t.placement.position = e.position + offset;
    t.placement.orientation = t.facing;
    t.placement.moving = true;
    return true;
}

void Transports::update(GameSession& session, WorldObjects& objects, physics::CollisionWorld& collision,
                        u32 nowMs) {
    VD_PROFILE_ZONE("Transports::update");
    if (!session.inWorld() && !m_preview.active) {
        clear(objects, collision);
        return;
    }
    ++m_frame;
    // Collision blobs that finished loading.
    pack::AsyncReader::Done done;
    while (m_readerStarted && m_reader.poll(done)) {
        const auto found = m_transports.find(done.tag);
        if (done.data && found != m_transports.end() &&
            found->second.collision == physics::CollisionWorld::kNoObject) {
            found->second.collision = collision.addObject(std::vector<u8>(done.data, done.data + done.size));
            if (found->second.collision == physics::CollisionWorld::kNoObject) {
                VD_LOG_WARN("Transport %llx: invalid collision", static_cast<unsigned long long>(done.tag));
            }
        }
        delete[] done.data;
    }
    if (!session.inWorld()) {
        updatePreview(objects, collision, nowMs);
        return;
    }
    session.entities().forEach([&](const game::Entity& e) {
        if (e.type != game::ObjectType::GameObject) {
            return;
        }
        const u32 type = game::gameObjectType(e);
        if (type != game::kGameObjectTransport && type != game::kGameObjectMoTransport) {
            return;
        }
        auto [it, added] = m_transports.try_emplace(e.guid);
        Transport& t = it->second;
        t.seen = m_frame;
        t.entry = e.value(uf::OBJECT_FIELD_ENTRY);
        t.type = type;
        const u32 displayId = e.value(uf::GAMEOBJECT_DISPLAYID);
        if (added) {
            VD_LOG_INFO("Transport %llx: entry %u, type %u, display %u, time %u",
                        static_cast<unsigned long long>(e.guid), t.entry, type, displayId, e.transportTime);
        }
        t.placed = place(session, e, t, nowMs);
        if (!t.placed) {
            if (t.instance != WorldObjects::kNoInstance) {
                objects.removeDynamic(t.instance);
                t.instance = WorldObjects::kNoInstance;
            }
            return;
        }
        const f32 scale =
            e.value(uf::OBJECT_FIELD_SCALE_X) != 0 ? floatValue(e, uf::OBJECT_FIELD_SCALE_X) : 1.0f;
        show(e.guid, t, displayId, scale, objects, collision);
        if (m_trace && static_cast<i32>(nowMs - m_traceAtMs) >= 0) {
            const std::optional<glm::vec3> me = session.playerPosition(nowMs);
            VD_LOG_INFO("Transport %u at (%.1f, %.1f, %.1f) map %u, moving %d, %.0f yd from the player", t.entry,
                        t.placement.position.x, t.placement.position.y, t.placement.position.z, t.placement.map,
                        t.placement.moving ? 1 : 0, me ? glm::length(*me - t.placement.position) : -1.0f);
        }
    });
    if (m_trace && static_cast<i32>(nowMs - m_traceAtMs) >= 0) {
        m_traceAtMs = nowMs + 2000;
    }
    // Gone (out of range, another map): their model and collision go too.
    for (auto it = m_transports.begin(); it != m_transports.end();) {
        if (it->second.seen != m_frame) {
            forget(it->second, objects, collision);
            it = m_transports.erase(it);
        } else {
            ++it;
        }
    }
}

bool Transports::placement(u64 guid, game::TransportPlacement& out) const {
    const auto found = m_transports.find(guid);
    if (found == m_transports.end() || !found->second.placed) {
        return false;
    }
    out = found->second.placement;
    return true;
}

bool Transports::placementOfEntry(u32 entry, u32 nowMs, game::TransportPlacement& out) const {
    const auto path = m_paths.find(entry);
    const auto base = m_timeBases.find(entry);
    if (path == m_paths.end() || base == m_timeBases.end() || !path->second.valid()) {
        return false;
    }
    out = path->second.at(base->second + nowMs);
    return true;
}

bool Transports::solid(u64 guid) const {
    const auto found = m_transports.find(guid);
    return found != m_transports.end() && found->second.placed &&
           found->second.collision != physics::CollisionWorld::kNoObject;
}

u64 Transports::transportOfObject(u32 collisionObject) const {
    for (const auto& [guid, t] : m_transports) {
        if (t.collision == collisionObject && collisionObject != physics::CollisionWorld::kNoObject) {
            return guid;
        }
    }
    return 0;
}

bool Transports::placementAt(u64 guid, u32 nowMs, game::TransportPlacement& out) const {
    if (guid == kPreviewGuid && m_preview.active) {
        out = m_preview.path.at(m_preview.firstStop + (nowMs - m_preview.startMs));
        return true;
    }
    const auto found = m_transports.find(guid);
    if (found == m_transports.end() || !found->second.placed) {
        return false;
    }
    const Transport& t = found->second;
    if (t.type == game::kGameObjectMoTransport) {
        const auto path = m_paths.find(t.entry);
        if (path == m_paths.end() || !path->second.valid()) {
            return false;
        }
        out = path->second.at(t.timeBase + nowMs);
        return out.map == t.placement.map;
    }
    glm::vec3 offset(0.0f);
    if (!m_animations.offsetAt(t.entry, t.timeBase + nowMs, offset)) {
        return false;
    }
    out = t.placement;
    out.position = t.origin + glm::vec3(offset.x, -offset.y, offset.z);
    return true;
}

} // namespace vd

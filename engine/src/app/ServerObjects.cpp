#include "app/ServerObjects.h"

#include "app/GameSession.h"
#include "core/Hash.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "net/UpdateFields.gen.h"
#include "pack/ObjectCollision.h"
#include "pack/PackReader.h"
#include "physics/CollisionWorld.h"
#include "world/Actors.h"
#include "world/WorldObjects.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <string>

namespace vd {
namespace {

namespace uf = net::uf;

f32 floatValue(const game::Entity& e, u16 index) {
    const u32 bits = e.value(index);
    f32 value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

constexpr f32 kBobberTapRadius = 1.5f; // phase 13: yards around the fishing bobber a tap takes

// translate * rotate(quaternion x, y, z, w) * scale, 3x4 by rows (WorldObjects' instance transform).
void placement(const glm::vec3& p, f32 qx, f32 qy, f32 qz, f32 qw, f32 s, f32 (&out)[12]) {
    const f32 r[9] = {1.0f - 2.0f * (qy * qy + qz * qz), 2.0f * (qx * qy - qz * qw),        2.0f * (qx * qz + qy * qw),
                      2.0f * (qx * qy + qz * qw),        1.0f - 2.0f * (qx * qx + qz * qz), 2.0f * (qy * qz - qx * qw),
                      2.0f * (qx * qz - qy * qw),        2.0f * (qy * qz + qx * qw),        1.0f - 2.0f * (qx * qx + qy * qy)};
    for (u32 row = 0; row < 3; ++row) {
        out[row * 4 + 0] = r[row * 3 + 0] * s;
        out[row * 4 + 1] = r[row * 3 + 1] * s;
        out[row * 4 + 2] = r[row * 3 + 2] * s;
        out[row * 4 + 3] = p[static_cast<glm::length_t>(row)];
    }
}

} // namespace

bool ServerObjects::init(const pack::PackReader& db) {
    const std::optional<pack::PackAsset> asset = db.find("DBFilesClient\\GameObjectDisplayInfo.dbc");
    if (!asset) {
        VD_LOG_WARN("GameObjectDisplayInfo.dbc missing: server objects are not drawn");
        return false;
    }
    Result<db::DbcView> parsed = db::DbcView::parse(asset->data, asset->size);
    if (!parsed) {
        VD_LOG_WARN("GameObjectDisplayInfo.dbc: %s", parsed.error().message.c_str());
        return false;
    }
    m_displays = parsed.value();
    m_displayRecords.reserve(m_displays.recordCount());
    for (u32 r = 0; r < m_displays.recordCount(); ++r) {
        m_displayRecords.emplace(m_displays.getU32(r, 0), r);
    }
    m_instances.reserve(512);
    return true;
}

u64 ServerObjects::modelOf(u32 displayId) const {
    const auto found = m_displayRecords.find(displayId);
    if (found == m_displayRecords.end()) {
        return 0;
    }
    // Field 1: the model, ".mdx" (the client loads the ".m2"); WMOs (transports) are not drawn this way.
    const std::string_view path = m_displays.getString(found->second, 1);
    const usize dot = path.find_last_of('.');
    if (dot == std::string_view::npos || path.size() < 5) {
        return 0;
    }
    const std::string_view extension = path.substr(dot);
    if (extension.size() == 4 && (extension[1] | 0x20) == 'w') {
        return 0;
    }
    u64 hash = kFnvOffset;
    const auto add = [&hash](std::string_view s) {
        for (const char c : s) {
            hash ^= static_cast<u8>(normalizeAssetPathChar(c));
            hash *= kFnvPrime;
        }
    };
    add(path.substr(0, dot));
    add(".m2");
    return hash;
}

bool ServerObjects::animated(u32 displayId) {
    const auto found = m_animated.find(displayId);
    if (found != m_animated.end()) {
        return found->second;
    }
    // Phase 13: the actors' pack holds the game object models with animations of their own (vd_baker bake --only
    // actors); the rest stay static doodads.
    const u64 model = modelOf(displayId);
    const bool actor = m_actors && m_actors->isOpen() && model != 0 && m_actors->hasModel(model);
    m_animated.emplace(displayId, actor);
    return actor;
}

void ServerObjects::setCollision(physics::CollisionWorld* world, const std::filesystem::path& packsDir) {
    m_collision = world;
    if (!world) {
        return;
    }
    if (Result<void> opened = m_collisionPack.open(packsDir / pack::kObjectCollisionPack); !opened) {
        VD_LOG_WARN("Doors do not block (no %s): %s", pack::kObjectCollisionPack, opened.error().message.c_str());
        m_collision = nullptr;
    }
}

void ServerObjects::shutdown(WorldObjects& objects) {
    clear(objects);
    m_demos.clear();
    if (m_readerStarted) {
        m_reader.stop();
        m_readerStarted = false;
    }
    m_collisionPack.close();
    m_collision = nullptr;
}

u64 ServerObjects::collisionOf(u32 displayId) const {
    const auto found = m_displayRecords.find(displayId);
    return found == m_displayRecords.end()
               ? 0
               : assetPathHash(pack::objectCollisionPath(m_displays.getString(found->second, 1)));
}

void ServerObjects::updateCollision(u64 guid, Placed& placed) {
    if (!m_collision || placed.type != game::kGameObjectDoor || placed.gone) {
        return;
    }
    // Closed (GO_STATE_READY) it collides; open or broken it lets pass. The baked collision cannot be scaled: a
    // door of another size than its model's does not block.
    const bool blocks = placed.state == game::kGoStateReady && std::abs(placed.scale - 1.0f) < 0.01f;
    if (!blocks) {
        if (placed.collision != kNone) {
            m_collision->removeObject(placed.collision);
            placed.collision = kNone;
        }
        placed.collisionAsked = false;
        return;
    }
    if (placed.collision != kNone || placed.collisionAsked) {
        return;
    }
    const std::optional<pack::PackAsset> asset = m_collisionPack.find(collisionOf(placed.displayId));
    if (!asset) {
        placed.collisionAsked = true; // nothing in it to collide with
        return;
    }
    if (!m_readerStarted) {
        m_reader.start("vd-doors");
        m_readerStarted = true;
    }
    placed.collisionAsked = m_reader.request(guid, *asset); // the queue full: next frame
}

void ServerObjects::takeCollision() {
    pack::AsyncReader::Done done;
    while (m_readerStarted && m_reader.poll(done)) {
        const auto found = m_instances.find(done.tag);
        if (done.data && m_collision && found != m_instances.end() && found->second.collision == kNone &&
            found->second.collisionAsked && found->second.state == game::kGoStateReady) {
            Placed& placed = found->second;
            placed.collision = m_collision->addObject(std::vector<u8>(done.data, done.data + done.size));
            if (placed.collision != kNone) {
                m_collision->placeObject(placed.collision, glm::dvec3(placed.x, placed.y, placed.z), placed.facing);
            }
        } else if (found != m_instances.end()) {
            found->second.collisionAsked = false; // it changed meanwhile: asked again if it still should
        }
        delete[] done.data;
    }
}

void ServerObjects::release(Placed& placed, WorldObjects& objects) {
    if (placed.collision != kNone && m_collision) {
        m_collision->removeObject(placed.collision);
        placed.collision = kNone;
    }
    placed.collisionAsked = false;
    if (placed.instance != kNone) {
        objects.removeDynamic(placed.instance);
        placed.instance = kNone;
    }
    if (placed.actor != kNone && m_actors) {
        m_actors->destroy(placed.actor);
        placed.actor = kNone;
    }
}

void ServerObjects::animate(Placed& placed) {
    if (!m_actors || placed.actor == kNone || !m_actors->ready(placed.actor)) {
        return; // nothing to ask the model yet: its state is kept for when it is there
    }
    struct Query {
        const Actors* actors;
        u32 actor;
    } query{m_actors, placed.actor};
    // A one-shot is over once the model plays it (the switch happens at the next collect) and reached its end.
    const bool oneShotDone = !placed.playing.loop && m_actors->playing(placed.actor) == placed.playing.animation &&
                             m_actors->animationFinished(placed.actor);
    const game::GameObjectAnimRequest request = placed.animator.next(
        [](const void* context, u16 animation) {
            const Query* q = static_cast<const Query*>(context);
            return q->actors->hasAnimation(q->actor, animation);
        },
        &query, oneShotDone);
    if (!(request == placed.playing)) {
        m_actors->play(placed.actor, request.animation, request.loop, 1.0f);
        placed.playing = request;
    }
}

void ServerObjects::readFx(const GameSession& session) {
    const game::Gameplay::FxEvents& fx = session.play().fx();
    if (m_fxPlayer != session.playerGuid()) { // another character: its events from now on
        m_fxPlayer = session.playerGuid();
        m_fxCursor = fx.end();
    }
    while (const game::FxEvent* e = fx.next(m_fxCursor)) {
        const bool object = e->kind == game::FxEventKind::ObjectCustomAnim ||
                            e->kind == game::FxEventKind::ObjectSpawnAnim ||
                            e->kind == game::FxEventKind::ObjectDespawnAnim ||
                            e->kind == game::FxEventKind::ObjectResetState;
        const auto found = object ? m_instances.find(e->guid) : m_instances.end();
        if (found == m_instances.end()) {
            continue;
        }
        game::GameObjectAnimator& animator = found->second.animator;
        switch (e->kind) {
        case game::FxEventKind::ObjectCustomAnim: animator.custom(e->id); break;
        case game::FxEventKind::ObjectSpawnAnim: animator.spawn(); break;
        case game::FxEventKind::ObjectDespawnAnim: animator.despawn(); break;
        default: animator.resetState(); break;
        }
    }
}

void ServerObjects::update(const GameSession& session, WorldObjects& objects, u32 nowMs) {
    VD_PROFILE_ZONE("ServerObjects::update");
    updateDemos(nowMs);
    if (!objects.isOpen() || m_displayRecords.empty()) {
        return;
    }
    if (!session.inWorld()) {
        clear(objects);
        return;
    }
    ++m_frame;
    session.entities().forEach([&](const game::Entity& e) {
        if (e.type != game::ObjectType::GameObject) {
            return;
        }
        if (const u32 type = e.value(uf::GAMEOBJECT_TYPE_ID); type == 11 || type == 15) {
            return; // elevators, ships, zeppelins: moved by app/Transports (phase 8)
        }
        const u32 displayId = e.value(uf::GAMEOBJECT_DISPLAYID);
        const f32 scale = e.value(uf::OBJECT_FIELD_SCALE_X) != 0 ? floatValue(e, uf::OBJECT_FIELD_SCALE_X) : 1.0f;
        const f32 facing = floatValue(e, uf::GAMEOBJECT_FACING);
        auto [it, added] = m_instances.try_emplace(e.guid);
        Placed& placed = it->second;
        placed.seen = m_frame;
        placed.type = game::gameObjectType(e);
        if (placed.gone) { // it came back (a respawn under the same GUID) while its despawn still played
            release(placed, objects);
            placed = Placed{};
            placed.seen = m_frame;
            added = true;
        }
        // Phase 13: an animated model follows the object's state (its transition first when it changes).
        const u32 state = e.value(uf::GAMEOBJECT_STATE);
        if (placed.state != state) {
            if (added || placed.state == 0xFFFFFFFFu) {
                placed.animator.appear(state);
            } else {
                placed.animator.changeState(state);
            }
            placed.state = state;
        }
        const bool moved = added || placed.displayId != displayId || placed.x != e.position.x ||
                           placed.y != e.position.y || placed.z != e.position.z || placed.facing != facing ||
                           placed.scale != scale;
        if (!moved) {
            return;
        }
        const bool newModel = added || placed.displayId != displayId;
        if (newModel) {
            release(placed, objects);
            placed.playing = {game::goanim::kNone, true};
        }
        if (animated(displayId)) {
            // Phase 13: an actor (upright: animated objects are not tilted), its look its display's M2.
            if (newModel) {
                placed.actor = m_actors->create();
                game::ActorLook look;
                look.model = modelOf(displayId);
                m_actors->setLook(placed.actor, look);
            }
            m_actors->setPlacement(placed.actor, glm::dvec3(e.position), facing, scale);
        } else {
            // Its rotation: the quaternion when the server sends one (tilted objects), else the facing about Z.
            f32 q[4] = {floatValue(e, uf::GAMEOBJECT_ROTATION), floatValue(e, uf::GAMEOBJECT_ROTATION + 1),
                        floatValue(e, uf::GAMEOBJECT_ROTATION + 2), floatValue(e, uf::GAMEOBJECT_ROTATION + 3)};
            const f32 length = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
            if (length < 0.5f || length > 1.5f || !std::isfinite(length)) {
                q[0] = q[1] = 0.0f;
                q[2] = std::sin(facing * 0.5f);
                q[3] = std::cos(facing * 0.5f);
            } else {
                for (f32& c : q) {
                    c /= length;
                }
            }
            f32 transform[12];
            placement(e.position, q[0], q[1], q[2], q[3], scale, transform);
            if (newModel) {
                placed.instance = objects.addDynamic(modelOf(displayId), transform);
            } else if (placed.instance != kNone) {
                objects.moveDynamic(placed.instance, transform);
            }
        }
        placed.displayId = displayId;
        placed.x = e.position.x;
        placed.y = e.position.y;
        placed.z = e.position.z;
        placed.facing = facing;
        placed.scale = scale;
        if (placed.collision != kNone && m_collision) {
            m_collision->placeObject(placed.collision, glm::dvec3(e.position), facing); // phase 13: a door moved
        }
    });
    readFx(session); // after the states: the bobber's bite sets its state, then plays Custom0
    takeCollision(); // phase 13: doors' collision that arrived
    for (auto it = m_instances.begin(); it != m_instances.end();) {
        Placed& placed = it->second;
        if (placed.seen != m_frame) {
            // Gone from the world (despawned, out of sight); a despawn animation plays to its end first (5 s at most).
            if (!placed.gone && placed.actor != kNone && placed.animator.despawning()) {
                placed.gone = true;
                placed.goneMs = nowMs;
            }
            const bool ended = !placed.gone || nowMs - placed.goneMs > 5000 ||
                               (placed.playing.animation == game::goanim::kDespawn &&
                                m_actors->playing(placed.actor) == game::goanim::kDespawn &&
                                m_actors->animationFinished(placed.actor));
            if (ended) {
                release(placed, objects);
                it = m_instances.erase(it);
                continue;
            }
        }
        animate(placed);
        updateCollision(it->first, placed);
        ++it;
    }
}

void ServerObjects::collectViews(GameSession& session, const WorldObjects& objects,
                                 std::vector<WorldObjectView>& out) const {
    out.clear();
    for (const auto& [guid, placed] : m_instances) {
        std::optional<glm::vec4> sphere;
        if (placed.instance != kNone) {
            sphere = objects.instanceSphere(placed.instance);
        } else if (placed.actor != kNone && m_actors && !placed.gone) {
            sphere = m_actors->bounds(placed.actor); // phase 13: animated objects
        }
        if (!sphere) {
            continue;
        }
        const game::Entity* e = session.entities().find(guid);
        f32 radius = sphere->w;
        if (e && game::gameObjectType(*e) == game::kGameObjectFishingNode) {
            radius = std::max(radius, kBobberTapRadius); // phase 13: the bobber is tiny; it must be easy to tap
        }
        out.push_back({guid, glm::dvec3(sphere->x, sphere->y, sphere->z), radius});
        if (e && worldObjectTappable(*e)) {
            session.gameObject(e->value(uf::OBJECT_FIELD_ENTRY));
        }
    }
}

void ServerObjects::clear(WorldObjects& objects) {
    for (auto& [guid, placed] : m_instances) {
        release(placed, objects);
    }
    m_instances.clear();
}

// -------------------------------------------------------------------------------------------------------
// Phase 13: development objects without a server.

bool ServerObjects::spawnDemo(u32 displayId, const glm::dvec3& position, f32 facing, u32 nowMs) {
    if (!animated(displayId)) {
        VD_LOG_WARN("gameobject:%u: no animated model for that display in actors.vdpk", displayId);
        return false;
    }
    Demo& demo = m_demos.emplace_back();
    demo.placed.displayId = displayId;
    demo.placed.actor = m_actors->create();
    game::ActorLook look;
    look.model = modelOf(displayId);
    m_actors->setLook(demo.placed.actor, look);
    m_actors->setPlacement(demo.placed.actor, position, facing, 1.0f);
    demo.placed.animator.appear(game::kGoStateReady);
    demo.nextMs = nowMs + 3000;
    VD_LOG_INFO("gameobject:%u: spawned closed (GO_STATE_READY)", displayId);
    return true;
}

void ServerObjects::updateDemos(u32 nowMs) {
    for (Demo& demo : m_demos) {
        if (static_cast<i32>(nowMs - demo.nextMs) >= 0) {
            // What the server does to objects, one step every 3 seconds: open (a door used, a chest looted), its
            // Custom0 (the bobber's bite), closed again, destroyed (GO_STATE_ACTIVE_ALTERNATIVE), rebuilt.
            demo.nextMs = nowMs + 3000;
            demo.step = demo.step % 5 + 1;
            game::GameObjectAnimator& a = demo.placed.animator;
            const char* what = "";
            switch (demo.step) {
            case 1:
                a.changeState(game::kGoStateActive);
                what = "open (GO_STATE_ACTIVE)";
                break;
            case 2:
                a.custom(0);
                what = "SMSG_GAMEOBJECT_CUSTOM_ANIM 0";
                break;
            case 3:
                a.changeState(game::kGoStateReady);
                what = "closed (GO_STATE_READY)";
                break;
            case 4:
                a.changeState(game::kGoStateActiveAlternative);
                what = "destroyed (GO_STATE_ACTIVE_ALTERNATIVE)";
                break;
            default:
                a.changeState(game::kGoStateReady);
                what = "rebuilt (GO_STATE_READY)";
                break;
            }
            VD_LOG_INFO("gameobject:%u: %s", demo.placed.displayId, what);
        }
        const u16 before = demo.placed.playing.animation;
        animate(demo.placed);
        if (demo.placed.playing.animation != before) {
            VD_LOG_INFO("gameobject:%u: plays animation %u%s", demo.placed.displayId, demo.placed.playing.animation,
                        demo.placed.playing.loop ? " (loop)" : " (once)");
        }
    }
}

} // namespace vd

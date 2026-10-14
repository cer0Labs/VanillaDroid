#include "game/Reactions.h"

#include "core/Log.h"
#include "db/DbcView.h"
#include "game/Abilities.h"
#include "pack/PackReader.h"

namespace vd::game {

bool Reactions::Template::friendlyTo(const Template& other) const {
    if (other.faction != 0) {
        for (const u32 enemy : enemies) {
            if (enemy == other.faction) {
                return false;
            }
        }
        for (const u32 f : friends) {
            if (f == other.faction) {
                return true;
            }
        }
    }
    return (friendlyMask & other.ourMask) != 0 || (ourMask & other.friendlyMask) != 0;
}

bool Reactions::Template::hostileTo(const Template& other) const {
    if (other.faction != 0) {
        for (const u32 enemy : enemies) {
            if (enemy == other.faction) {
                return true;
            }
        }
        for (const u32 f : friends) {
            if (f == other.faction) {
                return false;
            }
        }
    }
    return (hostileMask & other.ourMask) != 0;
}

void Reactions::init(const pack::PackReader& db) {
    const std::optional<pack::PackAsset> asset = db.find("DBFilesClient\\FactionTemplate.dbc");
    if (!asset) {
        VD_LOG_WARN("FactionTemplate.dbc missing: every unit looks neutral");
        return;
    }
    Result<db::DbcView> table = db::DbcView::parse(asset->data, asset->size);
    if (!table || table.value().fieldCount() < 14) {
        VD_LOG_WARN("FactionTemplate.dbc unreadable");
        return;
    }
    const db::DbcView& view = table.value();
    m_templates.reserve(view.recordCount());
    for (u32 r = 0; r < view.recordCount(); ++r) {
        Template t;
        t.faction = view.getU32(r, 1);
        t.ourMask = view.getU32(r, 3);
        t.friendlyMask = view.getU32(r, 4);
        t.hostileMask = view.getU32(r, 5);
        for (u32 i = 0; i < 4; ++i) {
            t.enemies[i] = view.getU32(r, 6 + i);
            t.friends[i] = view.getU32(r, 10 + i);
        }
        m_templates.emplace(view.getU32(r, 0), t);
    }
    // Phase 13: which factions have a reputation (Faction.dbc's reputationListID >= 0: FactionEntry::
    // CanHaveReputation), for the player's standing.
    if (const std::optional<pack::PackAsset> factions = db.find("DBFilesClient\\Faction.dbc")) {
        Result<db::DbcView> list = db::DbcView::parse(factions->data, factions->size);
        if (list && list.value().fieldCount() >= 2) {
            for (u32 r = 0; r < list.value().recordCount(); ++r) {
                const i32 listId = static_cast<i32>(list.value().getU32(r, 1));
                if (listId >= 0) {
                    m_factionList.emplace(list.value().getU32(r, 0), listId);
                }
            }
        }
    }
}

void Reactions::setTemplate(u32 id, u32 faction, u32 ourMask, u32 friendlyMask, u32 hostileMask) {
    Template t;
    t.faction = faction;
    t.ourMask = ourMask;
    t.friendlyMask = friendlyMask;
    t.hostileMask = hostileMask;
    m_templates[id] = t;
}

const PlayerStanding::Forced* Reactions::forced(u32 faction) const {
    if (m_standing != nullptr) {
        for (const PlayerStanding::Forced& f : m_standing->forced) {
            if (f.faction == faction) {
                return &f;
            }
        }
    }
    return nullptr;
}

bool Reactions::atWar(u32 faction) const {
    if (m_standing == nullptr || m_standing->reputation == nullptr) {
        return false;
    }
    const auto list = m_factionList.find(faction);
    return list != m_factionList.end() && static_cast<u32>(list->second) < m_standing->reputation->size() &&
           (m_standing->reputation->entry(static_cast<u32>(list->second)).flags & net::kFactionAtWar) != 0;
}

namespace {
// ReputationRank -> what the UI tells apart: hated and hostile attack (vMaNGOS IsHostileTo: rank <= REP_HOSTILE),
// unfriendly and neutral may be attacked, friendly and above are friends.
Reaction fromRank(u32 rank) {
    return rank <= 1 ? Reaction::Hostile : rank <= 3 ? Reaction::Neutral : Reaction::Friendly;
}
} // namespace

Reaction Reactions::playerTowards(u32 mine, u32 theirs) const {
    const auto target = m_templates.find(theirs);
    if (target != m_templates.end() && target->second.faction != 0) {
        if (const PlayerStanding::Forced* f = forced(target->second.faction)) {
            return fromRank(f->rank);
        }
        if (m_standing != nullptr && m_standing->reputation != nullptr &&
            m_factionList.count(target->second.faction) != 0) {
            // "if faction has reputation, hostile state depends only from AtWar state"
            return atWar(target->second.faction) ? Reaction::Hostile : Reaction::Friendly;
        }
    }
    return between(mine, theirs);
}

Reaction Reactions::towardsPlayer(u32 theirs, u32 player) const {
    const auto self = m_templates.find(theirs);
    if (self != m_templates.end() && self->second.faction != 0) {
        if (const PlayerStanding::Forced* f = forced(self->second.faction)) {
            return fromRank(f->rank);
        }
        // GetFactionReactionTo caps the player's rank with the faction at neutral while at war with it; the rank
        // itself (base + standing) only colours the reputation window, so at war is what tells hostile apart here.
        if (atWar(self->second.faction)) {
            return Reaction::Hostile;
        }
    }
    return between(theirs, player);
}

u32 Reactions::groupMask(u32 templateId) const {
    const auto found = m_templates.find(templateId);
    return found != m_templates.end() ? found->second.ourMask : 0;
}

Reaction Reactions::between(u32 mine, u32 theirs) const {
    const auto a = m_templates.find(mine);
    const auto b = m_templates.find(theirs);
    if (a == m_templates.end() || b == m_templates.end()) {
        return Reaction::Neutral;
    }
    if (b->second.hostileTo(a->second) || a->second.hostileTo(b->second)) {
        return Reaction::Hostile;
    }
    if (b->second.friendlyTo(a->second)) {
        return Reaction::Friendly;
    }
    return Reaction::Neutral;
}

} // namespace vd::game

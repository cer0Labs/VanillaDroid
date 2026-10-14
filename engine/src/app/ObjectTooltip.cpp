#include "app/ObjectTooltip.h"

#include "app/GameSession.h"
#include "app/Hud.h"
#include "app/UnitTooltip.h"
#include "core/Profile.h"
#include "core/Text.h"
#include "game/GameData.h"
#include "game/MeetingStone.h"
#include "game/Queries.h"
#include "net/UpdateFields.gen.h"
#include "ui/Widgets.h"

namespace vd {
namespace {

constexpr f32 kTooltipWidthDp = 270.0f; // as the unit tooltip's

} // namespace

void ObjectTooltip::actOnHud(GameSession& session, const HudOutput& out) {
    if (out.objectHold != 0 && session.inWorld()) {
        m_guid = out.objectHold;
    }
}

u32 ObjectTooltip::fill(GameSession& session) {
    const game::Entity* go = session.entities().find(m_guid);
    if (!go) {
        return 0;
    }
    const game::GameObjectInfo* info =
        session.play().queries().gameObject(go->value(net::uf::OBJECT_FIELD_ENTRY), m_guid);
    if (!info || !info->found || info->name.empty()) {
        return 0;
    }
    u32 count = 0;
    m_lines[count++] = {info->name, {}, game::kTooltipWhite};
    if (info->type == game::kGameObjectMeetingStone) {
        // 0x5F6B10: the stone's zone (when AreaTable has it), then "Level %d-%d".
        const std::string_view area = m_data ? m_data->areaName(info->data[2]) : std::string_view{};
        if (!area.empty()) {
            m_lines[count++] = {area, {}, game::kTooltipGold};
        }
        m_lines[count++] = {text::format(m_level, "MEETINGSTONE_LEVEL", info->data[0], info->data[1]),
                            {},
                            game::kTooltipGold};
    }
    return count;
}

void ObjectTooltip::draw(GameSession& session, DialogContext& c, const Hud& hud) {
    if (m_guid == 0) {
        return;
    }
    VD_PROFILE_ZONE("ObjectTooltip::draw");
    const u32 count = session.inWorld() ? fill(session) : 0;
    if (count == 0) {
        m_guid = 0; // gone, or nothing to say
        return;
    }
    ui::Gui g{c.ui, c.draw};
    HudTooltipView view;
    view.lines = {m_lines.data(), count};
    view.interactive = true; // a tap on it closes it
    const HudTooltipSize size = measureHudTooltip(g, view, g.px(kTooltipWidthDp));
    const ui::Rect r =
        defaultTooltipAnchor(g, c.safeArea, hud.actionBarRect(), c.safeArea.y + g.unit(200.0f), size);
    if (hudTooltip(g, ui::widgetId("objectTooltip"), r, view).closed) {
        m_guid = 0;
    }
}

} // namespace vd

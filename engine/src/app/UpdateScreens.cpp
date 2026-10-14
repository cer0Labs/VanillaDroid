#include "app/UpdateScreens.h"

#include "app/UpdateWindows.h"
#include "core/Profile.h"
#include "core/Text.h"

#include <algorithm>
#include <cstring>

namespace vd {
namespace {

using Stage = AppUpdates::Stage;
using Problem = AppUpdates::Problem;

std::string_view view(const std::array<char, 24>& text) {
    return text.data();
}
std::string_view view(const std::array<char, 160>& text) {
    return text.data();
}

// `first`, a line break and `second` (either may be empty) into `out`, cut to fit.
template <usize N>
std::string_view join(std::array<char, N>& out, std::string_view first, std::string_view second) {
    usize at = 0;
    const auto add = [&](std::string_view s) {
        const usize n = std::min(s.size(), N - 1 - at);
        std::memcpy(out.data() + at, s.data(), n);
        at += n;
    };
    add(first);
    if (!first.empty() && !second.empty()) {
        add("\n");
    }
    add(second);
    out[at] = '\0';
    return {out.data(), at};
}

} // namespace

UpdateScreens::Notice UpdateScreens::notice() const {
    switch (m_state.stage) {
    case Stage::Available:
    case Stage::TooNew:
    case Stage::NeedPermission:
    case Stage::Downloading:
    case Stage::Verifying:
    case Stage::Confirming:
    case Stage::Failed:
    case Stage::Downloaded: return Notice::Update;
    case Stage::Off:
    case Stage::Checking:
    case Stage::None:
    case Stage::Dismissed: break;
    }
    return m_state.outdatedData[0] != '\0' && !m_dataSeen ? Notice::Data : Notice::None;
}

bool UpdateScreens::wanted() {
    m_state = appUpdates().state();
    return notice() != Notice::None;
}

bool UpdateScreens::draw(DialogContext& c, GlueKey key) {
    VD_PROFILE_ZONE("UpdateScreens::draw");
    AppUpdates& updates = appUpdates();
    const Notice shown = notice();
    if (shown == Notice::None) {
        return false;
    }
    // The phone prepares the data itself when the player's WoW is on it and the phone baker is there (22.7).
    const bool phone = m_state.phoneHasWow && m_state.phoneBaker;
    std::array<char, 384> line{};
    UpdateDialogView v;
    if (shown == Notice::Data) {
        v.text = text::format(m_text, "VD_UPDATES_DATA_OUTDATED", view(m_state.outdatedData));
        v.detail = text::get(phone ? "VD_UPDATES_DATA_PHONE" : "VD_UPDATES_DATA_SETUP");
        v.buttons = {phone ? text::get("VD_UPDATES_DATA_PREPARE") : std::string_view{}, text::get("OKAY")};
        v.name = "glue.update.data";
    } else {
        v.name = "glue.update";
        const std::string_view version = view(m_state.version);
        switch (m_state.stage) {
        case Stage::Available: {
            v.text = text::format(m_text, "VD_UPDATES_AVAILABLE", version);
            const std::string_view data =
                m_state.releaseData[0] == '\0'
                    ? text::get("VD_UPDATES_DATA_SAME")
                    : text::format(line,
                                   phone ? "VD_UPDATES_DATA_CHANGES_PHONE" : "VD_UPDATES_DATA_CHANGES_PC",
                                   view(m_state.releaseData));
            v.detail = join(m_detail,
                            m_state.problem == Problem::Refused ? text::get("VD_UPDATES_REFUSED")
                                                                : std::string_view{},
                            data);
            v.buttons = {text::get("VD_UPDATES_UPDATE"), text::get("VD_UPDATES_NOTES"),
                         text::get("VD_UPDATES_LATER")};
            break;
        }
        case Stage::TooNew:
            v.text = text::format(m_text, "VD_UPDATES_TOO_NEW", version);
            v.buttons = {text::get("VD_UPDATES_NOTES"), text::get("VD_UPDATES_LATER")};
            break;
        case Stage::NeedPermission:
            v.text = text::get("VD_UPDATES_PERMISSION");
            v.buttons = {text::get("VD_UPDATES_OPEN_SETTINGS"), text::get("CANCEL")};
            break;
        case Stage::Downloading:
            v.text = text::format(m_text, "VD_UPDATES_DOWNLOADING", version);
            v.progress = true;
            v.fraction =
                m_state.total > 0
                    ? static_cast<f32>(static_cast<f64>(m_state.bytes) / static_cast<f64>(m_state.total))
                    : 0.0f;
            v.progressText =
                text::format(m_progress, "VD_UPDATES_PROGRESS", static_cast<f64>(m_state.bytes) / 1e6,
                             static_cast<f64>(m_state.total) / 1e6);
            v.buttons = {text::get("CANCEL")};
            break;
        case Stage::Verifying:
            v.text = text::get("VD_UPDATES_VERIFYING");
            v.progress = true;
            v.fraction = 1.0f;
            break;
        case Stage::Confirming:
            v.text = text::get("VD_UPDATES_CONFIRMING");
            v.buttons = {text::get("VD_UPDATES_LATER")};
            break;
        case Stage::Failed:
            v.text = text::get(m_state.problem == Problem::Network       ? "VD_UPDATES_FAILED_NETWORK"
                               : m_state.problem == Problem::Mismatch    ? "VD_UPDATES_FAILED_MISMATCH"
                               : m_state.problem == Problem::Unsupported ? "VD_UPDATES_FAILED_UNSUPPORTED"
                                                                         : "VD_UPDATES_FAILED_INSTALL");
            v.detail = view(m_state.detail);
            v.buttons = {m_state.problem == Problem::Unsupported ? std::string_view{}
                                                                 : text::get("VD_UPDATES_RETRY"),
                         text::get("VD_UPDATES_LATER")};
            break;
        case Stage::Downloaded:
            v.text = text::format(m_text, "VD_UPDATES_DOWNLOADED", version);
            v.detail = view(m_state.detail);
            v.buttons = {text::get("OKAY")};
            break;
        default: return false;
        }
    }
    i32 tapped = updateDialog(c, v).button;
    // Escape: the last button (Later, Cancel, OK); Enter: the first.
    if (tapped < 0 && key != GlueKey::None) {
        i32 first = -1, last = -1;
        for (u32 i = 0; i < v.buttons.size(); ++i) {
            if (!v.buttons[i].empty()) {
                first = first < 0 ? static_cast<i32>(i) : first;
                last = static_cast<i32>(i);
            }
        }
        tapped = key == GlueKey::Escape ? last : key == GlueKey::Enter ? first : -1;
    }
    if (tapped < 0) {
        return false;
    }
    if (shown == Notice::Data) {
        if (tapped == 0) {
            updates.bakeOnPhone();
        }
        m_dataSeen = true;
        return true;
    }
    switch (m_state.stage) {
    case Stage::Available:
        if (tapped == 0) {
            updates.update(text::format(m_title, "VD_UPDATES_DOWNLOAD_TITLE", view(m_state.version)));
        } else if (tapped == 1) {
            updates.openNotes();
        } else {
            updates.later();
        }
        break;
    case Stage::TooNew: tapped == 0 ? updates.openNotes() : updates.later(); break;
    case Stage::NeedPermission: tapped == 0 ? updates.openSettings() : updates.cancel(); break;
    case Stage::Downloading: updates.cancel(); break;
    case Stage::Confirming: updates.later(); break;
    case Stage::Failed:
        if (tapped == 0) {
            updates.update(text::format(m_title, "VD_UPDATES_DOWNLOAD_TITLE", view(m_state.version)));
        } else {
            updates.later();
        }
        break;
    case Stage::Downloaded: updates.later(); break;
    default: break;
    }
    return true;
}

} // namespace vd

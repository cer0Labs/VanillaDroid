#include "app/ChatLinkInsert.h"

#include "ui/Context.h"
#include "ui/TextInput.h"

#include <array>
#include <cstring>

namespace vd {

namespace {
constexpr ui::WidgetId kChatField = ui::widgetId("chat.input"); // app/SocialScreens' edit box
}

bool ChatLinkInsert::active() const {
    return m_text != nullptr && m_text->active() && m_text->field() == kChatField;
}

bool ChatLinkInsert::insert(std::string_view link) {
    if (!active() || link.empty() || m_text->text().size() + link.size() > m_text->maxBytes()) {
        return false;
    }
    std::array<char, ui::TextInput::kCapacity> joined;
    const std::string_view current = m_text->text();
    std::memcpy(joined.data(), current.data(), current.size());
    std::memcpy(joined.data() + current.size(), link.data(), link.size());
    m_text->setText({joined.data(), current.size() + link.size()});
    return true;
}

} // namespace vd

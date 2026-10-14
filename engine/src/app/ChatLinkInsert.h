#pragma once

#include "core/Types.h"

#include <string_view>

namespace vd {

namespace ui {
class TextInput;
}

// Phase 13 (chat links): the desktop client's Shift + click on an item, a recipe or a quest while the chat's edit
// box is open (ChatFrameEditBox:Insert in ContainerFrame.lua, TradeSkillFrame.lua, QuestLogFrame.lua...). On a
// touch screen the open edit box is the modifier: while it is open, a window's tap on something linkable inserts
// its link instead of acting. Windows find it in their DialogContext (null when there is no chat); GameScreens
// binds the frame's text input before drawing them.
class ChatLinkInsert {
public:
    void bind(ui::TextInput* text) { m_text = text; }
    // The chat's edit box is open: a tap on something linkable should insert it instead of acting.
    bool active() const;
    // Appends `link` (a whole "|cff..|H...|h[...]|h|r" link, or plain text such as a spell's name) at the end of
    // the edit box's text. False when the box is closed or the link does not fit (255 bytes, as the client).
    bool insert(std::string_view link);

private:
    ui::TextInput* m_text = nullptr;
};

} // namespace vd

#pragma once

#include "app/Dialogs.h"
#include "core/Types.h"
#include "ui/Context.h"
#include "ui/DrawList.h"

#include <span>
#include <string_view>

namespace vd {

// The chat as the 1.12 interface lays it out (parity: FloatingChatFrame.xml / .lua, ChatFrame.xml):
// ChatFrame1 at the bottom left, the other chat frames docked to it as tabs over it (General, the Combat Log,
// ours), its lines fading two minutes after they came in, and the edit box under it. A pure view like
// app/Dialogs: lines and tabs in, what the player did out (app/SocialScreens fills it). Interface words by
// key (core/Text).
//
// Touch adaptations (no mouse-over, no Enter key, no wheel, no right click):
// - Mouse-over faded the background in; here a tap on the frame does, for a few seconds, and brings the
//   faded lines back. The lines take no finger: the movement stick and the camera work through them; only
//   their links do.
// - Phase 20: no chat-menu and scroll buttons down the frame's left side, and at
//   rest no tabs and only the last four lines. A double tap on the frame (on its links too) opens the chat
//   to read and type ("expanded": larger, scrolled with a finger, its tabs over it, the edit box under it)
//   and another one closes it again (so does Escape / Back). A tap on a link or a line waits out the double
//   tap's time before it acts (as Android's onSingleTapConfirmed). The edit box's header ("Say:") picks
//   where the text goes (ChatMenu's chat types); an emote button opens the emote menu (ChatMenu's EmoteMenu).
// - A long press on a tab is its right-click menu (FCFOptionsDropDown).

// The sizes of the chat's parts in logical pixels: the client's units, larger for a finger and a phone's
// distance.
struct ChatFrameMetrics {
    f32 unit = 1.0f;        // one unit of the client's 768-high interface
    f32 font = 14.0f;       // ChatFontNormal (Arial Narrow, 14 units)
    f32 lineHeight = 16.0f; // one line of it
    f32 tabHeight = 30.0f;  // ChatTabTemplate (64 x 32 units)
    f32 tabFont = 12.0f;    // its GameFontNormalSmall (10 units)
    f32 editHeight = 40.0f; // ChatFrameEditBox (32 units high)
};
// `fontScale`: the window's font size (FCF_SetChatWindowFontSize, ChatFrameView::fontScale).
ChatFrameMetrics chatFrameMetrics(const DialogContext& c, f32 fontScale = 1.0f);

// The lines the chat shows at rest (the frame's height).
constexpr u32 kChatCollapsedLines = 4;

// Where ChatFrame1's lines go (its tabs above and the edit box under it when expanded). Collapsed: the last
// kChatCollapsedLines lines (at `fontScale`), 430 units wide at the bottom left, raised so that the movement
// stick's resting place under it stays free (the stick floats wherever the left thumb lands); right of
// `avoid` (the party frames) when it would cover them. Expanded: larger, over the lower left, under the unit
// frames unless the soft keyboard is up (`keyboard`). Both stay left of `actionBar` (the HUD's action
// buttons; empty: none).
ui::Rect chatFrameRect(const DialogContext& c, bool expanded, const ui::Rect& actionBar,
                       const ui::Rect& avoid, bool keyboard, f32 fontScale = 1.0f);
// Everything the chat draws around `frame` (tabs, background, edit box): what other HUD text keeps clear of.
ui::Rect chatFrameBounds(const DialogContext& c, const ui::Rect& frame, bool expanded);

// A link of a line ("|H..|h[..]|h", game/ChatLinks): its bytes in the line's text.
struct ChatFrameLink {
    u16 begin = 0;
    u16 end = 0;
};
struct ChatFrameLine {
    std::string_view text;  // with the client's escapes (|c colours, |H links)
    u32 color = 0xFFFFFFFF; // its ChatTypeInfo colour; the alpha is how much of it is left (fading out)
    u32 key = 0;            // the same line across frames (its widgets)
    std::span<const ChatFrameLink> links;
};
struct ChatFrameTab {
    std::string_view name;
    f32 flash = 0.0f; // FCF_FlashTab's highlight now (ui::flashAlpha); 0: not flashing
};
struct ChatFrameView {
    std::span<const ChatFrameLine>
        lines; // the selected frame's, oldest first (collapsed: up to the one shown last)
    std::span<const ChatFrameTab> tabs; // the docked frames, in dock order
    u32 selected = 0;
    bool expanded = false;      // the tabs show only then
    f32 backgroundAlpha = 0.0f; // collapsed: the background's fade towards DEFAULT_CHATFRAME_ALPHA (touched)
    // Phase 18 (p3): the selected window's SIZE (CHAT_FONT_HEIGHTS / 14) and COLOR (its background's colour and alpha
    // at rest: the touch, the mouse-over, raises it to DEFAULT_CHATFRAME_ALPHA when lower).
    f32 fontScale = 1.0f;
    u32 backgroundColor = 0xFF000000;
    f32 restAlpha = 0.0f;
    // The edit box (expanded): its header ("Say: ", "Tell Aeryn: ", "[2. Trade]: ") in the chat type's
    // colour, the text being typed, what shows when it is empty.
    std::string_view header;
    u32 headerColor = 0xFFFFFFFF;
    std::string_view input;
    std::string_view hint;
    bool editing = false; // the keyboard is typing into it (caret)
};
struct ChatFrameResult {
    i32 tab = -1;        // a tab tapped
    i32 tabMenu = -1;    // a tab held (its menu)
    bool toggle = false; // a double tap on the frame: expand / collapse
    bool touched = false; // anything of the chat was touched this frame (the faded lines come back)
    // Taps on a line or a link, reported once the double tap's time is over without a second one.
    i32 line = -1;     // expanded: a line tapped outside its links
    i32 linkLine = -1; // a link tapped: its line, where its "|H" is in the line's text and where it was drawn
    u32 linkBegin = 0;
    ui::Rect linkRect;
    // Phase 13 (social): a link held (the right click: a player's menu), the same way.
    i32 linkHeldLine = -1;
    u32 linkHeldBegin = 0;
    ui::Rect linkHeldRect;
    ui::Rect lineRect; // expanded: where the line tapped (`line`) is
    bool tappedInput = false; // the edit box's text
    bool pickChannel = false; // its header
    bool emotes = false;
    bool send = false;
};
// A tap waiting for the double tap's time to act (ChatFrameState).
enum class ChatPendingTap : u8 { None, Line, Link };

// What the view keeps between frames for its caller.
struct ChatFrameState {
    f32 viewport = 0.0f; // open: the lines' height last frame...
    bool atEnd = true;   // ...and whether the newest line was in sight
    // The last tap on the frame (the first of a double tap?): when it lifted (0: none) and where it went
    // down.
    u64 tapMs = 0;
    f32 tapX = 0.0f, tapY = 0.0f;
    // A tap on a line or a link that acts once no second one came: its line's key, the link's start in it
    // and which of its drawn pieces (a link may wrap), when.
    ChatPendingTap pending = ChatPendingTap::None;
    u32 pendingKey = 0;
    u16 pendingBegin = 0;
    u32 pendingSpan = 0;
    u64 pendingMs = 0;
};
ChatFrameResult chatFrame(DialogContext& c, const ui::Rect& frame, const ChatFrameView& v,
                          ChatFrameState& state);

} // namespace vd

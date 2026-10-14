#pragma once

#include "core/Types.h"
#include "game/AccountData.h"

#include <array>
#include <filesystem>
#include <string>
#include <string_view>

namespace vd {

class GameSession;
class MacroScreens;
class SocialScreens;
class TouchBarScreens; // phase 20 (edit)

// Phase 18 (p3): this client's files behind the account data the server keeps (game::AccountData), where 1.12 keeps
// them (under the app's folder, as the macros: WTF\Account\<ACCOUNT>\ and ...\<realm>\<character>\):
//   type 4 GLOBAL_MACROS        WTF\Account\<ACCOUNT>\macros-cache.txt           (app/MacroScreens: General Macros)
//   type 5 PER_CHARACTER_MACROS ...\<realm>\<character>\macros-cache.txt         (its Specific Macros)
//   type 7 PER_CHARACTER_CHAT   ...\<realm>\<character>\chat-cache.txt           (game::ChatWindows)
//   type 6 PER_CHARACTER_LAYOUT ...\<realm>\<character>\touch-layout.txt         (phase 20: the touch cluster's
//          templates, app/TouchBarScreens; 1.12's own layout-cache.txt shares the type: a text not starting with
//          "VDTOUCH" never replaces the local file, which goes up again)
//   cache.md5                   WTF\Account\<ACCOUNT>\cache.md5 (8 x 16 bytes: the digests of the last sync)
// The others (config-cache.wtf, the key bindings, layout-cache.txt) this client does not keep: never asked for nor
// sent (1.12.1 itself never writes config-cache.wtf). A text the server sends replaces the file and what was read
// from it; one that comes before the character's first frame in the world waits here and replaces what is read then.
// Saves: the macros on every change (MacroScreens), the chat windows on every change of their menus; each starts the
// 30 s upload timer; leaving the world uploads what waits. GameScreens owns it. Main thread: the files are read at
// login (small, during the loading screen, as the macros); writes go to the background writer.
class AccountDataFiles {
public:
    void init(MacroScreens* macros, SocialScreens* social);
    // Phase 20 (edit, docs/MOBILE_CONTROLS.md §5.5): the touch layout kept in step as type 6.
    void setTouch(TouchBarScreens* touch);
    // Every frame (also before the world: the session's game::AccountData gets this store; leaving the world is seen
    // here).
    void update(GameSession& session, u32 nowMs);
    // A character's first frame in the world (after MacroScreens::enterWorld): its chat-cache.txt, and the texts the
    // server sent before.
    void enterWorld(GameSession& session);
    // Leaving the world (the character list, a disconnection): the chat windows saved, every waiting upload sent.
    void leaveWorld(GameSession& session);

private:
    static bool read(void* context, u32 type, std::string& out);
    static void write(void* context, u32 type, std::string_view text);
    static void begin(void* context);
    std::filesystem::path file(u32 type) const;
    void saveChat(u32 nowMs);

    MacroScreens* m_macros = nullptr;
    SocialScreens* m_social = nullptr;
    GameSession* m_session = nullptr;
    bool m_attached = false;
    u64 m_loadedFor = 0; // the character whose files are read (0: none yet)
    u32 m_syncedVersion = 0;
    u32 m_renameVersion = 0; // phase 18 (extras): the session's renames seen
    std::array<std::string, net::kAccountDataTypes> m_pending{}; // downloads before the first frame in the world
    std::array<bool, net::kAccountDataTypes> m_hasPending{};
    // Phase 20 (edit): the touch layout; the server had 1.12's layout-cache.txt in type 6: ours goes up again.
    TouchBarScreens* m_touch = nullptr;
    bool m_touchReupload = false;
};

} // namespace vd

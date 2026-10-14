#include "app/AccountDataFiles.h"

#include "app/GameSession.h"
#include "app/MacroScreens.h"
#include "app/SocialScreens.h"
#include "app/TouchBarScreens.h" // phase 20 (edit)
#include "app/WtfFolders.h" // phase 18 (extras)
#include "core/Log.h"
#include "core/Profile.h"
#include "game/TouchLayoutEdit.h" // phase 20 (edit)

#include <cstring>
#include <fstream>
#include <iterator>

namespace vd {
namespace {

using net::AccountDataType;

constexpr u32 kGlobalMacros = static_cast<u32>(AccountDataType::GlobalMacros);
constexpr u32 kCharacterMacros = static_cast<u32>(AccountDataType::CharacterMacros);
constexpr u32 kCharacterChat = static_cast<u32>(AccountDataType::CharacterChat);
constexpr u32 kCharacterLayout = static_cast<u32>(AccountDataType::CharacterLayout); // phase 20 (edit)

std::string readText(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

game::MacroTab macroTab(u32 type) {
    return type == kGlobalMacros ? game::MacroTab::Account : game::MacroTab::Character;
}

} // namespace

void AccountDataFiles::init(MacroScreens* macros, SocialScreens* social) {
    m_macros = macros;
    m_social = social;
    if (m_macros) {
        m_macros->setSaveHook(
            [](void* self, game::MacroTab tab) {
                AccountDataFiles& files = *static_cast<AccountDataFiles*>(self);
                if (files.m_session && files.m_session->inWorld()) {
                    files.m_session->play().accountData().changed(
                        tab == game::MacroTab::Account ? kGlobalMacros : kCharacterMacros,
                        files.m_session->lastUpdateMs());
                }
            },
            this);
    }
}

void AccountDataFiles::setTouch(TouchBarScreens* touch) {
    // Phase 20 (edit): each save of touch-layout.txt starts type 6's 30 s timer, as the macros' saves do theirs.
    m_touch = touch;
    if (m_touch) {
        m_touch->setSaveHook(
            [](void* self) {
                AccountDataFiles& files = *static_cast<AccountDataFiles*>(self);
                if (files.m_session && files.m_loadedFor != 0) {
                    files.m_session->play().accountData().changed(kCharacterLayout, files.m_session->lastUpdateMs());
                }
            },
            this);
    }
}

std::filesystem::path AccountDataFiles::file(u32 type) const {
    if (!m_session || !m_macros || m_macros->storageFolder().empty()) {
        return {};
    }
    const std::filesystem::path& folder = m_macros->storageFolder();
    switch (type) {
    case kGlobalMacros: return MacroScreens::wtfFolder(folder, *m_session, false) / "macros-cache.txt";
    case kCharacterMacros: return MacroScreens::wtfFolder(folder, *m_session, true) / "macros-cache.txt";
    case kCharacterChat: return MacroScreens::wtfFolder(folder, *m_session, true) / "chat-cache.txt";
    case kCharacterLayout: return MacroScreens::wtfFolder(folder, *m_session, true) / "touch-layout.txt"; // phase 20
    default: return {};
    }
}

void AccountDataFiles::begin(void* context) {
    // A login: cache.md5 of the account (WoW.exe reads it at start, 0x5AF880), nothing read of the character yet.
    AccountDataFiles& self = *static_cast<AccountDataFiles*>(context);
    self.m_loadedFor = 0;
    self.m_hasPending.fill(false);
    game::AccountData::Digests synced{};
    if (self.m_session && self.m_macros && !self.m_macros->storageFolder().empty()) {
        const std::string bytes =
            readText(MacroScreens::wtfFolder(self.m_macros->storageFolder(), *self.m_session, false) / "cache.md5");
        for (u32 i = 0; i < synced.size() && (i + 1) * 16 <= bytes.size(); ++i) {
            std::memcpy(synced[i].data(), bytes.data() + i * 16, 16);
        }
    }
    if (self.m_session) {
        self.m_session->play().accountData().setSynced(synced);
    }
}

bool AccountDataFiles::read(void* context, u32 type, std::string& out) {
    AccountDataFiles& self = *static_cast<AccountDataFiles*>(context);
    if (type != kGlobalMacros && type != kCharacterMacros && type != kCharacterChat &&
        !(type == kCharacterLayout && self.m_touch)) {
        return false; // not kept by this client
    }
    const std::filesystem::path path = self.file(type);
    if (path.empty()) {
        return false; // no folder to keep files in (tests, a platform without one)
    }
    const bool loaded = self.m_session && self.m_loadedFor != 0 && self.m_loadedFor == self.m_session->playerGuid();
    if (type == kCharacterChat) {
        out = loaded && self.m_social ? self.m_social->chatWindows().format() : readText(path);
        return true;
    }
    if (type == kCharacterLayout) { // phase 20 (edit): the layout in use once read, else its file
        out = loaded && self.m_touch->haveLayout() ? game::formatTouchLayout(self.m_touch->layout()) : readText(path);
        return true;
    }
    if (loaded && self.m_macros && self.m_macros->loaded()) {
        out = self.m_macros->store().format(macroTab(type));
        return true;
    }
    out = readText(path);
    if (out.empty() && type == kCharacterMacros) {
        out = readText(path.parent_path() / "macros-local.txt"); // what phase 15's client wrote
    }
    return true;
}

void AccountDataFiles::write(void* context, u32 type, std::string_view text) {
    // 0x5AFC60: the server's text over the file; what was read from it follows.
    AccountDataFiles& self = *static_cast<AccountDataFiles*>(context);
    const std::filesystem::path path = self.file(type);
    if (path.empty() || !self.m_macros) {
        return;
    }
    const bool loaded = self.m_session && self.m_loadedFor != 0 && self.m_loadedFor == self.m_session->playerGuid();
    if (type == kCharacterLayout && !game::isTouchLayoutText(text)) {
        // Phase 20 (edit, §5.5): 1.12's layout-cache.txt (the character was played on the PC): the touch layout stays
        // and goes up again (update()), when there is one.
        self.m_touchReupload = (loaded && self.m_touch && self.m_touch->haveLayout()) || !readText(path).empty();
        VD_LOG_INFO("Account data: type %u from the server is not a touch layout (%zu bytes): ours kept", type,
                    text.size());
        return;
    }
    self.m_macros->writeFile(path, std::string(text));
    if (!loaded) {
        self.m_pending[type].assign(text); // the character's first frame in the world reads it
        self.m_hasPending[type] = true;
        return;
    }
    if (type == kCharacterChat && self.m_social) {
        self.m_social->chatWindows().parse(text);
    } else if (type == kCharacterLayout && self.m_touch) {
        self.m_touch->adoptLayout(text); // phase 20 (edit)
    } else if (type == kGlobalMacros || type == kCharacterMacros) {
        self.m_macros->store().parse(macroTab(type), text);
    }
    VD_LOG_INFO("Account data: type %u from the server (%zu bytes)", type, text.size());
}

void AccountDataFiles::update(GameSession& session, u32 nowMs) {
    VD_PROFILE_ZONE("AccountDataFiles::update");
    m_session = &session;
    game::AccountData& data = session.play().accountData();
    if (!m_attached) {
        game::AccountDataStore store;
        store.read = &AccountDataFiles::read;
        store.write = &AccountDataFiles::write;
        store.begin = &AccountDataFiles::begin;
        store.context = this;
        data.setStore(store);
        m_attached = true;
    }
    if (!session.inWorld() && m_loadedFor != 0) {
        leaveWorld(session); // logged out, disconnected
    }
    if (session.renameVersion() != m_renameVersion) {
        // Phase 18 (extras): a character renamed at the character list takes its WTF folders along (0x4725F0); a
        // rename is rare and a folder's rename is one metadata operation.
        m_renameVersion = session.renameVersion();
        if (session.renameResult() == 0 && m_macros &&
            renameCharacterFolders(m_macros->storageFolder(), session.loginParams().account, session.realmName(),
                                   session.renamedFrom(), session.renamedTo())) {
            VD_LOG_INFO("WTF: %.*s's folders renamed to %.*s", static_cast<int>(session.renamedFrom().size()),
                        session.renamedFrom().data(), static_cast<int>(session.renamedTo().size()),
                        session.renamedTo().data());
        }
    }
    if (session.inWorld() && m_loadedFor == session.playerGuid() && m_social &&
        m_social->chatWindows().changed()) {
        saveChat(nowMs);
    }
    if (m_touchReupload && session.inWorld() && !data.waiting(kCharacterLayout)) {
        m_touchReupload = false; // phase 20 (edit): ours over 1.12's layout-cache.txt, after the 30 s
        data.changed(kCharacterLayout, nowMs);
    }
    if (data.syncedVersion() != m_syncedVersion && m_macros && !m_macros->storageFolder().empty()) {
        // cache.md5 (0x5AF9C0): the eight digests.
        m_syncedVersion = data.syncedVersion();
        std::string bytes(16 * net::kAccountDataTypes, '\0');
        for (u32 i = 0; i < net::kAccountDataTypes; ++i) {
            std::memcpy(bytes.data() + i * 16, data.synced()[i].data(), 16);
        }
        m_macros->writeFile(MacroScreens::wtfFolder(m_macros->storageFolder(), session, false) / "cache.md5",
                            std::move(bytes));
    }
}

void AccountDataFiles::enterWorld(GameSession& session) {
    m_session = &session;
    m_loadedFor = session.playerGuid();
    if (m_social) {
        // The chat windows of the character (WoW.exe 0x498A20: its reader, deferred while the server's text comes).
        game::ChatWindows& windows = m_social->chatWindows();
        const std::filesystem::path path = file(kCharacterChat);
        const std::string text = m_hasPending[kCharacterChat] ? m_pending[kCharacterChat]
                                 : path.empty()                ? std::string{}
                                                               : readText(path);
        if (!windows.parse(text)) {
            windows.setDefaults();
        }
        windows.clearChanged();
    }
    for (const u32 type : {kGlobalMacros, kCharacterMacros}) {
        if (m_hasPending[type] && m_macros) {
            m_macros->store().parse(macroTab(type), m_pending[type]);
        }
    }
    if (m_hasPending[kCharacterLayout] && m_touch) { // phase 20 (edit): the server's newer touch layout
        m_touch->adoptLayout(m_pending[kCharacterLayout]);
    }
    m_hasPending.fill(false);
}

void AccountDataFiles::saveChat(u32 nowMs) {
    if (!m_social || !m_session) {
        return;
    }
    game::ChatWindows& windows = m_social->chatWindows();
    windows.clearChanged();
    const std::filesystem::path path = file(kCharacterChat);
    if (path.empty() || !m_macros) {
        return;
    }
    m_macros->writeFile(path, windows.format());
    m_session->play().accountData().changed(kCharacterChat, nowMs);
}

void AccountDataFiles::leaveWorld(GameSession& session) {
    // 0x490C50 (the chat windows saved when the character leaves the world), then 0x5AFD30 (what waits goes up).
    m_session = &session;
    if (m_loadedFor != 0 && m_social && m_social->chatWindows().changed()) {
        saveChat(session.lastUpdateMs());
    }
    session.play().accountData().flush();
    m_loadedFor = 0;
}

} // namespace vd

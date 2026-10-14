#pragma once

#include "core/Types.h"
#include "game/Entities.h"
#include "game/GameEvents.h"
#include "game/GuildNotice.h"
#include "net/GuildMessages.h"
#include "net/Opcodes.gen.h"

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vd::game {

class Inventory;
class Social;

// What the guild registrar offers (SMSG_PETITION_SHOWLIST: GuildRegistrarFrame, the GUILD_REGISTRAR_SHOW
// event).
struct GuildRegistrar {
    bool open = false;
    u64 npc = 0;
    u32 cost = 0;  // GetGuildCharterCost (copper)
    u32 index = 0; // the list entry CMSG_PETITION_BUY names
};

// The charter shown (SMSG_PETITION_SHOW_SIGNATURES + SMSG_PETITION_QUERY_RESPONSE: PetitionFrame,
// PETITION_SHOW).
struct GuildPetition {
    bool open = false;
    u64 item = 0;  // the charter
    u64 owner = 0; // its guild master
    u32 petition = 0;
    std::string name;    // GetPetitionInfo's title: the guild's name (empty until the query answer)
    bool charter = true; // GetPetitionInfo's type: "charter" (flags & 1), else "Petition"
    u32 maxSignatures = net::kPetitionSignaturesMax;
    std::array<u64, net::kPetitionSignaturesMax> signers{};
    u32 signatures = 0;
    bool signedIt = false; // the player signed it (ClosePetition sends no decline then)
};

// Phase 15 (guild): creating a guild as 1.12 does it (GuildRegistrarFrame.lua, PetitionFrame.lua and the C
// functions behind them, WoW.exe 0x4F3F60..0x4F5300 and the packet handlers 0x5EEB80..0x5EF1D0): the
// registrar's list (BuyGuildCharter with the client's own name checks, TurnInGuildCharter), the charter
// window (its signatures, the guild's name from CMSG_PETITION_QUERY, CanSignPetition, SignPetition,
// OfferPetition to the target with the client's checks, RenamePetition, ClosePetition's decline of an
// unsigned charter someone offered) and every answer as the UIErrorsFrame line WoW.exe shows (notices()).
// Packets as vmangos-core 4641790 handles them (net/GuildMessages). Main thread; fixed storage.
class Petitions {
public:
    Petitions(const EntityStore& entities, const Social& social, const Inventory& inventory);

    void setSink(PacketSink sink) { m_sink = sink; }
    void enterWorld(u64 player, u32 nowMs);
    void leaveWorld();
    static bool handles(net::Opcode opcode);
    // False when malformed.
    bool handle(net::Opcode opcode, std::span<const u8> payload, u32 nowMs);
    const GuildNotices& notices() const { return m_notices; }
    // Bumps with every change of the registrar or the charter (the windows rebuild their views).
    u32 version() const { return m_version; }

    // --- The registrar ---
    const GuildRegistrar& registrar() const { return m_registrar; }
    // BuyGuildCharter(name): the client's checks (a guild name it accepts, not in a guild, the money), then
    // CMSG_PETITION_BUY; a refusal is a notice. True when sent.
    bool buyCharter(std::string_view name, u32 nowMs);
    // TurnInGuildCharter: the charter the player carries, CMSG_TURN_IN_PETITION. False when it carries none.
    bool turnInCharter();
    void closeRegistrar(); // CloseGuildRegistrar (GUILD_REGISTRAR_CLOSED)

    // --- The charter ---
    const GuildPetition& petition() const { return m_petition; }
    bool isOriginator() const { return m_petition.open && m_petition.owner == m_player; }
    // Using the charter item (its on-use: CMSG_PETITION_SHOW_SIGNATURES): the window opens on the answer.
    bool showCharter(u64 item);
    // CanSignPetition (WoW.exe 0x4F45E0).
    bool canSign() const;
    bool sign(); // SignPetition: CMSG_PETITION_SIGN
    // OfferPetition: to the player's target (WoW.exe 0x4F4790's checks: a player in sight, not the player, of
    // its faction, not in a guild), CMSG_OFFER_PETITION and ERR_PETITION_OFFERED_S. `friendly`: UnitIsFriend
    // (the caller's reaction rules).
    bool offer(u64 target, bool friendly, u32 nowMs);
    // RenamePetition(name): the client's name checks, then MSG_PETITION_RENAME.
    bool rename(std::string_view name, u32 nowMs);
    // ClosePetition: an unsigned charter someone else offered is declined (MSG_PETITION_DECLINE);
    // PETITION_CLOSED.
    void closePetition();

    // The client's checks of a guild name (WoW.exe 0x6C9B70 through 0x4F5160): the ERR_GUILD_NAME_* /
    // ERR_GUILD_ENTER_NAME key that refuses it, nullptr when accepted. Spaces inside, letters (any UTF-8
    // letter), 2..24 letters; the profanity, reserved and mixed language tables of WoW.exe are the server's
    // to check (ERR_GUILD_NAME_INVALID).
    static const char* guildNameError(std::string_view name);

private:
    bool sendBody(net::Opcode opcode);
    GuildNotice& notice(const char* key, u64 guid = 0);
    void onShowSignatures(const net::PetitionSignatures& m);
    void addSigner(u64 guid);
    u64 carriedCharter() const;

    const EntityStore& m_entities;
    const Social& m_social;
    const Inventory& m_inventory;
    PacketSink m_sink;
    std::vector<u8> m_body;
    bool m_inWorld = false;
    u64 m_player = 0;
    u32 m_nowMs = 0;
    u32 m_version = 0;
    GuildRegistrar m_registrar;
    GuildPetition m_petition;
    GuildNotices m_notices;
    net::PetitionSignatures m_signatures; // parsed, reused
};

} // namespace vd::game

#include "audio/ModelSoundRules.h"

#include "audio/SoundData.h"

namespace vd::audio {

ModelEvent classifyModelEventInfo(u32 id) {
    switch (id) {
    case modelEventId("$FSD"): return {ModelEventKind::Footstep, 0};
    case modelEventId("$DSL"): return {ModelEventKind::DoodadLoop, 0};
    case modelEventId("$DSO"): return {ModelEventKind::DoodadOneShot, 0};
    case modelEventId("$DSE"): return {ModelEventKind::DoodadStop, 0};
    case modelEventId("$SND"): return {ModelEventKind::Sound, 0};
    case modelEventId("$CSD"): return {ModelEventKind::EmoteSound, 0};
    case modelEventId("$ESD"): return {ModelEventKind::EmoteState, 0};
    case modelEventId("$FDX"): return {ModelEventKind::Stand, 0};
    case modelEventId("$WNG"): return {ModelEventKind::WingFlap, 0};
    case modelEventId("$WGG"): return {ModelEventKind::WingGlide, 0};
    default: break;
    }
    // "$FD1".."$FD4": fidget 0..3 (WoW.exe 0x6232C0 passes 0..8 for $FD1..$FD9 and 0x6230A0 answers only below 4).
    if ((id & 0x00FFFFFFu) == (modelEventId("$FD0") & 0x00FFFFFFu)) {
        const u32 digit = (id >> 24) - '0';
        return digit >= 1 && digit <= 4 ? ModelEvent{ModelEventKind::Fidget, static_cast<u8>(digit - 1)} : ModelEvent{};
    }
    // "$AH0".."$AH3": the custom attacks (0x6247D0).
    if ((id & 0x00FFFFFFu) == (modelEventId("$AH0") & 0x00FFFFFFu)) {
        const u32 digit = (id >> 24) - '0';
        return digit <= 3 ? ModelEvent{ModelEventKind::CustomAttack, static_cast<u8>(digit)} : ModelEvent{};
    }
    return {};
}

u32 creatureEventSound(const CreatureSounds& sounds, const ModelEvent& event) {
    switch (event.kind) {
    case ModelEventKind::Fidget: return event.index < sounds.fidget.size() ? sounds.fidget[event.index] : 0;
    case ModelEventKind::Stand: return sounds.stand;
    case ModelEventKind::CustomAttack:
        return event.index < sounds.customAttack.size() ? sounds.customAttack[event.index] : 0;
    case ModelEventKind::WingFlap: return sounds.wingFlap;
    case ModelEventKind::WingGlide: return sounds.wingGlide;
    default: return 0;
    }
}

bool crossed(u32 fromMs, u32 toMs, u32 eventMs, u32 cycleMs) {
    if (cycleMs == 0 || fromMs == toMs) {
        return false;
    }
    if (fromMs < toMs) {
        return eventMs > fromMs && eventMs <= toMs;
    }
    return eventMs > fromMs || eventMs <= toMs; // wrapped
}

} // namespace vd::audio

#include "game/Chat.h"

#include "game/CharacterNames.h"

#include <cctype>

namespace vd::game {
namespace {

void appendCased(std::string& out, std::string_view word, bool lower) {
    for (const char c : word) {
        out.push_back(lower ? static_cast<char>(std::tolower(static_cast<unsigned char>(c))) : c);
    }
}

} // namespace

ChatLine& ChatLog::push(ChatSource source, u32 nowMs) {
    ChatLine& line = m_lines.push();
    line.timeMs = nowMs;
    line.source = source;
    line.type = net::ChatType::System;
    line.language = 0;
    line.sender = 0;
    line.target = 0;
    line.tag = 0;
    line.playerRank = 0;
    line.textEmote = 0;
    line.emoteNum = 0;
    line.senderName.clear(); // keeps the capacity
    line.channel.clear();
    line.text.clear();
    line.languageName = {}; // phase 18 (extras)
    return line;
}

ChatLine& ChatLog::add(const net::ChatMessage& m, u32 nowMs) {
    ChatLine& line = push(ChatSource::Message, nowMs);
    line.type = m.type;
    line.language = m.language;
    line.sender = m.sender;
    line.target = m.target;
    line.tag = m.tag;
    line.playerRank = m.playerRank;
    line.senderName.assign(m.senderName);
    line.channel.assign(m.channel);
    line.text.assign(m.text);
    return line;
}

void ChatLog::addNotice(ChatSource source, std::string_view text, u32 nowMs) {
    push(source, nowMs).text.assign(text);
}

void ChatLog::addTextEmote(const net::TextEmote& m, u32 nowMs) {
    ChatLine& line = push(ChatSource::TextEmote, nowMs);
    line.type = net::ChatType::TextEmote;
    line.sender = m.guid;
    line.textEmote = m.textEmote;
    line.emoteNum = m.emoteNum;
    line.text.assign(m.targetName);
}

void expandText(std::string_view text, const TextSubject& subject, std::string& out) {
    out.clear();
    for (usize i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c != '$' || i + 1 >= text.size()) {
            out.push_back(c);
            continue;
        }
        const char code = text[i + 1];
        const bool lower = code >= 'a' && code <= 'z';
        switch (code) {
        case 'N':
        case 'n':
            out.append(subject.name);
            ++i;
            break;
        case 'R':
        case 'r':
            appendCased(out, raceName(subject.race), lower);
            ++i;
            break;
        case 'C':
        case 'c':
            appendCased(out, className(subject.cls), lower);
            ++i;
            break;
        case 'B':
        case 'b':
            out.push_back('\n');
            ++i;
            break;
        case 'G':
        case 'g': {
            // $G<male>:<female>; (a space may follow the G)
            const usize colon = text.find(':', i + 2);
            const usize end = colon == std::string_view::npos ? colon : text.find(';', colon + 1);
            if (end == std::string_view::npos) {
                out.push_back(c);
                break;
            }
            std::string_view male = text.substr(i + 2, colon - (i + 2));
            while (!male.empty() && male.front() == ' ') {
                male.remove_prefix(1);
            }
            const std::string_view female = text.substr(colon + 1, end - (colon + 1));
            out.append(subject.gender == 1 ? female : male);
            i = end;
            break;
        }
        default: out.push_back(c); break;
        }
    }
}

} // namespace vd::game

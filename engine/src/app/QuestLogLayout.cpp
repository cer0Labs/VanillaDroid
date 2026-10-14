#include "app/QuestLogLayout.h"

#include <algorithm>
#include <cmath>
#include <span>

namespace vd {
namespace {

using ui::Rect;
using ui::TexCoords;

// A picture made of four textures side by side (QuestLogFrame.xml: TopLeft at its TOPLEFT, TopRight at its TOPRIGHT,
// BotLeft and BotRight under them), the first of them and the others in QuestLogTexture's order.
struct Canvas {
    QuestLogTexture first;
    f32 splitX, splitY; // where the textures meet
    f32 width, height;
};
constexpr Canvas kFrameArt{QuestLogTexture::TopLeft, 256.0f, 256.0f, 384.0f, 512.0f};
// EmptyQuestLogFrame's: 256 + 64 wide, 256 + 128 high, at (19, -73) of the frame.
constexpr Canvas kEmptyArt{QuestLogTexture::EmptyTopLeft, 256.0f, 256.0f, 320.0f, 384.0f};

// The art's coordinates (its units from its top left), as measured on the textures. What is not transparent: the
// portrait's ring reaches 3 from the left and 2 from the top, the frame ends at 349 and 464.
constexpr f32 kArtLeft = 3.0f, kArtTop = 2.0f, kArtRight = 349.0f, kArtBottom = 464.0f;
constexpr f32 kHeaderEnd = 72.0f; // the title bar and the header end at the list box's top
constexpr f32 kBarStart = 436.0f; // the buttons' bar starts at the boxes' bottom line (the parchment's edge over it)
constexpr f32 kParchmentTop = 172.0f; // wide: the parchment box from its top line, level with the list box's (73)
// In the frame's units (from what is drawn of it): the header's height, the buttons' bar's.
constexpr f32 kHeader = kHeaderEnd - kArtTop;  // 70
constexpr f32 kBar = kArtBottom - kBarStart;   // 28
constexpr f32 kSeparator = 10.0f;              // wide: the list box's right bar between the list and the parchment
constexpr f32 kColumn = kArtRight - 318.0f;    // the parchment's column (its scroll bar) and the frame's right edge

// One cut of the art along an axis: [from, to) of it keeps its size or shares what is left with the other stretched
// cuts (in proportion to theirs).
struct Cut {
    f32 from, to;
    bool stretch;
};
constexpr u32 kMaxCuts = 4;

// Where each cut goes between a and b: out[i]..out[i + 1]. When the room is shorter than the fixed cuts, they all
// shrink alike.
void positions(std::span<const Cut> cuts, f32 a, f32 b, f32* out) {
    f32 fixed = 0.0f, stretched = 0.0f;
    for (const Cut& c : cuts) {
        (c.stretch ? stretched : fixed) += c.to - c.from;
    }
    const f32 length = std::max(0.0f, b - a);
    const f32 shrink = fixed > length ? length / fixed : 1.0f;
    const f32 room = std::max(0.0f, length - fixed);
    out[0] = a;
    for (usize i = 0; i < cuts.size(); ++i) {
        const f32 size = cuts[i].to - cuts[i].from;
        out[i + 1] = out[i] + (cuts[i].stretch ? (stretched > 0.0f ? room * size / stretched : 0.0f) : size * shrink);
    }
}

class Builder {
public:
    explicit Builder(QuestLogLayout& l) : m_l(l) {}

    // The frame's units (from its top left) in logical pixels.
    Rect rect(f32 x, f32 y, f32 w, f32 h) const {
        return {m_l.frame.x + x * m_l.k, m_l.frame.y + y * m_l.k, w * m_l.k, h * m_l.k};
    }

    // The art's rectangle [x0, x1) x [y0, y1) cut along both axes, over the frame's units [dx0, dx1) x [dy0, dy1).
    void slices(std::span<const Cut> xs, std::span<const Cut> ys, f32 dx0, f32 dy0, f32 dx1, f32 dy1) {
        f32 px[kMaxCuts + 1], py[kMaxCuts + 1];
        positions(xs, dx0, dx1, px);
        positions(ys, dy0, dy1, py);
        for (usize j = 0; j < ys.size(); ++j) {
            for (usize i = 0; i < xs.size(); ++i) {
                add(m_l.pieces.data(), m_l.pieces.size(), m_l.pieceCount, kFrameArt,
                    {xs[i].from, ys[j].from, xs[i].to, ys[j].to},
                    rect(px[i], py[j], px[i + 1] - px[i], py[j + 1] - py[j]));
            }
        }
    }
    // EmptyQuestLogFrame's picture [x0, x1) x [y0, y1) over the frame's units.
    void empty(f32 x0, f32 y0, f32 x1, f32 y1, f32 dx0, f32 dy0, f32 dx1, f32 dy1) {
        add(m_l.emptyPieces.data(), m_l.emptyPieces.size(), m_l.emptyPieceCount, kEmptyArt, {x0, y0, x1, y1},
            rect(dx0, dy0, dx1 - dx0, dy1 - dy0));
    }

private:
    struct Source {
        f32 x0, y0, x1, y1;
    };
    // A rectangle of a canvas over `dst`, one piece per texture it crosses.
    static void add(QuestLogPiece* pieces, usize capacity, u32& count, const Canvas& c, const Source& s,
                    const Rect& dst) {
        if (dst.w <= 0.0f || dst.h <= 0.0f || s.x1 <= s.x0 || s.y1 <= s.y0) {
            return;
        }
        const f32 xs[3] = {0.0f, c.splitX, c.width};
        const f32 ys[3] = {0.0f, c.splitY, c.height};
        const f32 sx = dst.w / (s.x1 - s.x0), sy = dst.h / (s.y1 - s.y0);
        for (u32 row = 0; row < 2; ++row) {
            for (u32 col = 0; col < 2; ++col) {
                const f32 x0 = std::max(s.x0, xs[col]), x1 = std::min(s.x1, xs[col + 1]);
                const f32 y0 = std::max(s.y0, ys[row]), y1 = std::min(s.y1, ys[row + 1]);
                if (x1 <= x0 || y1 <= y0 || count >= capacity) {
                    continue;
                }
                const f32 tw = xs[col + 1] - xs[col], th = ys[row + 1] - ys[row];
                QuestLogPiece& p = pieces[count++];
                p.texture = static_cast<QuestLogTexture>(static_cast<u32>(c.first) + row * 2 + col);
                p.dst = {dst.x + (x0 - s.x0) * sx, dst.y + (y0 - s.y0) * sy, (x1 - x0) * sx, (y1 - y0) * sy};
                p.uv = {(x0 - xs[col]) / tw, (y0 - ys[row]) / th, (x1 - xs[col]) / tw, (y1 - ys[row]) / th};
            }
        }
    }

    QuestLogLayout& m_l;
};

} // namespace

bool questLogWide(const Rect& place, f32 k) {
    // Side by side the list and the parchment want most of the original's width each (a 20:9 phone's place over a
    // stance bar has about 520 units).
    return k > 0.0f && place.w >= place.h * 1.15f && place.w / k >= 480.0f;
}

QuestLogLayout questLogLayout(const Rect& place, f32 k, bool wide, f32 listShare) {
    QuestLogLayout l;
    l.wide = wide;
    if (place.w <= 0.0f || place.h <= 0.0f || k <= 0.0f) {
        return l;
    }
    k = std::min({k, place.w / kQuestLogMinWidth, place.h / kQuestLogMinHeight});
    l.k = k;
    const f32 W = std::min(place.w / k, wide ? kQuestLogMaxWideWidth : kQuestLogMaxTallWidth);
    const f32 H = std::min(place.h / k, wide ? kQuestLogMaxWideHeight : kQuestLogMaxTallHeight);
    l.frame = {place.x, place.y, W * k, H * k};
    Builder b(l);

    // The title bar and the header across the frame: the portrait's end and the close button's notch keep their
    // size, the middle stretches.
    {
        constexpr Cut xs[] = {{kArtLeft, 110.0f, false}, {110.0f, 290.0f, true}, {290.0f, kArtRight, false}};
        constexpr Cut ys[] = {{kArtTop, kHeaderEnd, false}};
        b.slices(xs, ys, 0.0f, 0.0f, W, kHeader);
    }
    // The buttons' bar: Abandon Quest's slot at the left; the middle slot (Share Quest's) and Exit's at the right; in
    // between, wider than the original, the middle slot's inside stretched into a slot of its own (the divider before
    // Share Quest's drawn again).
    {
        constexpr Cut ys[] = {{kBarStart, kArtBottom, false}};
        if (W > kArtRight - kArtLeft + 3.0f) {
            constexpr Cut xs[] = {{kArtLeft, 141.0f, false}, {150.0f, 250.0f, true}, {138.0f, kArtRight, false}};
            b.slices(xs, ys, 0.0f, H - kBar, W, H);
        } else {
            constexpr Cut xs[] = {{kArtLeft, 141.0f, false}, {141.0f, kArtRight, true}};
            b.slices(xs, ys, 0.0f, H - kBar, W, H);
        }
    }
    const f32 c0 = kHeader, c1 = H - kBar; // the content between them
    const f32 column = W - kColumn;          // the parchment's column (the original's two columns, tall)

    if (wide) {
        // Side by side: the list box (its left edge kept, the dark inside stretched), its right bar, then the parchment
        // box and its column. The parchment keeps the original's 300 units when it can (its rewards two to a row), the
        // list at least 200 and at most `listShare` of the room.
        const f32 room = W - kSeparator;
        const f32 most = std::max(160.0f, room * listShare);
        const f32 listW = std::round(std::clamp(W - kSeparator - kColumn - 300.0f, std::min(200.0f, most), most));
        const f32 p0 = listW + kSeparator;
        {
            constexpr Cut xs[] = {{kArtLeft, 22.0f, false}, {22.0f, 316.0f, true}};
            constexpr Cut ys[] = {{kHeaderEnd, 80.0f, false}, {80.0f, 158.0f, true}, {158.0f, 166.0f, false}};
            b.slices(xs, ys, 0.0f, c0, listW, c1);
        }
        {
            constexpr Cut xs[] = {{316.0f, 326.0f, true}};
            constexpr Cut ys[] = {{kHeaderEnd, 80.0f, false}, {100.0f, 140.0f, true}, {158.0f, 166.0f, false}};
            b.slices(xs, ys, listW, c0, p0, c1);
        }
        {
            constexpr Cut xs[] = {{18.0f, 40.0f, false}, {40.0f, 290.0f, true}, {290.0f, 318.0f, false}};
            constexpr Cut ys[] = {{kParchmentTop, 192.0f, false}, {192.0f, 410.0f, true}, {410.0f, kBarStart, false}};
            b.slices(xs, ys, p0, c0, column, c1);
        }
        {
            constexpr Cut xs[] = {{318.0f, kArtRight, false}};
            constexpr Cut ys[] = {{kParchmentTop, 194.0f, false}, {194.0f, 414.0f, true}, {414.0f, kBarStart, false}};
            b.slices(xs, ys, column, c0, W, c1);
        }
        // The rows from the list box's inside (the original's 19, -75) to its right bar; its scroll bar, when they
        // overflow, inside it at the right.
        l.list = b.rect(16.0f, c0 + 3.0f, listW + 2.0f - 16.0f, c1 - c0 - 6.0f);
        l.listBar = b.rect(listW - 18.0f, c0 + 3.0f, 16.0f, c1 - c0 - 6.0f);
        // QuestLogDetailScrollFrame over the parchment box (the original's 19..319 of 22..316, from 175 down to the
        // parchment's edge), its bar 6 right of it in the column, the buttons 16 above and under it (175..436).
        const f32 detailTop = c0 + 175.0f - kParchmentTop;
        l.detail = b.rect(p0 + 1.0f, detailTop, column + 1.0f - (p0 + 1.0f), c1 - 3.0f - detailTop);
        l.detailBar = b.rect(column + 7.0f, detailTop, 16.0f, c1 - detailTop);
        // The empty log's cobwebs hang from the list box's top (their own height); its book over the parchment
        // (174..434).
        const f32 webs = std::min(95.0f * (listW - 12.0f) / 301.0f, c1 - c0 - 1.0f);
        b.empty(0.0f, 0.0f, 301.0f, 95.0f, 16.0f, c0 + 1.0f, listW + 4.0f, c0 + 1.0f + webs);
        b.empty(0.0f, 101.0f, 301.0f, 361.0f, p0 + 1.0f, c0 + 174.0f - kParchmentTop, column + 2.0f, c1 - 2.0f);
        l.noQuests = b.rect(16.0f, c0 + 33.0f, listW - 16.0f, 16.0f);
    } else {
        // The original: the list box over the parchment box, each with its column at the right; the list takes
        // `listShare` of the height (the original's 96 of 361 units: six rows of 16).
        const f32 room = c1 - c0;
        const f32 listH = std::round(std::clamp(room * listShare, 60.0f, std::max(60.0f, room - 80.0f)));
        const f32 l1 = c0 + listH;
        constexpr Cut xs[] = {{kArtLeft, 60.0f, false}, {60.0f, 280.0f, true}, {280.0f, kArtRight, false}};
        {
            constexpr Cut ys[] = {{kHeaderEnd, 92.0f, false}, {92.0f, 147.0f, true}, {147.0f, 168.0f, false}};
            b.slices(xs, ys, 0.0f, c0, W, l1);
        }
        {
            constexpr Cut ys[] = {{168.0f, 192.0f, false}, {192.0f, 414.0f, true}, {414.0f, kBarStart, false}};
            b.slices(xs, ys, 0.0f, l1, W, c1);
        }
        // QuestLogListScrollFrame (19, -75), 300 wide; QuestLogDetailScrollFrame 7 under it; their bars in the
        // columns (6 right of them, the buttons 16 above and under).
        l.list = b.rect(16.0f, c0 + 3.0f, column - 15.0f, listH - 3.0f);
        l.listBar = b.rect(column + 7.0f, c0 + 3.0f, 16.0f, listH - 3.0f);
        l.detail = b.rect(16.0f, l1 + 7.0f, column - 15.0f, c1 - 3.0f - (l1 + 7.0f));
        l.detailBar = b.rect(column + 7.0f, l1 + 7.0f, 16.0f, c1 - (l1 + 7.0f));
        // EmptyQuestLogFrame at (19, -73): the cobwebs over the list box, the book over the parchment box.
        b.empty(0.0f, 0.0f, 301.0f, 95.0f, 16.0f, c0 + 1.0f, column + 2.0f, l1);
        b.empty(0.0f, 101.0f, 301.0f, 361.0f, 16.0f, l1 + 6.0f, column + 2.0f, c1 - 2.0f);
        // QuestLogNoQuestsText 20 left of the title, 90 under it.
        const f32 center = (W + kArtLeft + 35.0f) * 0.5f - kArtLeft - 20.0f;
        l.noQuests = b.rect(center - 100.0f, c0 + 33.0f, 200.0f, 16.0f);
    }
    // QuestLogTitleText at the frame's TOP, in both arrangements: the whole 384-unit frame's middle (its transparent
    // margin too), centered on the title bar.
    l.titleCenterX = l.frame.x + ((W + kArtLeft + 35.0f) * 0.5f - kArtLeft) * k;

    // The header: the portrait (4, -4), the title (TOP, -15: 14 high), the close button (TOPRIGHT -30, -8), the All
    // tab (70, -48 and its button 2 lower), Track Quest (5 right of it, 10 up: 129, -44), the count's box (TOPRIGHT
    // -47, -41).
    l.portrait = b.rect(4.0f - kArtLeft, 4.0f - kArtTop, 64.0f, 64.0f);
    l.title = {l.titleCenterX - 150.0f * k, l.frame.y + (15.0f - kArtTop) * k, 300.0f * k, 14.0f * k};
    l.close = b.rect(W - (kArtRight - 322.0f), 8.0f - kArtTop, 32.0f, 32.0f);
    l.expand = b.rect(70.0f - kArtLeft, 50.0f - kArtTop, 40.0f, 22.0f);
    l.track = b.rect(129.0f - kArtLeft, 44.0f - kArtTop, 20.0f, 20.0f);
    l.countRight = l.frame.x + (W - (kArtRight - 337.0f)) * k;
    l.countTop = l.frame.y + (41.0f - kArtTop) * k;
    // The buttons (BOTTOMLEFT 17, 54 and BOTTOMRIGHT -43, 54 of the 512-unit frame: 437..458), in their slots.
    const f32 buttonY = H - (kArtBottom - 437.0f);
    l.abandon = b.rect(17.0f - kArtLeft, buttonY, 125.0f, 21.0f);
    l.exit = b.rect(W - (kArtRight - 264.0f), buttonY, 77.0f, 21.0f);
    l.share = b.rect(W - (kArtRight - 141.0f), buttonY, 123.0f, 21.0f);
    return l;
}

} // namespace vd

#include "TextTable.h"
#include "core/Hash.h"
#include "core/Text.h"
#include "ui/Context.h"
#include "ui/DrawList.h"
#include "ui/UiAssets.h"
#include "ui/Widgets.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

using namespace vd;
using namespace vd::ui;

namespace {

// A finger as Context reports it (Touch fields, positions in pixels).
Touch finger(i32 id, f32 startX, f32 startY, f32 x, f32 y, f32 dy, u64 downMs) {
    Touch t;
    t.id = id;
    t.startX = startX;
    t.startY = startY;
    t.x = x;
    t.y = y;
    t.dy = dy;
    t.downMs = downMs;
    return t;
}

std::string money(u32 copper) {
    char text[64];
    const usize n = formatMoney(text, sizeof(text), copper);
    CHECK(n == std::strlen(text));
    return text;
}

} // namespace

TEST_CASE("Money: copper splits into gold, silver and copper as the MoneyFrame shows them") {
    // The denominations' letters are texts (VD_MONEY_*): English, then Spanish.
    const std::vector<u8> english =
        test::textTable({{"VD_MONEY_GOLD", "%dg"}, {"VD_MONEY_SILVER", "%ds"}, {"VD_MONEY_COPPER", "%dc"}});
    text::setTable(english);
    const Coins c = splitMoney(1234567);
    CHECK(c.gold == 123);
    CHECK(c.silver == 45);
    CHECK(c.copper == 67);
    CHECK(money(0) == "0c"); // nothing: a lone copper
    CHECK(money(7) == "7c");
    CHECK(money(100) == "1s");
    CHECK(money(105) == "1s 5c");
    CHECK(money(10000) == "1g");
    CHECK(money(10500) == "1g 5s");
    CHECK(money(120003) == "12g 0s 3c"); // silver shown between gold and copper
    CHECK(money(1234567) == "123g 45s 67c");
    CHECK(money(4294967295u) == "429496g 72s 95c");
    const CoinsShown shown = coinsShown(120000);
    CHECK(shown.gold);
    CHECK(!shown.silver);
    CHECK(!shown.copper);
    // Truncated to the buffer, always terminated.
    char small[5];
    const usize n = formatMoney(small, sizeof(small), 1234567);
    CHECK(n == 4);
    CHECK(std::strlen(small) == 4);
    const std::vector<u8> spanish =
        test::textTable({{"VD_MONEY_GOLD", "%do"}, {"VD_MONEY_SILVER", "%dp"}, {"VD_MONEY_COPPER", "%dc"}});
    text::setTable(spanish);
    CHECK(money(1234567) == "123o 45p 67c");
    text::unload();
}

TEST_CASE("Scroll view: a vertical drag takes the finger and moves the content") {
    ScrollState s;
    s.content = 1000.0f;
    const Rect viewport{0.0f, 100.0f, 300.0f, 200.0f};
    const f32 dp = 2.0f; // tap slop 20 px
    u64 now = 1000;
    // Down inside, still within the slop: nothing.
    Touch t = finger(1, 50.0f, 150.0f, 50.0f, 160.0f, 10.0f, now);
    t.began = true;
    scrollStep(s, {&t, 1}, viewport, dp, now);
    CHECK(s.touch == -1);
    CHECK(s.offset == 0.0f);
    CHECK(!scrollClaimed(&t));
    // Up 40 px from where it started: taken; this frame's motion scrolls (content up = offset grows).
    now += 16;
    t = finger(1, 50.0f, 150.0f, 50.0f, 110.0f, -50.0f, 1000);
    scrollStep(s, {&t, 1}, viewport, dp, now);
    CHECK(s.touch == 1);
    CHECK(s.offset == doctest::Approx(50.0f));
    // It follows the finger while held.
    for (int i = 0; i < 5; ++i) {
        now += 16;
        t.y -= 20.0f;
        t.dy = -20.0f;
        scrollStep(s, {&t, 1}, viewport, dp, now);
    }
    CHECK(s.offset == doctest::Approx(150.0f));
    CHECK(s.velocity > 1000.0f); // 20 px per 16 ms
    CHECK(s.atEnd == false);
}

TEST_CASE("Scroll view: sideways drags and fingers from outside are left alone") {
    ScrollState s;
    s.content = 1000.0f;
    const Rect viewport{0.0f, 0.0f, 300.0f, 200.0f};
    Touch sideways = finger(1, 50.0f, 50.0f, 150.0f, 70.0f, 20.0f, 0);
    scrollStep(s, {&sideways, 1}, viewport, 1.0f, 16);
    CHECK(s.touch == -1);
    Touch outside = finger(2, 50.0f, 250.0f, 50.0f, 150.0f, -100.0f, 0);
    scrollStep(s, {&outside, 1}, viewport, 1.0f, 32);
    CHECK(s.touch == -1);
    CHECK(s.offset == 0.0f);
}

TEST_CASE("Scroll view: a released drag flings, slows down and stops at the edges") {
    ScrollState s;
    s.content = 5000.0f;
    const Rect viewport{0.0f, 0.0f, 300.0f, 500.0f};
    u64 now = 0;
    Touch t = finger(3, 100.0f, 400.0f, 100.0f, 400.0f, 0.0f, now);
    t.began = true;
    scrollStep(s, {&t, 1}, viewport, 1.0f, now);
    for (int i = 0; i < 6; ++i) { // 30 px per frame upwards
        now += 16;
        t.y -= 30.0f;
        t.dy = -30.0f;
        scrollStep(s, {&t, 1}, viewport, 1.0f, now);
    }
    REQUIRE(s.touch == 3);
    const f32 released = s.offset;
    now += 16;
    t.released = true;
    t.dy = 0.0f;
    scrollStep(s, {&t, 1}, viewport, 1.0f, now);
    CHECK(scrollClaimed(&t) == false); // this state is not in the widget table
    CHECK(s.touch == 3);               // kept through the frame the finger lifts: it taps nothing
    const f32 speed = s.velocity;
    CHECK(speed > 1000.0f);
    // The finger is gone: the content keeps moving, slower and slower.
    f32 last = s.offset;
    f32 lastStep = 1e9f;
    for (int i = 0; i < 30; ++i) {
        now += 16;
        scrollStep(s, {}, viewport, 1.0f, now);
        const f32 step = s.offset - last;
        CHECK(step > 0.0f);
        CHECK(step <= lastStep + 0.01f);
        lastStep = step;
        last = s.offset;
    }
    CHECK(s.touch == -1);
    CHECK(s.offset > released + 200.0f);
    // It comes to rest by itself, well within a few seconds.
    for (int i = 0; i < 400 && s.velocity != 0.0f; ++i) {
        now += 16;
        scrollStep(s, {}, viewport, 1.0f, now);
    }
    CHECK(s.velocity == 0.0f);
    CHECK(s.offset <= s.maxOffset());

    // A fling towards the end stops there, never beyond.
    s.offset = s.maxOffset() - 50.0f;
    s.velocity = 3000.0f;
    now += 16;
    scrollStep(s, {}, viewport, 1.0f, now);
    now += 16;
    scrollStep(s, {}, viewport, 1.0f, now);
    CHECK(s.offset == s.maxOffset());
    CHECK(s.velocity == 0.0f);
    CHECK(s.atEnd);
    // And towards the start stops at 0.
    s.offset = 20.0f;
    s.velocity = -3000.0f;
    now += 16;
    scrollStep(s, {}, viewport, 1.0f, now);
    CHECK(s.offset == 0.0f);
    CHECK(s.velocity == 0.0f);
}

TEST_CASE("Scroll view: the fling speed is the finger's, whatever the frame and touch rates") {
    const Rect viewport{0.0f, 0.0f, 300.0f, 500.0f};
    // Frames every 7 ms, the finger reported every 16 ms (30 px each time): frames without motion don't slow
    // it.
    ScrollState s;
    s.content = 5000.0f;
    Touch t = finger(1, 100.0f, 450.0f, 100.0f, 450.0f, 0.0f, 0);
    t.began = true;
    scrollStep(s, {&t, 1}, viewport, 1.0f, 0);
    t.began = false;
    u64 nextMove = 16;
    for (u64 now = 7; now <= 150; now += 7) {
        t.dy = 0.0f;
        if (now >= nextMove) {
            t.y -= 30.0f;
            t.dy = -30.0f;
            nextMove += 16;
        }
        scrollStep(s, {&t, 1}, viewport, 1.0f, now);
    }
    REQUIRE(s.touch == 1);
    t.released = true;
    t.dy = 0.0f;
    scrollStep(s, {&t, 1}, viewport, 1.0f, 157);
    CHECK(s.velocity == doctest::Approx(30.0f * 1000.0f / 16.0f).epsilon(0.15));

    // A finger that stops before lifting leaves nothing to fling.
    ScrollState still;
    still.content = 5000.0f;
    t = finger(2, 100.0f, 450.0f, 100.0f, 450.0f, 0.0f, 1000);
    t.began = true;
    scrollStep(still, {&t, 1}, viewport, 1.0f, 1000);
    t.began = false;
    u64 now = 1000;
    for (int i = 0; i < 5; ++i) { // a fast drag...
        now += 16;
        t.y -= 30.0f;
        t.dy = -30.0f;
        scrollStep(still, {&t, 1}, viewport, 1.0f, now);
    }
    for (int i = 0; i < 10; ++i) { // ...then held still for 160 ms
        now += 16;
        t.dy = 0.0f;
        scrollStep(still, {&t, 1}, viewport, 1.0f, now);
    }
    t.released = true;
    scrollStep(still, {&t, 1}, viewport, 1.0f, now + 16);
    CHECK(still.velocity == 0.0f);
}

TEST_CASE("Scroll view: a finger landing on a flinging view stops it") {
    ScrollState s;
    s.content = 5000.0f;
    s.velocity = 2000.0f;
    const Rect viewport{0.0f, 0.0f, 300.0f, 500.0f};
    scrollStep(s, {}, viewport, 1.0f, 16);
    Touch t = finger(4, 100.0f, 100.0f, 100.0f, 100.0f, 0.0f, 20);
    t.began = true;
    const f32 before = s.offset;
    scrollStep(s, {&t, 1}, viewport, 1.0f, 32);
    CHECK(s.touch == 4); // taken: the tap that stops it selects nothing
    CHECK(s.velocity == 0.0f);
    CHECK(s.offset == doctest::Approx(before));
}

TEST_CASE("Scroll view: content changes clamp it, and a view stuck to the end follows new content") {
    ScrollState s;
    s.stickToEnd = true;
    scrollSetContent(s, 300.0f, 200.0f); // starts at the end
    CHECK(s.offset == 100.0f);
    scrollSetContent(s, 500.0f, 200.0f); // a new line: still at the end
    CHECK(s.offset == 300.0f);
    s.offset = 100.0f; // scrolled back to read
    s.atEnd = false;
    scrollSetContent(s, 700.0f, 200.0f);
    CHECK(s.offset == 100.0f);           // stays where the reader is
    scrollSetContent(s, 150.0f, 200.0f); // content shorter than the view
    CHECK(s.offset == 0.0f);
    CHECK(s.maxOffset() == 0.0f);
}

TEST_CASE("Scroll view widget: rows inside ignore the finger that drags the list") {
    Context ui;
    DrawList draw;
    draw.init(nullptr, 256); // solid quads only: no assets needed
    Gui g{ui, draw};
    const Rect viewport{0.0f, 0.0f, 200.0f, 100.0f};
    const WidgetId list = widgetId("test.list"), row = widgetId("test.row");
    u64 now = 1000;
    bool tappedRow = false;
    auto frame = [&](f32 content) {
        draw.reset();
        ui.begin(400.0f, 400.0f, 1.0f, {0.0f, 0.0f, 400.0f, 400.0f}, now);
        const ScrollView view = beginScroll(g, list, viewport);
        const Rect r{0.0f, view.top + 20.0f, 200.0f, 40.0f};
        tappedRow |= tapped(hit(g, row, r));
        endScroll(g, view, content);
        ui.end();
        now += 16;
    };
    frame(400.0f); // the widgets exist: fingers can land on them
    // A tap on the row counts.
    ui.touchEvent(TouchPhase::Down, 7, 50.0f, 30.0f);
    frame(400.0f);
    ui.touchEvent(TouchPhase::Up, 7, 50.0f, 30.0f);
    frame(400.0f);
    CHECK(tappedRow);
    // A drag that starts on the row scrolls the list and taps nothing, even released over the row.
    tappedRow = false;
    ui.touchEvent(TouchPhase::Down, 8, 50.0f, 50.0f);
    frame(400.0f);
    ui.touchEvent(TouchPhase::Move, 8, 50.0f, 20.0f);
    frame(400.0f);
    ui.touchEvent(TouchPhase::Move, 8, 50.0f, 10.0f);
    frame(400.0f);
    ui.touchEvent(TouchPhase::Up, 8, 50.0f, 10.0f);
    frame(400.0f);
    CHECK(!tappedRow);
    // The content moved with the finger (40 px up).
    ui.touchEvent(TouchPhase::Down, 9, 50.0f, 90.0f); // a new finger stops any fling
    frame(400.0f);
    ui.touchEvent(TouchPhase::Up, 9, 50.0f, 90.0f);
    frame(400.0f);
    draw.reset();
    ui.begin(400.0f, 400.0f, 1.0f, {0.0f, 0.0f, 400.0f, 400.0f}, now);
    const ScrollView view = beginScroll(g, list, viewport);
    CHECK(view.offset >= 29.0f);
    CHECK(view.top == doctest::Approx(viewport.y - view.offset));
    endScroll(g, view, 400.0f);
    ui.end();
}

TEST_CASE("Quest difficulty colors follow GetDifficultyColor") {
    CHECK(questDifficultyColor(10, 5) == rgba(255, 26, 26)); // 5 above: impossible
    CHECK(questDifficultyColor(8, 5) == rgba(255, 128, 64)); // 3 above
    CHECK(questDifficultyColor(4, 5) == rgba(255, 255, 0));  // within 2
    CHECK(questDifficultyColor(1, 5) == rgba(64, 191, 64));  // below, still experience (gray level 0 up to 5)
    CHECK(questDifficultyColor(40, 60) == rgba(128, 128, 128)); // gray at 60: 51 and below
    CHECK(questDifficultyColor(52, 60) == rgba(64, 191, 64));
    CHECK(questDifficultyColor(3, 0) == kGold); // player level unknown
    CHECK(qualityColor(0) == rgba(157, 157, 157));
    CHECK(qualityColor(4) == rgba(163, 53, 238));
    CHECK(qualityColor(99) == qualityColor(6));
}

// With the baked ui.vdpk ($VD_PACKS_DIR): the art the widgets and dialogs draw is in the atlas, and text
// measuring agrees with drawing.
TEST_CASE("Widgets: the client art they use is in ui.vdpk; wrapped text measures as it draws") {
    const char* dir = std::getenv("VD_PACKS_DIR");
    const std::filesystem::path pack = dir ? std::filesystem::path(dir) / "ui.vdpk" : std::filesystem::path();
    Assets assets;
    if (!dir || !std::filesystem::exists(pack) || !assets.open(pack)) {
        MESSAGE("VD_PACKS_DIR not set or ui.vdpk missing: skipping");
        return;
    }
    constexpr const char* kArt[] = {
        "Interface\\DialogFrame\\UI-DialogBox-Background.blp",
        "Interface\\DialogFrame\\UI-DialogBox-Border.blp",
        "Interface\\DialogFrame\\UI-DialogBox-Header.blp",
        "Interface\\Tooltips\\UI-Tooltip-Background.blp",
        "Interface\\Tooltips\\UI-Tooltip-Border.blp",
        "Interface\\QuestFrame\\UI-QuestGreeting-TopLeft.blp",
        "Interface\\QuestFrame\\UI-QuestGreeting-TopRight.blp",
        "Interface\\QuestFrame\\UI-QuestGreeting-BotLeft.blp",
        "Interface\\QuestFrame\\UI-QuestGreeting-BotRight.blp",
        "Interface\\QuestFrame\\UI-HorizontalBreak.blp",
        "Interface\\Buttons\\UI-Panel-Button-Up.blp",
        "Interface\\Buttons\\UI-Panel-Button-Down.blp",
        "Interface\\Buttons\\UI-Panel-Button-Highlight.blp",
        "Interface\\Buttons\\UI-Panel-Button-Disabled.blp",
        "Interface\\Buttons\\UI-Panel-MinimizeButton-Up.blp",
        "Interface\\Buttons\\UI-Panel-MinimizeButton-Down.blp",
        "Interface\\Buttons\\UI-Panel-MinimizeButton-Highlight.blp",
        "Interface\\Buttons\\UI-CheckBox-Up.blp",
        "Interface\\Buttons\\UI-CheckBox-Down.blp",
        "Interface\\Buttons\\UI-CheckBox-Highlight.blp",
        "Interface\\Buttons\\UI-CheckBox-Check.blp",
        "Interface\\Buttons\\UI-SliderBar-Background.blp",
        "Interface\\Buttons\\UI-SliderBar-Border.blp",
        "Interface\\Buttons\\UI-SliderBar-Button-Horizontal.blp",
        "Interface\\Buttons\\UI-PlusButton-Up.blp",
        "Interface\\Buttons\\UI-PlusButton-Down.blp",
        "Interface\\Buttons\\UI-PlusButton-Disabled.blp",
        "Interface\\Buttons\\UI-PlusButton-Hilight.blp",
        "Interface\\Buttons\\UI-MinusButton-Up.blp",
        "Interface\\Buttons\\UI-MinusButton-Down.blp",
        "Interface\\Buttons\\UI-MinusButton-Disabled.blp",
        "Interface\\QuestFrame\\UI-QuestTitleHighlight.blp",
        "Interface\\QuestFrame\\UI-QuestLogTitleHighlight.blp",
        "Interface\\Buttons\\UI-Listbox-Highlight.blp",
        "Interface\\Buttons\\UI-Quickslot2.blp",
        "Interface\\Buttons\\UI-Quickslot-Depress.blp",
        "Interface\\Buttons\\ButtonHilight-Square.blp",
        "Interface\\Buttons\\UI-EmptySlot.blp",
        "Interface\\MoneyFrame\\UI-MoneyIcons.blp",
        "Interface\\Common\\Common-Input-Border.blp",
        // app/Dialogs
        "Interface\\GossipFrame\\AvailableQuestIcon.blp",
        "Interface\\GossipFrame\\ActiveQuestIcon.blp",
        "Interface\\GossipFrame\\GossipGossipIcon.blp",
        "Interface\\GossipFrame\\VendorGossipIcon.blp",
        "Interface\\GossipFrame\\TaxiGossipIcon.blp",
        "Interface\\GossipFrame\\TrainerGossipIcon.blp",
        "Interface\\GossipFrame\\HealerGossipIcon.blp",
        "Interface\\GossipFrame\\BinderGossipIcon.blp",
        "Interface\\GossipFrame\\BankerGossipIcon.blp",
        "Interface\\GossipFrame\\PetitionGossipIcon.blp",
        "Interface\\GossipFrame\\TabardGossipIcon.blp",
        "Interface\\GossipFrame\\BattleMasterGossipIcon.blp",
        "Interface\\QuestFrame\\UI-QuestItemNameFrame.blp",
        "Interface\\QuestFrame\\UI-QuestItemHighlight.blp",
        "Interface\\QuestFrame\\UI-Quest-BulletPoint.blp",
        "Interface\\ChatFrame\\ChatFrameBackground.blp",
        "Interface\\ChatFrame\\UI-ChatIcon-Chat-Up.blp",
        "Interface\\ChatFrame\\UI-ChatIcon-Chat-Down.blp",
        "Interface\\ChatFrame\\UI-ChatIcon-ScrollEnd-Up.blp",
        "Interface\\ChatFrame\\UI-ChatIcon-ScrollEnd-Down.blp",
    };
    for (const char* path : kArt) {
        INFO(path);
        CHECK(assets.sprite(assetPathHash(path)).has_value());
    }

    DrawList draw;
    draw.init(&assets, 4096);
    constexpr const char* kTexts[] = {
        "Kobolds have dug in among the trees north of the abbey, $N, and they grow bolder every "
        "night.$B$BHead "
        "into the woods and thin out the vermin you find there.",
        "|cffff80ff[Aeryn] susurra: ¿vienes a la mina de Echo Ridge?|r",
        "Supercalifragilisticexpialidocious-without-any-space-to-break-it",
        "",
        "One line",
    };
    for (const char* text : kTexts) {
        for (const f32 width : {60.0f, 150.0f, 400.0f}) {
            draw.reset();
            const f32 drawn = draw.wrappedText({0.0f, 0.0f, width, 1000.0f}, text, questFont(16.0f));
            CHECK(draw.measureWrapped(width, text, FontId::Friz, 16.0f) == doctest::Approx(drawn));
        }
    }
    // Money width = what money() draws, and the coins are drawn.
    Context ui;
    Gui g{ui, draw};
    draw.reset();
    ui.begin(800.0f, 600.0f, 1.0f, {0.0f, 0.0f, 800.0f, 600.0f}, 0);
    CHECK(money(g, 100.0f, 50.0f, 120003, 14.0f, Align::Left) ==
          doctest::Approx(moneyWidth(draw, 120003, 14.0f)));
    CHECK(draw.quadCount() > 3);
    // A label never draws past its box: "..." cuts it.
    draw.reset();
    label(g, {10.0f, 0.0f, 80.0f, 20.0f}, "A very long quest title that does not fit", gameFont(14.0f));
    f32 right = 0.0f;
    for (const UiVertex& v : draw.vertices()) {
        right = std::max(right, v.x);
    }
    CHECK(right <= 90.0f + 4.0f); // + the shadow and the glyph cells' distance-field padding
    ui.end();
}

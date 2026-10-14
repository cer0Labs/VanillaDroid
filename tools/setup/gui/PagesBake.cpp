// VanillaDroid Setup's window: the game data, what will be baked and why, and the bake with its progress
// (phase 22, gui; vdsetup bake).
#include "SetupWindow.h"

#include <algorithm>

namespace vd::setup::gui {
namespace {

std::string joined(const std::vector<std::string>& items) {
    std::string out;
    for (const std::string& item : items) {
        out += (out.empty() ? "" : ", ") + item;
    }
    return out;
}

std::string percent(f64 fraction) {
    return trf("VD_SETUP_GUI_PERCENT",
               {std::to_string(static_cast<int>(std::clamp(fraction, 0.0, 1.0) * 100.0))});
}

} // namespace

void SetupWindow::updateOptionsPlan() {
    if (!m_bakeForce && m_bakeOnly == 0) {
        m_optionsPlan.reset();
        return;
    }
    if (!m_snapshot.clients.base.usable) {
        return;
    }
    // A plan is a manifest and a folder listing: quick enough for the window's thread, on a click.
    const std::string only =
        m_bakeOnly > 0 ? std::string(bakeSteps()[static_cast<usize>(m_bakeOnly - 1)]) : "";
    m_optionsPlan = planFor(m_settings, packs(), m_snapshot.clients, m_bakeForce, only);
}

void SetupWindow::startBake() {
    const Settings settings = m_settings;
    const std::filesystem::path packsFolder = packs();
    const bool force = m_bakeForce;
    const std::string only =
        m_bakeOnly > 0 ? std::string(bakeSteps()[static_cast<usize>(m_bakeOnly - 1)]) : "";
    const std::filesystem::path baker = m_snapshot.baker;
    m_bakeOutcome = {};
    m_showBakeLog = false;
    m_bakeTask.start(
        [this, settings, packsFolder, force, only, baker](const Operation& operation) -> Task::Apply {
            auto outcome = std::make_shared<BakeOutcomeView>();
            auto fingerprint = std::make_shared<std::string>();
            operation.report("check", -1.0, formatText("VD_SETUP_GUI_BAKE_CHECKING"));
            // The clients and the plan again: what is on disk now decides.
            const Clients clients = checkClients(settings);
            if (!clients.base.usable) {
                outcome->error = formatText("VD_SETUP_GUI_WOW_REFUSED");
            } else {
                const BakePlan plan = planFor(settings, packsFolder, clients, force, only);
                if (!plan.jobs.empty()) {
                    std::error_code error;
                    std::filesystem::create_directories(packsFolder, error);
                    for (const BakeJob& job : plan.jobs) {
                        outcome->baked.push_back(stepTitle(job));
                    }
                    Result<BakeOutcome> result =
                        runBake(plan.jobs, bakeRunFor(settings, clients, baker, packsFolder), operation);
                    if (!result) {
                        outcome->error = result.error().message;
                    } else {
                        outcome->cancelled = result.value().cancelled;
                        outcome->failed = result.value().failed;
                    }
                }
                if (outcome->error.empty() && !outcome->cancelled && outcome->failed.empty()) {
                    *fingerprint = fingerprintOf(settings, clients);
                }
            }
            outcome->shown = true;
            return [this, outcome, fingerprint]() {
                m_bakeOutcome = *outcome;
                m_bakeOutcome.seconds = m_bakeTask.state().seconds;
                m_showBakeLog = !m_bakeOutcome.failed.empty();
                if (!fingerprint->empty()) {
                    m_settings.lastBake = *fingerprint;
                    saveSettings();
                }
                refresh(false);
            };
        });
}

void SetupWindow::drawBakeProgress() {
    const Task::State state = m_bakeTask.state();
    spinner();
    ImGui::SameLine();
    text(state.text.empty() ? tr("VD_SETUP_GUI_BAKE_CHECKING") : std::string_view(state.text));
    // A step that logs no counts moves the bar only when it ends: until then, the bar shows that it works.
    const bool known = state.step == "bake" && state.fraction > 0;
    progressBar(known ? state.fraction : -1.0, known ? percent(state.fraction) : "");
    std::string times = trf("VD_SETUP_GUI_ELAPSED", {formatDuration(state.seconds)});
    if (const f64 left = estimateRemaining(state.fraction, state.seconds);
        left >= 0 && state.step == "bake") {
        times += "    " + trf("VD_SETUP_GUI_REMAINING", {formatDuration(left)});
    }
    textMuted(times);
    if (!state.detail.empty()) {
        textColored(kMuted, state.detail, false); // one line: the baker's latest
    }
    gap(0.3f);
    if (button(label("VD_SETUP_GUI_CANCEL", "cancelbake"))) {
        m_bakeTask.cancel();
    }
    textMuted(tr("VD_SETUP_GUI_BAKE_CANCEL_NOTE"));
}

void SetupWindow::drawBake() {
    title(tr("VD_SETUP_GUI_BAKE_TITLE"));
    textWrapped(tr("VD_SETUP_GUI_BAKE_INTRO"));
    gap(0.5f);
    const Snapshot& s = m_snapshot;

    if (m_bakeTask.running()) {
        drawBakeProgress();
    } else if (!s.clients.base.usable) {
        if (!s.done || m_refreshTask.running()) {
            spinner();
            ImGui::SameLine();
            text(tr("VD_SETUP_GUI_BAKE_CHECKING"));
        } else {
            textMuted(tr("VD_SETUP_GUI_BAKE_NEEDS_WOW"));
            if (!m_wizard && button(label("VD_SETUP_GUI_GO_WOW", "gowow"))) {
                goTo(Page::Wow);
            }
        }
        return;
    }

    // The outcome of the latest bake.
    if (!m_bakeTask.running() && m_bakeOutcome.shown) {
        if (!m_bakeOutcome.error.empty()) {
            messageText(Message::Level::Error, trf("VD_SETUP_BAKE_FAILED", {m_bakeOutcome.error}));
        } else if (m_bakeOutcome.cancelled) {
            messageText(Message::Level::Warning, tr("VD_SETUP_GUI_BAKE_CANCELLED"));
        } else if (!m_bakeOutcome.failed.empty()) {
            messageText(Message::Level::Error,
                        trf("VD_SETUP_GUI_BAKE_STEPS_FAILED", {joined(m_bakeOutcome.failed)}));
        } else if (!s.plan.jobs.empty() || s.packs.empty()) {
            // Some steps (Advanced): the rest is still to bake.
            messageText(Message::Level::Info,
                        trf("VD_SETUP_GUI_BAKE_BAKED",
                            {formatDuration(m_bakeOutcome.seconds), joined(m_bakeOutcome.baked)}));
        } else {
            messageText(Message::Level::Info,
                        trf("VD_SETUP_GUI_BAKE_DONE", {formatDuration(m_bakeOutcome.seconds)}));
            if (!m_wizard && primaryButton(label("VD_SETUP_GUI_NEXT_PHONE", "nextphone"))) {
                goTo(Page::Phone);
            }
        }
        gap(0.3f);
    }

    // Where the packs go.
    heading(tr("VD_SETUP_GUI_PACKS_FOLDER"));
    selectableText(utf8FromPath(packs()), "packsdir");
    ImGui::SameLine();
    if (button(label("VD_SETUP_GUI_CHANGE", "packschange"), !busy() && !m_dialogOpen)) {
        openFolderDialog(DialogPurpose::Packs, packs());
    }
    ImGui::SameLine();
    std::error_code error;
    if (button(label("VD_SETUP_GUI_OPEN_FOLDER", "packsopen"),
               std::filesystem::is_directory(packs(), error))) {
        openPath(utf8FromPath(packs()));
    }
    textMuted(trf("VD_SETUP_GUI_PACKS_STATE", {std::to_string(s.packs.size()), formatBytes(s.packBytes)}));
    if (s.freeKnown) {
        if (s.freeSpace < kFullBakeBytes) {
            messageText(Message::Level::Warning,
                        trf("VD_SETUP_BAKE_LOW_SPACE", {formatBytes(s.freeSpace), utf8FromPath(packs())}));
        } else {
            textMuted(trf("VD_SETUP_GUI_FREE_SPACE", {formatBytes(s.freeSpace)}));
        }
    }

    if (!m_bakeTask.running()) {
        // What will be baked, and why.
        heading(tr("VD_SETUP_GUI_BAKE_PLAN"));
        const BakePlan& plan = m_optionsPlan ? *m_optionsPlan : s.plan;
        if (m_refreshTask.running()) {
            spinner();
            ImGui::SameLine();
            text(tr("VD_SETUP_GUI_BAKE_CHECKING"));
        }
        if (plan.inputsChanged) {
            messageText(Message::Level::Warning, tr("VD_SETUP_BAKE_INPUTS_CHANGED"));
        }
        if (plan.noManifest) {
            messageText(Message::Level::Info, tr("VD_SETUP_GUI_BAKE_NO_MANIFEST"));
        }
        if (s.clients.localeSkipped) {
            messageText(Message::Level::Warning, tr("VD_SETUP_BAKE_LOCALE_CLIENT_SKIPPED"));
        }
        if (s.baker.empty()) {
            messageText(Message::Level::Error, tr("VD_SETUP_GUI_BAKE_NO_BAKER"));
        }
        if (plan.jobs.empty() && m_bakeOnly > 0) {
            const std::string step(bakeSteps()[static_cast<usize>(m_bakeOnly - 1)]);
            messageText(Message::Level::Info,
                        trf("VD_SETUP_GUI_BAKE_STEP_UP_TO_DATE", {stepTitle(BakeJob{step, {}})}));
        } else if (plan.jobs.empty()) {
            messageText(Message::Level::Info, tr("VD_SETUP_BAKE_UP_TO_DATE"));
        } else {
            gap(0.2f);
            if (primaryButton(label("VD_SETUP_GUI_BAKE_START", "startbake"),
                              !s.baker.empty() && !busy() && !m_refreshTask.running())) {
                startBake();
            }
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            textColored(kMuted, trf("VD_SETUP_GUI_BAKE_TODO", {std::to_string(plan.jobs.size())}), false);
            if (ImGui::BeginTable("##plan", 2,
                                  ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                      ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn(label("VD_SETUP_GUI_BAKE_COL_STEP", "step").c_str(),
                                        ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupColumn(label("VD_SETUP_GUI_BAKE_COL_WHY", "why").c_str(),
                                        ImGuiTableColumnFlags_WidthStretch, 1.6f);
                ImGui::TableHeadersRow();
                for (const BakeJob& job : plan.jobs) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    textWrapped(stepTitle(job));
                    ImGui::TableNextColumn();
                    const std::vector<std::string> files = outdatedPacksOf(plan, job);
                    std::string why;
                    if (plan.inputsChanged || m_bakeForce) {
                        why = tr("VD_SETUP_GUI_WHY_ALL");
                    } else if (std::find(plan.missing.begin(), plan.missing.end(), job.step) !=
                               plan.missing.end()) {
                        why = tr("VD_SETUP_GUI_WHY_MISSING");
                    } else if (!files.empty()) {
                        why = trf("VD_SETUP_GUI_WHY_OUTDATED", {joined(files)});
                    } else {
                        why = tr("VD_SETUP_GUI_WHY_CHOSEN");
                    }
                    textColored(kMuted, why);
                }
                ImGui::EndTable();
            }
        }

        gap(0.3f);
        if (ImGui::CollapsingHeader(label("VD_SETUP_GUI_ADVANCED", "advanced").c_str())) {
            bool changed = ImGui::Checkbox(label("VD_SETUP_GUI_BAKE_FORCE", "force").c_str(), &m_bakeForce);
            const std::span<const std::string_view> steps = bakeSteps();
            const std::string current =
                m_bakeOnly == 0
                    ? std::string(tr("VD_SETUP_GUI_BAKE_ALL_STEPS"))
                    : stepTitle(BakeJob{std::string(steps[static_cast<usize>(m_bakeOnly - 1)]), {}});
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
            if (ImGui::BeginCombo(label("VD_SETUP_GUI_BAKE_ONLY", "only").c_str(), current.c_str())) {
                for (int i = 0; i <= static_cast<int>(steps.size()); ++i) {
                    const std::string name =
                        i == 0 ? std::string(tr("VD_SETUP_GUI_BAKE_ALL_STEPS"))
                               : stepTitle(BakeJob{std::string(steps[static_cast<usize>(i - 1)]), {}});
                    if (ImGui::Selectable((name + "##only" + std::to_string(i)).c_str(), m_bakeOnly == i)) {
                        changed = changed || m_bakeOnly != i;
                        m_bakeOnly = i;
                    }
                }
                ImGui::EndCombo();
            }
            if (changed) {
                updateOptionsPlan();
            }
        }
    }

    // The baker's own lines (English: they are its log).
    const std::vector<std::string> lines = m_bakeTask.log();
    if (!lines.empty()) {
        gap(0.3f);
        ImGui::SetNextItemOpen(m_showBakeLog, ImGuiCond_Always);
        m_showBakeLog = ImGui::CollapsingHeader(label("VD_SETUP_GUI_BAKE_LOG", "bakelog").c_str());
        if (m_showBakeLog) {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, kPanel);
            ImGui::BeginChild("##bakelines", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 12.0f),
                              ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
            for (const std::string& line : lines) {
                const bool failed = line.rfind("[ERROR]", 0) == 0;
                textColored(failed ? kError : kMuted, line, false);
            }
            if (m_bakeTask.running()) {
                ImGui::SetScrollHereY(1.0f);
            }
            ImGui::EndChild();
            ImGui::PopStyleColor();
        }
    }
}

} // namespace vd::setup::gui

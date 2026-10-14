#include "GuiTask.h"

namespace vd::setup::gui {
namespace {

std::atomic<void (*)()> g_wake{nullptr};

} // namespace

void setWakeFunction(void (*wake)()) {
    g_wake.store(wake);
}

void wakeWindow() {
    if (void (*wake)() = g_wake.load()) {
        wake();
    }
}

Task::~Task() {
    stop();
}

bool Task::start(Body body) {
    if (running()) {
        return false;
    }
    if (m_thread.joinable()) {
        m_thread.join(); // ended, but its Apply was never polled
    }
    m_cancel.reset();
    m_finished = false;
    m_start = Clock::now();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_state = State{};
        m_state.running = true;
        m_log.clear();
        m_apply = nullptr;
    }
    m_thread = std::thread([this, body = std::move(body)]() {
        Operation operation;
        operation.cancel = &m_cancel;
        operation.progress = [this](const ProgressEvent& event) { report(event); };
        Apply apply = body(operation);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_apply = std::move(apply);
            m_state.running = false;
            m_state.seconds = secondsBetween(m_start, Clock::now());
        }
        m_finished = true;
        wakeWindow();
    });
    wakeWindow();
    return true;
}

void Task::cancel() {
    m_cancel.cancel();
}

bool Task::running() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_state.running;
}

Task::State Task::state() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    State state = m_state;
    if (state.running) {
        state.seconds = secondsBetween(m_start, Clock::now());
    }
    return state;
}

std::vector<std::string> Task::log() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return std::vector<std::string>(m_log.begin(), m_log.end());
}

bool Task::poll() {
    if (!m_finished.load()) {
        return false;
    }
    m_finished = false;
    if (m_thread.joinable()) {
        m_thread.join();
    }
    Apply apply;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        apply = std::move(m_apply);
        m_apply = nullptr;
    }
    if (apply) {
        apply();
    }
    return true;
}

void Task::stop() {
    m_cancel.cancel();
    if (m_thread.joinable()) {
        m_thread.join();
    }
    m_finished = false;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_apply = nullptr;
    m_state.running = false;
}

void Task::report(const ProgressEvent& event) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (event.step == "bake-log") {
            m_state.detail = std::string(event.text);
            m_log.emplace_back(event.text);
            while (m_log.size() > kMaxLogLines) {
                m_log.pop_front();
            }
        } else {
            m_state.step = std::string(event.step);
            if (event.fraction >= 0) {
                m_state.fraction = event.fraction;
            }
            if (!event.text.empty()) {
                m_state.text = std::string(event.text);
            }
        }
    }
    wakeWindow();
}

} // namespace vd::setup::gui

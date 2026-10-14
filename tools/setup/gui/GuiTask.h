#pragma once

#include "SetupCommon.h"
#include "core/Clock.h"
#include "core/Types.h"

#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Phase 22 (gui): the window never waits. Each long operation (checking a WoW, a bake, adb) runs as a Task on
// its own thread with the core's Operation (progress, cancel); what it found is applied on the window's
// thread when it ends, so the window's state is only ever touched there.
namespace vd::setup::gui {

// Wakes the window's loop from any thread (main.cpp: an SDL event; at most one waits in the queue).
void setWakeFunction(void (*wake)());
void wakeWindow();

class Task {
public:
    struct State {
        bool running = false;
        f64 fraction = -1.0; // of the whole operation; negative when unknown
        std::string step;    // the core's step id ("bake", "hash", "push"...)
        std::string text;    // the latest line for the player
        std::string detail;  // the latest line of a tool (the baker's)
        f64 seconds = 0.0;   // since it started (or how long it took, once it ended)
    };
    // Runs on the window's thread when the body has ended.
    using Apply = std::function<void()>;
    // Runs on the task's thread.
    using Body = std::function<Apply(const Operation& operation)>;
    static constexpr usize kMaxLogLines = 400;

    Task() = default;
    ~Task();
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    // False while another body runs.
    bool start(Body body);
    void cancel();
    bool running() const;
    State state() const;
    // The tool lines kept ("bake-log"), oldest first.
    std::vector<std::string> log() const;
    // The window's thread, once a frame: when the body has ended, joins its thread and runs its Apply. True
    // then.
    bool poll();
    // Cancels and waits for the body (closing the window); its Apply is dropped.
    void stop();

private:
    void report(const ProgressEvent& event);

    mutable std::mutex m_mutex;
    State m_state;
    std::deque<std::string> m_log;
    Apply m_apply;
    std::thread m_thread;
    std::atomic<bool> m_finished{false};
    CancelToken m_cancel;
    Clock::time_point m_start{};
};

} // namespace vd::setup::gui

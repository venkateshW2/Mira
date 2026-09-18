#pragma once

#include <functional>
#include <map>
#include <memory>

#include "Sa3Worker.h"

// ONE SA3 worker for the whole app, shared by every window that can generate.
//
// This exists because project windows became plural. A worker holds the DiT and the
// decoder; a 30-second generation peaked at 11 GB on a 16 GB machine. Two windows each
// owning one would not be twice as capable, it would be two processes swapping against
// each other, and the failure looks exactly like "the model got slower" -- the thing the
// memory readout was added to make visible.
//
// Sharing is correct without a queue of our own: Sa3Worker is a conversation with a
// single-threaded child, requests are answered in order, and replies are matched by id.
// So a second window's request simply waits its turn inside the worker, which is what
// "wait your turn" should mean anyway.
//
// What DOES need managing is the two singular callbacks. Sa3Worker has one onLog and one
// onExit; with several windows attached, whoever assigned last would silently own them
// and every other window's console would go quiet. The hub owns them and fans out.
class Sa3WorkerHub
{
public:
    explicit Sa3WorkerHub(juce::File studioRootIn) : studioRoot(std::move(studioRootIn)) {}

    struct Listener
    {
        std::function<void(juce::String)> onLog;
        std::function<void(int)> onExit;
    };

    int addListener(Listener l)
    {
        const int token = nextToken++;
        listeners[token] = std::move(l);
        return token;
    }
    void removeListener(int token) { listeners.erase(token); }

    // Starts on first use rather than at app launch: the worker costs a python process
    // and, on its first generate, a ~44 s model load. An app opened to organise a library
    // should pay neither.
    mira::Sa3Worker* get(juce::String& errorOut)
    {
        if (worker != nullptr && worker->isRunning()) return worker.get();
        worker = std::make_unique<mira::Sa3Worker>();
        worker->onLog = [this](juce::String line) {
            for (auto& [_, l] : listeners) if (l.onLog) l.onLog(line);
        };
        worker->onExit = [this](int code) {
            for (auto& [_, l] : listeners) if (l.onExit) l.onExit(code);
        };
        const auto script = studioRoot.getChildFile("sa3_worker.py");
        if (!worker->start(pythonFor(studioRoot), script, "medium", "same-l", errorOut))
        {
            worker.reset();
            return nullptr;
        }
        return worker.get();
    }

    bool isRunning() const { return worker != nullptr && worker->isRunning(); }

    // Stop, as the button means it: the worker is blocked inside MLX and will not read
    // stdin until the current request returns, so killing the process is the only real
    // stop. With the worker shared that also cancels anything ANOTHER window had queued,
    // which is why the button says so.
    void restart()
    {
        worker.reset();
        juce::String error;
        get(error);
    }

    // How many windows are attached. The UI uses it to decide whether Stop needs to warn
    // about affecting someone else's run.
    int listenerCount() const { return static_cast<int>(listeners.size()); }

    static juce::File pythonFor(const juce::File& studioRoot)
    {
        return studioRoot.getChildFile("stable-audio-3/optimized/mlx/.venv/bin/python");
    }

private:
    juce::File studioRoot;
    std::unique_ptr<mira::Sa3Worker> worker;
    std::map<int, Listener> listeners;
    int nextToken = 1;
};

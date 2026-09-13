#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>  // MessageManager::callAsync — replies land on the message thread

namespace mira {

// A persistent SA3 inference worker (sa3-studio/sa3_worker.py), kept alive for the life
// of the generate window.
//
// Why this exists rather than juce::ChildProcess: ChildProcess has no way to WRITE to a
// child's stdin (juce_ChildProcess.h exposes only start() + readProcessOutput()), and the
// whole point of the worker is a conversation -- send a request, keep the loaded model,
// send another. So this is a small posix_spawn + pipe pair instead.
//
// Why a persistent process at all: the measured DiT load is ~44 s cold. Spawning
// sa3_mlx.py per click would put that on every generation and make this worse than the
// gradio page it replaces. With the worker warm, a second generation reloads nothing
// (measured dit_load_ms: 0.0) and a LoRA strength change is an in-place ~26/80 ms swap.
//
// Threading: one reader thread owns the stdout pipe and parses JSON Lines. Replies are
// delivered on the JUCE message thread via MessageManager::callAsync, so callbacks can
// touch UI directly. Requests may be sent from any thread; writes are mutex-guarded.
class Sa3Worker {
public:
    // `ok` is the protocol's own ok flag; `payload` is the parsed reply object (or an
    // object carrying "error" when ok is false). Always called on the message thread.
    using Reply = std::function<void(bool ok, juce::var payload)>;

    Sa3Worker();
    ~Sa3Worker();

    // Spawns `python sa3_worker.py --dit ... --decoder ...`. Returns false if the
    // interpreter or the script is missing -- both are reported rather than silently
    // degrading, because a missing venv is the single most likely setup failure.
    bool start(const juce::File& python, const juce::File& script,
               const juce::String& dit, const juce::String& decoder,
               juce::String& errorOut);
    void stop();
    bool isRunning() const { return running.load(); }

    // Sends one request. `request` must be a JSON object; an "id" is added here. The
    // reply callback fires once, on the message thread. Requests are answered in order
    // (the worker is single-threaded by design), so this is a simple id->callback map.
    void send(juce::DynamicObject::Ptr request, Reply onReply);

    // Fired on the message thread for every stderr line -- the worker logs progress and
    // tracebacks there, deliberately keeping stdout pure protocol.
    std::function<void(juce::String)> onLog;
    // Fired when the child exits unexpectedly, so the UI can stop pretending it is warm.
    std::function<void(int exitCode)> onExit;

private:
    void readLoop();
    void readErrLoop();
    void deliver(int id, bool ok, juce::var payload);

    int stdinFd = -1, stdoutFd = -1, stderrFd = -1;
    int childPid = -1;
    std::atomic<bool> running { false };
    std::atomic<int> nextId { 1 };
    std::thread reader, errReader;
    std::mutex writeMutex, pendingMutex;
    std::vector<std::pair<int, Reply>> pending;
};

} // namespace mira

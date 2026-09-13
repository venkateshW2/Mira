#include "Sa3Worker.h"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace mira {

namespace {

// Reads one '\n'-terminated line from `fd`. Returns false at EOF. Byte-at-a-time is fine
// here: the protocol is a handful of lines per generation, not a stream.
bool readLine(int fd, std::string& out) {
    out.clear();
    char c;
    while (true) {
        const auto n = ::read(fd, &c, 1);
        if (n <= 0) return !out.empty();
        if (c == '\n') return true;
        out.push_back(c);
    }
}

} // namespace

Sa3Worker::Sa3Worker() = default;
Sa3Worker::~Sa3Worker() { stop(); }

bool Sa3Worker::start(const juce::File& python, const juce::File& script,
                      const juce::String& dit, const juce::String& decoder,
                      juce::String& errorOut) {
    if (running.load()) return true;
    if (!python.existsAsFile()) {
        errorOut = "Python not found at " + python.getFullPathName()
                 + "\n\nThe MLX venv is missing. See sa3-studio/SETUP.md.";
        return false;
    }
    if (!script.existsAsFile()) {
        errorOut = "Worker script not found at " + script.getFullPathName();
        return false;
    }

    int inPipe[2], outPipe[2], errPipe[2];
    if (::pipe(inPipe) != 0 || ::pipe(outPipe) != 0 || ::pipe(errPipe) != 0) {
        errorOut = "Could not create pipes: " + juce::String(std::strerror(errno));
        return false;
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, inPipe[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, outPipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, errPipe[1], STDERR_FILENO);
    // The child must not inherit our ends, or EOF never arrives when we close them.
    posix_spawn_file_actions_addclose(&actions, inPipe[1]);
    posix_spawn_file_actions_addclose(&actions, outPipe[0]);
    posix_spawn_file_actions_addclose(&actions, errPipe[0]);

    const auto pyPath = python.getFullPathName().toStdString();
    const auto scPath = script.getFullPathName().toStdString();
    const auto ditStr = dit.toStdString();
    const auto decStr = decoder.toStdString();
    std::vector<char*> argv {
        const_cast<char*>(pyPath.c_str()), const_cast<char*>(scPath.c_str()),
        const_cast<char*>("--dit"), const_cast<char*>(ditStr.c_str()),
        const_cast<char*>("--decoder"), const_cast<char*>(decStr.c_str()),
        nullptr
    };

    int pid = 0;
    const int rc = ::posix_spawn(&pid, pyPath.c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(inPipe[0]); ::close(outPipe[1]); ::close(errPipe[1]);

    if (rc != 0) {
        ::close(inPipe[1]); ::close(outPipe[0]); ::close(errPipe[0]);
        errorOut = "Could not launch the worker: " + juce::String(std::strerror(rc));
        return false;
    }

    childPid = pid;
    stdinFd = inPipe[1];
    stdoutFd = outPipe[0];
    stderrFd = errPipe[0];
    running = true;
    reader = std::thread([this] { readLoop(); });
    errReader = std::thread([this] { readErrLoop(); });
    return true;
}

void Sa3Worker::stop() {
    if (!running.exchange(false)) return;
    // Ask politely first: closing stdin ends the worker's `for line in sys.stdin` loop,
    // which lets it release the GPU cleanly rather than being killed mid-generation.
    if (stdinFd >= 0) { ::close(stdinFd); stdinFd = -1; }
    if (childPid > 0) {
        int status = 0;
        for (int i = 0; i < 50 && ::waitpid(childPid, &status, WNOHANG) == 0; ++i)
            juce::Thread::sleep(100);          // up to 5s for a clean exit
        if (::waitpid(childPid, &status, WNOHANG) == 0) {
            ::kill(childPid, SIGKILL);
            ::waitpid(childPid, &status, 0);
        }
        childPid = -1;
    }
    if (stdoutFd >= 0) { ::close(stdoutFd); stdoutFd = -1; }
    if (stderrFd >= 0) { ::close(stderrFd); stderrFd = -1; }
    if (reader.joinable()) reader.join();
    if (errReader.joinable()) errReader.join();

    // Nothing will ever answer the in-flight requests now; fail them rather than leaving
    // the UI spinning forever on a reply that cannot come.
    std::vector<std::pair<int, Reply>> stranded;
    { std::lock_guard<std::mutex> lock(pendingMutex); stranded.swap(pending); }
    for (auto& [id, cb] : stranded) {
        juce::ignoreUnused(id);
        auto obj = new juce::DynamicObject();
        obj->setProperty("error", "worker stopped");
        juce::MessageManager::callAsync([cb, v = juce::var(obj)] { cb(false, v); });
    }
}

void Sa3Worker::send(juce::DynamicObject::Ptr request, Reply onReply) {
    if (!running.load()) {
        auto obj = new juce::DynamicObject();
        obj->setProperty("error", "worker is not running");
        juce::MessageManager::callAsync([onReply, v = juce::var(obj)] { onReply(false, v); });
        return;
    }
    const int id = nextId.fetch_add(1);
    request->setProperty("id", id);
    { std::lock_guard<std::mutex> lock(pendingMutex); pending.emplace_back(id, std::move(onReply)); }

    const auto line = juce::JSON::toString(juce::var(request.get()), true) + "\n";
    const auto utf8 = line.toRawUTF8();
    const auto len = std::strlen(utf8);
    std::lock_guard<std::mutex> lock(writeMutex);
    for (size_t written = 0; written < len;) {
        const auto n = ::write(stdinFd, utf8 + written, len - written);
        if (n <= 0) break;                      // child gone; readLoop will report it
        written += static_cast<size_t>(n);
    }
}

void Sa3Worker::deliver(int id, bool ok, juce::var payload) {
    Reply cb;
    {
        std::lock_guard<std::mutex> lock(pendingMutex);
        for (auto it = pending.begin(); it != pending.end(); ++it) {
            if (it->first == id) { cb = std::move(it->second); pending.erase(it); break; }
        }
    }
    // id == null is an unsolicited event (the startup "ready" line, or a fatal): there is
    // no pending callback for it, and that is not an error.
    if (cb) juce::MessageManager::callAsync([cb, ok, payload] { cb(ok, payload); });
}

void Sa3Worker::readLoop() {
    std::string line;
    while (readLine(stdoutFd, line)) {
        if (line.empty()) continue;
        const auto parsed = juce::JSON::parse(juce::String::fromUTF8(line.c_str()));
        if (auto* obj = parsed.getDynamicObject()) {
            const bool ok = static_cast<bool>(obj->getProperty("ok"));
            const auto idVar = obj->getProperty("id");
            if (idVar.isInt()) deliver(static_cast<int>(idVar), ok, parsed);
            else if (onLog) {
                auto text = juce::JSON::toString(parsed, true);
                juce::MessageManager::callAsync([cb = onLog, text] { cb(text); });
            }
        }
    }
    if (running.load()) {          // died on its own rather than via stop()
        running = false;
        if (onExit) juce::MessageManager::callAsync([cb = onExit] { cb(-1); });
    }
}

void Sa3Worker::readErrLoop() {
    std::string line;
    while (readLine(stderrFd, line)) {
        if (line.empty() || !onLog) continue;
        auto text = juce::String::fromUTF8(line.c_str());
        juce::MessageManager::callAsync([cb = onLog, text] { cb(text); });
    }
}

} // namespace mira

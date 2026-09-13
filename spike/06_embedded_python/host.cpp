// Minimal stand-in for mira's JUCE ChildProcess call site: spawns the bundled
// interpreter on the bundled worker and relays its output. Proves the process
// seam works from inside a hardened-runtime bundle.
#include <array>
#include <cstdio>
#include <iostream>
#include <string>
#include <libgen.h>
#include <mach-o/dyld.h>
#include <vector>

static std::string exeDir() {
    std::vector<char> buf(4096);
    uint32_t sz = buf.size();
    _NSGetExecutablePath(buf.data(), &sz);
    return std::string(dirname(buf.data()));
}

int main() {
    const std::string res = exeDir() + "/../Resources";
    // PYTHONHOME pins the interpreter to the bundled stdlib; PYTHONPATH adds
    // the bundled site dir. Without both, a stray system Python can win.
    // PYTHONDONTWRITEBYTECODE is load-bearing, not hygiene: without it the
    // interpreter writes __pycache__/*.pyc into Contents/Resources on first
    // run, which breaks the bundle's code signature ("a sealed resource is
    // missing or invalid") and would fail Gatekeeper after notarisation.
    const std::string cmd =
        "PYTHONDONTWRITEBYTECODE=1 "
        "PYTHONHOME='" + res + "/python' "
        "PYTHONPATH='" + res + "/pysite' "
        "'" + res + "/python/bin/python3.11' '" + res + "/worker.py' 2>&1";

    std::cout << "[host] launching bundled interpreter\n";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) { std::cerr << "[host] popen failed\n"; return 1; }
    std::array<char, 512> line{};
    while (fgets(line.data(), line.size(), p)) std::cout << line.data();
    const int rc = pclose(p);
    std::cout << "[host] child exit " << (rc == 0 ? 0 : rc) << "\n";
    return rc == 0 ? 0 : 1;
}

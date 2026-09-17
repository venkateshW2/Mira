#include "PathNormalise.h"

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#endif

namespace mira {
namespace {

#ifdef __APPLE__
std::string normalise(const std::string& utf8, CFStringNormalizationForm form) {
    if (utf8.empty()) return utf8;
    CFStringRef immutable = CFStringCreateWithBytes(
        kCFAllocatorDefault, reinterpret_cast<const UInt8*>(utf8.data()),
        static_cast<CFIndex>(utf8.size()), kCFStringEncodingUTF8, false);
    // Invalid UTF-8 returns null rather than a substitute. Returning the input unchanged
    // is right here: a path mira cannot decode is one it should pass through untouched,
    // not one it should guess at (convention 6 -- never silently substitute).
    if (immutable == nullptr) return utf8;

    CFMutableStringRef mutableCopy = CFStringCreateMutableCopy(kCFAllocatorDefault, 0, immutable);
    CFRelease(immutable);
    if (mutableCopy == nullptr) return utf8;

    CFStringNormalize(mutableCopy, form);

    // Ask for the byte count first: CFStringGetCString needs a buffer big enough for the
    // WHOLE string, and a decomposed form can be meaningfully longer than its input.
    CFIndex length = CFStringGetLength(mutableCopy);
    CFIndex maxBytes = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
    std::string out(static_cast<size_t>(maxBytes), '\0');
    const bool ok = CFStringGetCString(mutableCopy, out.data(), maxBytes, kCFStringEncodingUTF8);
    CFRelease(mutableCopy);
    if (!ok) return utf8;

    out.resize(std::char_traits<char>::length(out.c_str()));
    return out;
}
#endif

} // namespace

std::string toNfd(const std::string& utf8) {
#ifdef __APPLE__
    return normalise(utf8, kCFStringNormalizationFormD);
#else
    return utf8;
#endif
}

std::string toNfc(const std::string& utf8) {
#ifdef __APPLE__
    return normalise(utf8, kCFStringNormalizationFormC);
#else
    return utf8;
#endif
}

bool pathsEquivalent(const std::string& a, const std::string& b) {
    if (a == b) return true; // the overwhelmingly common case, and free
    return toNfc(a) == toNfc(b);
}

} // namespace mira

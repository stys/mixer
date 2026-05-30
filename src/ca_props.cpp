#include "ca_props.h"

#include <CoreFoundation/CoreFoundation.h>

#include <cstdio>
#include <cstring>

std::optional<std::string> getString(AudioObjectID id, AudioObjectPropertySelector selector) {
    AudioObjectPropertyAddress addr = makeAddress(selector);
    CFStringRef cf = nullptr;
    UInt32 size = sizeof(CFStringRef);
    if (AudioObjectGetPropertyData(id, &addr, 0, nullptr, &size, &cf) != noErr || cf == nullptr) {
        return std::nullopt;
    }
    const CFIndex len = CFStringGetLength(cf);
    const CFIndex maxBytes = CFStringGetMaximumSizeForEncoding(len, kCFStringEncodingUTF8) + 1;
    std::string out(static_cast<size_t>(maxBytes), '\0');
    std::optional<std::string> result;
    if (CFStringGetCString(cf, out.data(), maxBytes, kCFStringEncodingUTF8)) {
        out.resize(std::strlen(out.c_str()));
        result = std::move(out);
    }
    CFRelease(cf);
    return result;
}

std::optional<double> getDouble(AudioObjectID id, AudioObjectPropertySelector selector) {
    AudioObjectPropertyAddress addr = makeAddress(selector);
    double v = 0;
    UInt32 size = sizeof(v);
    if (AudioObjectGetPropertyData(id, &addr, 0, nullptr, &size, &v) != noErr) return std::nullopt;
    return v;
}

std::vector<AudioValueRange> getRanges(AudioObjectID id, AudioObjectPropertySelector selector) {
    AudioObjectPropertyAddress addr = makeAddress(selector);
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(id, &addr, 0, nullptr, &size) != noErr) return {};
    const size_t count = size / sizeof(AudioValueRange);
    if (count == 0) return {};
    std::vector<AudioValueRange> ranges(count);
    if (AudioObjectGetPropertyData(id, &addr, 0, nullptr, &size, ranges.data()) != noErr) return {};
    return ranges;
}

UInt32 channelCount(AudioObjectID id, AudioObjectPropertyScope scope) {
    AudioObjectPropertyAddress addr = makeAddress(kAudioDevicePropertyStreamConfiguration, scope);
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(id, &addr, 0, nullptr, &size) != noErr || size == 0) return 0;
    // AudioBufferList is variable-length; back it with a suitably-aligned byte buffer.
    std::vector<std::byte> raw(size);
    if (AudioObjectGetPropertyData(id, &addr, 0, nullptr, &size, raw.data()) != noErr) return 0;
    const auto* abl = reinterpret_cast<const AudioBufferList*>(raw.data());
    UInt32 total = 0;
    for (UInt32 i = 0; i < abl->mNumberBuffers; ++i) total += abl->mBuffers[i].mNumberChannels;
    return total;
}

std::string formatRates(const std::vector<AudioValueRange>& ranges) {
    if (ranges.empty()) return "?";
    std::string out;
    char buf[64];
    for (size_t i = 0; i < ranges.size(); ++i) {
        if (i) out += ", ";
        const auto& r = ranges[i];
        if (r.mMinimum == r.mMaximum) std::snprintf(buf, sizeof buf, "%g", r.mMinimum);
        else                          std::snprintf(buf, sizeof buf, "%g-%g", r.mMinimum, r.mMaximum);
        out += buf;
    }
    return out;
}
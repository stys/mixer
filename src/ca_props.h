// Thin wrappers over the CoreAudio AudioObject property API — the read-side
// helpers used during device discovery and setup.
#pragma once

#include <CoreAudio/CoreAudio.h>

#include <optional>
#include <string>
#include <vector>

inline AudioObjectPropertyAddress makeAddress(
    AudioObjectPropertySelector selector,
    AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal,
    AudioObjectPropertyElement element = kAudioObjectPropertyElementMain) {
    return AudioObjectPropertyAddress{selector, scope, element};
}

std::optional<std::string> getString(AudioObjectID id, AudioObjectPropertySelector selector);
std::optional<double> getDouble(AudioObjectID id, AudioObjectPropertySelector selector);
std::vector<AudioValueRange> getRanges(AudioObjectID id, AudioObjectPropertySelector selector);
UInt32 channelCount(AudioObjectID id, AudioObjectPropertyScope scope);
std::string formatRates(const std::vector<AudioValueRange>& ranges);
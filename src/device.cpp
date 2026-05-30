#include "device.h"

#include "ca_error.h"
#include "ca_props.h"

#include <cstdio>
#include <stdexcept>

std::vector<AudioObjectID> listAllDevices() {
    AudioObjectPropertyAddress addr = makeAddress(kAudioHardwarePropertyDevices);
    UInt32 size = 0;
    check(AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0, nullptr, &size),
          "AudioObjectGetPropertyDataSize(Devices)");
    std::vector<AudioObjectID> ids(size / sizeof(AudioObjectID));
    check(AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &size, ids.data()),
          "AudioObjectGetPropertyData(Devices)");
    return ids;
}

DeviceInfo findDevice(const std::string& name) {
    for (AudioObjectID id : listAllDevices()) {
        auto n = getString(id, kAudioObjectPropertyName);
        if (!n || *n != name) continue;
        auto uid = getString(id, kAudioDevicePropertyDeviceUID);
        if (!uid) continue;
        return DeviceInfo{id, *uid,
                          channelCount(id, kAudioObjectPropertyScopeInput),
                          channelCount(id, kAudioObjectPropertyScopeOutput)};
    }
    throw std::runtime_error("device not found: \"" + name + "\"");
}

void printDeviceList() {
    std::printf("available devices:\n");
    for (AudioObjectID id : listAllDevices()) {
        const std::string name = getString(id, kAudioObjectPropertyName).value_or("?");
        const std::string uid = getString(id, kAudioDevicePropertyDeviceUID).value_or("?");
        const UInt32 inCh = channelCount(id, kAudioObjectPropertyScopeInput);
        const UInt32 outCh = channelCount(id, kAudioObjectPropertyScopeOutput);
        const auto rate = getDouble(id, kAudioDevicePropertyNominalSampleRate);
        char cur[32] = "?";
        if (rate) std::snprintf(cur, sizeof cur, "%g", *rate);
        const std::string supported =
            formatRates(getRanges(id, kAudioDevicePropertyAvailableNominalSampleRates));
        std::printf("  - %s   [uid: %s]\n", name.c_str(), uid.c_str());
        std::printf("      channels: %u in / %u out\n", inCh, outCh);
        std::printf("      sample rate: %s Hz   supported: %s\n", cur, supported.c_str());
    }
}
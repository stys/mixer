// Audio device discovery: enumerate Core Audio devices, resolve one by name,
// and print the `--list` report.
#pragma once

#include <CoreAudio/CoreAudio.h>

#include <string>
#include <vector>

struct DeviceInfo {
    AudioObjectID id = 0;
    std::string uid;
    UInt32 inputChannels = 0;
    UInt32 outputChannels = 0;
};

std::vector<AudioObjectID> listAllDevices();

// Resolve a device by its Core Audio name. Throws std::runtime_error if absent.
DeviceInfo findDevice(const std::string& name);

void printDeviceList();
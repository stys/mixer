#include "aggregate.h"

#include "ca_error.h"
#include "ca_props.h"

#include <CoreFoundation/CoreFoundation.h>

#include <cmath>
#include <cstdio>

namespace {

// Create a CFString from a std::string. Caller owns the result (CFRelease it).
CFStringRef cfStr(const std::string& s) {
    return CFStringCreateWithCString(kCFAllocatorDefault, s.c_str(), kCFStringEncodingUTF8);
}

// Create a CFNumber holding an int. Caller owns the result (CFRelease it).
CFNumberRef cfInt(int v) {
    return CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &v);
}

// Build one sub-device entry: { UID: <uid>, DriftCompensation: <drift> }.
// Caller owns the returned dictionary (CFRelease it).
CFDictionaryRef makeSubDevice(const std::string& uid, int drift) {
    CFMutableDictionaryRef d = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFStringRef u = cfStr(uid);
    CFNumberRef dc = cfInt(drift);
    CFDictionarySetValue(d, CFSTR(kAudioSubDeviceUIDKey), u);
    CFDictionarySetValue(d, CFSTR(kAudioSubDeviceDriftCompensationKey), dc);
    CFRelease(u);
    CFRelease(dc);
    return d;
}

}  // namespace

AggregateDevice::AggregateDevice(const std::vector<std::string>& subDeviceUIDs,
                                 const std::string& mainUID, double sampleRate) {
    // Unique private UID: "mixer.<uuid>".
    CFUUIDRef uuid = CFUUIDCreate(kCFAllocatorDefault);
    CFStringRef uuidStr = CFUUIDCreateString(kCFAllocatorDefault, uuid);
    char uuidBuf[64] = {0};
    CFStringGetCString(uuidStr, uuidBuf, sizeof uuidBuf, kCFStringEncodingUTF8);
    CFRelease(uuidStr);
    CFRelease(uuid);
    const std::string aggUID = std::string("mixer.") + uuidBuf;

    CFMutableDictionaryRef desc = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 6, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

    CFStringRef aggUIDcf = cfStr(aggUID);
    CFStringRef mainSub = cfStr(mainUID);
    CFNumberRef one = cfInt(1);
    CFNumberRef zero = cfInt(0);

    // CFSTR(...) constants are not owned, so they are not released below.
    CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceNameKey), CFSTR("mixer"));
    CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceUIDKey), aggUIDcf);
    CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceIsPrivateKey), one);
    CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceIsStackedKey), zero);
    CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceMainSubDeviceKey), mainSub);

    // Sub-device list, in the given order. Main sub-device gets drift comp off (0),
    // everything else on (1). The channel concatenation order = this list order.
    std::vector<CFDictionaryRef> subDicts;
    subDicts.reserve(subDeviceUIDs.size());
    for (const auto& uid : subDeviceUIDs) {
        subDicts.push_back(makeSubDevice(uid, uid == mainUID ? 0 : 1));
    }
    CFArrayRef subList = CFArrayCreate(
        kCFAllocatorDefault, reinterpret_cast<const void**>(subDicts.data()),
        static_cast<CFIndex>(subDicts.size()), &kCFTypeArrayCallBacks);
    CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceSubDeviceListKey), subList);

    OSStatus st = AudioHardwareCreateAggregateDevice(desc, &id_);

    CFRelease(subList);
    for (CFDictionaryRef d : subDicts) CFRelease(d);
    CFRelease(zero);
    CFRelease(one);
    CFRelease(mainSub);
    CFRelease(aggUIDcf);
    CFRelease(desc);

    check(st, "AudioHardwareCreateAggregateDevice");

    // Try to set the nominal sample rate. Aggregates with rate-incompatible
    // sub-devices may reject an explicit set, but the master sub-device may
    // already have brought us to the right rate — warn rather than fail.
    double sr = sampleRate;
    AudioObjectPropertyAddress srAddr = makeAddress(kAudioDevicePropertyNominalSampleRate);
    OSStatus srStatus = AudioObjectSetPropertyData(
        id_, &srAddr, 0, nullptr, sizeof(double), &sr);
    if (srStatus != noErr) {
        double actual = 0;
        UInt32 sz = sizeof(double);
        if (AudioObjectGetPropertyData(id_, &srAddr, 0, nullptr, &sz, &actual) == noErr) {
            if (std::fabs(actual - sampleRate) > 0.5) {
                std::fprintf(stderr, "warning: aggregate rate is %g Hz, requested %g Hz\n",
                             actual, sampleRate);
            }
        } else {
            std::fprintf(stderr, "warning: could not set or query aggregate sample rate (%s)\n",
                         fourCC(srStatus).c_str());
        }
    }
}

AggregateDevice::~AggregateDevice() {
    if (id_ != 0) {
        OSStatus st = AudioHardwareDestroyAggregateDevice(id_);
        if (st != noErr) {
            std::fprintf(stderr, "warning: failed to destroy aggregate device (%s)\n",
                         fourCC(st).c_str());
        }
        id_ = 0;
    }
}
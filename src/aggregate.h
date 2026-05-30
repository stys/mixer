// Private aggregate device: fuses sub-devices into one clock-synced virtual
// device a single AudioUnit can read and write across. Created and destroyed
// via RAII, so it never lingers after the process exits.
#pragma once

#include <CoreAudio/CoreAudio.h>

#include <string>
#include <vector>

class AggregateDevice {
public:
    // Build a private aggregate from `subDeviceUIDs` in this exact order (the
    // aggregate concatenates channels in sub-device order, which the plan relies
    // on). `mainUID` is the clock-master sub-device (drift comp off); the rest are
    // drift-compensated. Throws std::runtime_error on failure.
    AggregateDevice(const std::vector<std::string>& subDeviceUIDs,
                    const std::string& mainUID, double sampleRate);
    ~AggregateDevice();

    AggregateDevice(const AggregateDevice&) = delete;
    AggregateDevice& operator=(const AggregateDevice&) = delete;

    AudioObjectID id() const { return id_; }

private:
    AudioObjectID id_ = 0;
};
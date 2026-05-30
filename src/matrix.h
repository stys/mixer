// Matrix planner: turns a validated Config + per-device channel counts into a
// MatrixPlan (sub-device order, AUHAL channel maps, lane layout, and per-send /
// per-output copy ops). Pure function (no CoreAudio calls), so it is snapshot-testable.
#pragma once

#include "config.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// Channel counts for one device (what the planner needs to know about hardware).
struct DevChannels {
    std::string uid;
    uint32_t in = 0;   // input channels (capture side, into the Mac)
    uint32_t out = 0;  // output channels (playback side, out of the Mac)
};

// A device whose inputs we capture; its channels occupy a slice of the scratch.
struct CapturedSource {
    std::string key;
    uint32_t scratchOffset;
    uint32_t channels;
};

struct BusLane {
    std::string name;
    uint32_t offset;     // into the bus-buffer pool
    uint32_t channels;
};

// Add `channels` channels of scratch[srcScratchOffset..] * gain into bus[busOffset..].
struct SendOp {
    uint32_t srcScratchOffset;
    uint32_t busOffset;
    uint32_t channels;
    float gain;
};

// A destination device; its outputs occupy a slice of the internal-output pool.
struct DestDevice {
    std::string key;
    uint32_t internalOutOffset;
    uint32_t channels;
};

// Add `channels` channels of bus[busOffset..] into internalOut[destInternalOutOffset..].
struct OutputOp {
    uint32_t busOffset;
    uint32_t destInternalOutOffset;
    uint32_t channels;
};

struct MatrixPlan {
    std::string mainUID;                     // aggregate clock-master sub-device
    std::vector<std::string> subDeviceUIDs;  // aggregate sub-device order
    uint32_t totalAggInputChannels = 0;
    uint32_t totalAggOutputChannels = 0;

    std::vector<CapturedSource> sources;
    uint32_t totalCapturedChannels = 0;      // AUHAL input-bus channel count
    std::vector<int32_t> inputChannelMap;    // AUHAL bus 1, output scope

    std::vector<BusLane> busLanes;           // aligned with config.buses order
    uint32_t totalBusChannels = 0;
    std::vector<SendOp> sends;               // aligned with config.sends order

    std::vector<DestDevice> dests;
    uint32_t totalInternalOutChannels = 0;   // AUHAL output-bus channel count
    std::vector<int32_t> outputChannelMap;   // AUHAL bus 0, output scope
    std::vector<OutputOp> outputs;           // aligned with config.outputs order
};

// Build the plan. `devs` must contain channel info for every referenced device.
// Throws std::runtime_error if a referenced device is missing from `devs`.
MatrixPlan planMatrix(const Config& c, const std::map<std::string, DevChannels>& devs);

// Deterministic human-readable dump (for `mixer --plan` and snapshot tests).
std::string describePlan(const MatrixPlan& p);

// Load synthetic device channel counts from a JSON map {key: {uid, in, out}}.
// Used by `--plan --devs <file>` for hardware-free planning/testing.
std::map<std::string, DevChannels> loadDevChannels(const std::string& path);
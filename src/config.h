// Routing-matrix configuration: the devices/buses/sends/outputs schema.
//
// Model: SOURCES (device inputs) --send--> BUSES (summing nodes) --out--> DESTINATIONS
// (device outputs). A `send` carries a gain; there is no bus->bus chaining.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

struct BusConfig {
    std::string name;
    uint32_t channels = 2;
};

// A source -> bus edge. `from` is a device key (its input/capture side).
struct SendConfig {
    std::string from;
    std::string to;       // bus name
    float gain = 1.0f;
};

// A bus -> destination edge. `device` is a device key (its output/playback side).
// `deviceChannel` (0-based) is the first physical output channel to write to, so a
// bus can target a sub-range of a multi-output device.
struct OutputConfig {
    std::string bus;
    std::string device;
    uint32_t deviceChannel = 0;
};

struct RecordingConfig {
    bool enabled = false;
    std::optional<std::string> directory;
    std::vector<std::string> record;  // any of {"sources", "buses"}
};

struct Config {
    double sampleRate = 48000.0;
    uint32_t bufferFrames = 64;
    std::map<std::string, std::string> devices;  // key -> Core Audio device name
    std::vector<BusConfig> buses;
    std::vector<SendConfig> sends;
    std::vector<OutputConfig> outputs;
    RecordingConfig recording;
    bool meters = true;  // live terminal level meters; on by default ("meters": false or --no-meters to disable)
};

// Parse + structurally validate a config file. Throws std::runtime_error with a
// human-readable message on any parse/validation failure (including a direct
// device self-loop). Emits a warning to stderr for cross-device feedback cycles.
// Does NOT resolve device names against live hardware — that happens at run time.
Config loadConfig(const std::string& path);

// Human-readable one-line-per-section summary, for `mixer --check`.
std::string describeConfig(const Config& c);

#include "config.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>
#include <stdexcept>

using json = nlohmann::json;

namespace {

// Find a feedback cycle in the device-level routing graph (edges src->dst where a
// source feeds a bus that outputs to dst). Returns the cycle as a node path, or
// nullopt. Direct self-loops are handled separately (as a hard error) before this.
std::optional<std::vector<std::string>> findCycle(
    const std::map<std::string, std::set<std::string>>& g) {
    enum Color { White, Gray, Black };
    std::map<std::string, Color> color;
    std::vector<std::string> path;
    std::optional<std::vector<std::string>> found;

    std::function<bool(const std::string&)> dfs = [&](const std::string& u) -> bool {
        color[u] = Gray;
        path.push_back(u);
        if (auto it = g.find(u); it != g.end()) {
            for (const auto& v : it->second) {
                if (color[v] == Gray) {  // back edge -> cycle
                    auto start = std::find(path.begin(), path.end(), v);
                    std::vector<std::string> cyc(start, path.end());
                    cyc.push_back(v);
                    found = cyc;
                    return true;
                }
                if (color[v] == White && dfs(v)) return true;
            }
        }
        color[u] = Black;
        path.pop_back();
        return false;
    };

    for (const auto& [u, _] : g) {
        if (color[u] == White && dfs(u)) break;
    }
    return found;
}

void validate(const Config& c) {
    if (c.devices.empty()) {
        throw std::runtime_error("config: 'devices' must declare at least one device");
    }

    // Bus names: unique, non-empty, channels >= 1.
    std::set<std::string> busNames;
    for (const auto& b : c.buses) {
        if (b.name.empty()) throw std::runtime_error("config: a bus has an empty name");
        if (b.channels == 0) throw std::runtime_error("config: bus '" + b.name + "' has 0 channels");
        if (!busNames.insert(b.name).second) {
            throw std::runtime_error("config: duplicate bus name '" + b.name + "'");
        }
    }

    auto isDevice = [&](const std::string& k) { return c.devices.count(k) > 0; };
    auto isBus = [&](const std::string& n) { return busNames.count(n) > 0; };

    for (const auto& s : c.sends) {
        if (!isDevice(s.from)) {
            throw std::runtime_error("config: send.from '" + s.from + "' is not a device key");
        }
        if (!isBus(s.to)) {
            throw std::runtime_error("config: send.to '" + s.to + "' is not a bus name");
        }
        if (!std::isfinite(s.gain) || s.gain < 0.0f) {
            throw std::runtime_error("config: send " + s.from + "->" + s.to +
                                     " has invalid gain (must be finite and >= 0)");
        }
    }

    for (const auto& o : c.outputs) {
        if (!isBus(o.bus)) {
            throw std::runtime_error("config: output.bus '" + o.bus + "' is not a bus name");
        }
        if (!isDevice(o.device)) {
            throw std::runtime_error("config: output.device '" + o.device + "' is not a device key");
        }
    }

    for (const auto& r : c.recording.record) {
        if (r != "sources" && r != "buses") {
            throw std::runtime_error("config: recording.record entry '" + r +
                                     "' must be \"sources\" or \"buses\"");
        }
    }

    // Feedback analysis. Build device-level edges src->dst for each
    // (send into bus B) x (output of bus B to device D).
    std::map<std::string, std::set<std::string>> g;
    for (const auto& o : c.outputs) {
        for (const auto& s : c.sends) {
            if (s.to != o.bus) continue;
            if (s.from == o.device) {  // direct self-loop: device feeds a bus routed back to itself
                throw std::runtime_error(
                    "config: feedback self-loop — device '" + o.device + "' feeds bus '" +
                    o.bus + "' which routes straight back to it");
            }
            g[s.from].insert(o.device);
        }
    }
    if (auto cyc = findCycle(g)) {
        std::string path;
        for (size_t i = 0; i < cyc->size(); ++i) {
            if (i) path += " -> ";
            path += (*cyc)[i];
        }
        std::fprintf(stderr,
                     "warning: potential feedback loop across devices: %s\n"
                     "  (may be intentional for cross-monitoring, but can howl if a device "
                     "re-emits its input — verify on the hardware)\n",
                     path.c_str());
    }
}

}  // namespace

Config loadConfig(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("config file not found: " + path);

    json j;
    try {
        in >> j;
    } catch (const json::parse_error& e) {
        throw std::runtime_error("invalid JSON in " + path + ": " + e.what());
    }

    Config c;
    try {
        c.sampleRate = j.value("sample_rate", 48000.0);
        c.bufferFrames = j.value("buffer_frames", static_cast<uint32_t>(64));

        if (!j.contains("devices") || !j["devices"].is_object()) {
            throw std::runtime_error("config: 'devices' object is required");
        }
        for (const auto& [key, val] : j["devices"].items()) {
            if (!val.is_string()) {
                throw std::runtime_error("config: devices['" + key + "'] must be a string");
            }
            c.devices[key] = val.get<std::string>();
        }

        for (const auto& b : j.value("buses", json::array())) {
            BusConfig bc;
            bc.name = b.at("name").get<std::string>();
            bc.channels = b.value("channels", static_cast<uint32_t>(2));
            c.buses.push_back(std::move(bc));
        }

        for (const auto& s : j.value("sends", json::array())) {
            SendConfig sc;
            sc.from = s.at("from").get<std::string>();
            sc.to = s.at("to").get<std::string>();
            sc.gain = s.value("gain", 1.0f);
            c.sends.push_back(std::move(sc));
        }

        for (const auto& o : j.value("outputs", json::array())) {
            OutputConfig oc;
            oc.bus = o.at("bus").get<std::string>();
            oc.device = o.at("device").get<std::string>();
            oc.deviceChannel = o.value("device_channel", static_cast<uint32_t>(0));
            c.outputs.push_back(std::move(oc));
        }

        if (j.contains("recording")) {
            const auto& r = j["recording"];
            c.recording.enabled = r.value("enabled", false);
            if (r.contains("directory") && !r["directory"].is_null()) {
                c.recording.directory = r["directory"].get<std::string>();
            }
            if (r.contains("record")) {
                for (const auto& e : r["record"]) c.recording.record.push_back(e.get<std::string>());
            } else {
                c.recording.record = {"sources", "buses"};
            }
        }
    } catch (const json::exception& e) {
        throw std::runtime_error("config: malformed entry in " + path + ": " + e.what());
    }

    validate(c);
    return c;
}

std::string describeConfig(const Config& c) {
    std::ostringstream o;
    o << "config summary:\n";
    o << "  sample rate: " << c.sampleRate << " Hz, buffer: " << c.bufferFrames << " frames\n";

    o << "  devices (" << c.devices.size() << "):\n";
    for (const auto& [key, name] : c.devices) {
        o << "    " << key << " -> \"" << name << "\"\n";
    }

    o << "  buses (" << c.buses.size() << "):\n";
    for (const auto& b : c.buses) {
        o << "    " << b.name << " (" << b.channels << " ch)\n";
    }

    o << "  sends (" << c.sends.size() << "):\n";
    for (const auto& s : c.sends) {
        o << "    " << s.from << " --(gain " << s.gain << ")--> " << s.to << "\n";
    }

    o << "  outputs (" << c.outputs.size() << "):\n";
    for (const auto& out : c.outputs) {
        o << "    " << out.bus << " -> " << out.device;
        if (out.deviceChannel != 0) o << " @ch" << out.deviceChannel;
        o << "\n";
    }

    o << "  recording: " << (c.recording.enabled ? "on" : "off");
    if (c.recording.enabled) {
        o << " [";
        for (size_t i = 0; i < c.recording.record.size(); ++i) {
            if (i) o << ", ";
            o << c.recording.record[i];
        }
        o << "]";
        if (c.recording.directory) o << " dir=" << *c.recording.directory;
    }
    o << "\n";
    return o.str();
}
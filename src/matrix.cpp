#include "matrix.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>

using json = nlohmann::json;

namespace {
uint32_t umin(uint32_t a, uint32_t b) { return a < b ? a : b; }
}  // namespace

MatrixPlan planMatrix(const Config& c, const std::map<std::string, DevChannels>& devs) {
    MatrixPlan p;

    auto chans = [&](const std::string& key) -> const DevChannels& {
        auto it = devs.find(key);
        if (it == devs.end()) {
            throw std::runtime_error("planMatrix: no channel info for device '" + key + "'");
        }
        return it->second;
    };

    // 1. Referenced devices (sources ∪ destinations), sorted for determinism.
    //    These become the aggregate sub-devices; compute their lane offsets.
    std::set<std::string> refset;
    for (const auto& s : c.sends) refset.insert(s.from);
    for (const auto& o : c.outputs) refset.insert(o.device);

    std::map<std::string, uint32_t> inOff, outOff;
    uint32_t inAcc = 0, outAcc = 0;
    std::string mainKey;
    for (const auto& k : refset) {  // std::set iterates in sorted order
        const auto& d = chans(k);
        inOff[k] = inAcc;
        outOff[k] = outAcc;
        inAcc += d.in;
        outAcc += d.out;
        p.subDeviceUIDs.push_back(d.uid);
        if (mainKey.empty() && d.out > 0) mainKey = k;  // prefer a device with outputs as clock master
    }
    if (mainKey.empty() && !refset.empty()) mainKey = *refset.begin();
    p.mainUID = mainKey.empty() ? "" : chans(mainKey).uid;
    p.totalAggInputChannels = inAcc;
    p.totalAggOutputChannels = outAcc;

    // 2. Captured sources (unique send.from), sorted. Each occupies a scratch slice;
    //    its aggregate input lanes go into the input channel map.
    std::set<std::string> srcset;
    for (const auto& s : c.sends) srcset.insert(s.from);
    std::map<std::string, uint32_t> srcOff;
    uint32_t scratchAcc = 0;
    for (const auto& k : srcset) {
        const auto& d = chans(k);
        srcOff[k] = scratchAcc;
        p.sources.push_back({k, scratchAcc, d.in});
        for (uint32_t ci = 0; ci < d.in; ++ci) {
            p.inputChannelMap.push_back(static_cast<int32_t>(inOff[k] + ci));
        }
        scratchAcc += d.in;
    }
    p.totalCapturedChannels = scratchAcc;

    // 3. Bus lanes (config order).
    std::map<std::string, uint32_t> busOff, busCh;
    uint32_t busAcc = 0;
    for (const auto& b : c.buses) {
        busOff[b.name] = busAcc;
        busCh[b.name] = b.channels;
        p.busLanes.push_back({b.name, busAcc, b.channels});
        busAcc += b.channels;
    }
    p.totalBusChannels = busAcc;

    // 4. Sends: source scratch slice * gain -> bus slice (min of the two widths).
    for (const auto& s : c.sends) {
        const uint32_t ch = umin(chans(s.from).in, busCh[s.to]);
        p.sends.push_back({srcOff[s.from], busOff[s.to], ch, s.gain});
    }

    // 5. Destinations (unique output.device), sorted. Each owns a slice of the
    //    internal-output pool (= its device output channels).
    std::set<std::string> dstset;
    for (const auto& o : c.outputs) dstset.insert(o.device);
    std::map<std::string, uint32_t> dstOff;
    uint32_t outIntAcc = 0;
    for (const auto& k : dstset) {
        const auto& d = chans(k);
        dstOff[k] = outIntAcc;
        p.dests.push_back({k, outIntAcc, d.out});
        outIntAcc += d.out;
    }
    p.totalInternalOutChannels = outIntAcc;

    // 6. Output channel map (length = aggregate output lanes), -1 = silence.
    //    Each destination device's aggregate output lanes pull from its internal slice.
    p.outputChannelMap.assign(outAcc, -1);
    for (const auto& [k, off] : dstOff) {
        const auto& d = chans(k);
        for (uint32_t ci = 0; ci < d.out; ++ci) {
            p.outputChannelMap[outOff[k] + ci] = static_cast<int32_t>(off + ci);
        }
    }

    // 7. Output ops: bus slice -> destination internal slice, offset by the output's
    //    deviceChannel. Summed in the render callback, so multiple buses may target
    //    one device at distinct offsets.
    for (const auto& o : c.outputs) {
        const uint32_t devOut = chans(o.device).out;
        if (o.deviceChannel >= devOut) {
            throw std::runtime_error(
                "planMatrix: output bus '" + o.bus + "' targets device '" + o.device +
                "' channel " + std::to_string(o.deviceChannel) + ", but it has only " +
                std::to_string(devOut) + " output channel(s)");
        }
        const uint32_t ch = umin(busCh[o.bus], devOut - o.deviceChannel);
        p.outputs.push_back({busOff[o.bus], dstOff[o.device] + o.deviceChannel, ch});
    }

    return p;
}

std::string describePlan(const MatrixPlan& p) {
    std::ostringstream o;
    auto rng = [](uint32_t off, uint32_t n) {
        std::ostringstream s;
        s << "[" << off << ".." << (off + n) << ")";
        return s.str();
    };
    auto vec = [](const std::vector<int32_t>& v) {
        std::ostringstream s;
        s << "[";
        for (size_t i = 0; i < v.size(); ++i) {
            if (i) s << ", ";
            s << v[i];
        }
        s << "]";
        return s.str();
    };

    o << "matrix plan:\n";
    o << "  main sub-device: " << p.mainUID << "\n";
    o << "  sub-device order (" << p.subDeviceUIDs.size() << "):";
    for (const auto& u : p.subDeviceUIDs) o << " " << u;
    o << "\n";
    o << "  aggregate lanes: " << p.totalAggInputChannels << " in / "
      << p.totalAggOutputChannels << " out\n";

    o << "  captured sources (" << p.sources.size() << "), scratch width "
      << p.totalCapturedChannels << ":\n";
    for (const auto& s : p.sources) {
        o << "    " << s.key << " @scratch" << rng(s.scratchOffset, s.channels) << "\n";
    }
    o << "  input channel map: " << vec(p.inputChannelMap) << "\n";

    o << "  bus lanes (" << p.busLanes.size() << "), total " << p.totalBusChannels << ":\n";
    for (const auto& b : p.busLanes) {
        o << "    " << b.name << " @bus" << rng(b.offset, b.channels) << "\n";
    }

    o << "  sends (" << p.sends.size() << "):\n";
    for (const auto& s : p.sends) {
        o << "    scratch" << rng(s.srcScratchOffset, s.channels) << " --(gain " << s.gain
          << ")--> bus" << rng(s.busOffset, s.channels) << "\n";
    }

    o << "  destinations (" << p.dests.size() << "), internal-out width "
      << p.totalInternalOutChannels << ":\n";
    for (const auto& d : p.dests) {
        o << "    " << d.key << " @out" << rng(d.internalOutOffset, d.channels) << "\n";
    }
    o << "  output channel map: " << vec(p.outputChannelMap) << "\n";

    o << "  outputs (" << p.outputs.size() << "):\n";
    for (const auto& out : p.outputs) {
        o << "    bus" << rng(out.busOffset, out.channels) << " --> out"
          << rng(out.destInternalOutOffset, out.channels) << "\n";
    }
    return o.str();
}

std::map<std::string, DevChannels> loadDevChannels(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("devs file not found: " + path);
    json j;
    try {
        in >> j;
    } catch (const json::parse_error& e) {
        throw std::runtime_error("invalid JSON in " + path + ": " + e.what());
    }
    std::map<std::string, DevChannels> m;
    for (const auto& [k, v] : j.items()) {
        DevChannels d;
        d.uid = v.at("uid").get<std::string>();
        d.in = v.at("in").get<uint32_t>();
        d.out = v.at("out").get<uint32_t>();
        m[k] = std::move(d);
    }
    return m;
}
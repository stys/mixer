// mixer — low-latency Core Audio routing/mixing/recording CLI.

#include "aggregate.h"
#include "config.h"
#include "device.h"
#include "matrix.h"
#include "recorder.h"
#include "router.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

static volatile std::sig_atomic_t g_keepRunning = 1;
static void onSignal(int) { g_keepRunning = 0; }

static void usage() {
    std::printf(
        "mixer — low-latency Core Audio router/mixer\n\n"
        "  mixer <config.json>     route/mix audio per config\n"
        "      [--seconds <n>]     auto-stop after n seconds (default: run until Ctrl-C)\n"
        "  mixer --check <cfg>     parse + validate a config, print the plan, exit\n"
        "  mixer --plan <cfg>      print the computed channel-lane matrix plan, exit\n"
        "      [--devs <json>]     ...using synthetic device channel counts (no hardware)\n"
        "  mixer --list            list available devices and exit\n"
        "  mixer --help            show this help\n");
}

// Return the argument following `flag`, or nullptr if absent/missing.
static const char* valueAfter(const std::vector<std::string>& args, std::string_view flag) {
    for (size_t i = 1; i + 1 < args.size(); ++i) {
        if (args[i] == flag) return args[i + 1].c_str();
    }
    return nullptr;
}

// Resolve every config device against the live hardware (throws if one is absent).
static std::map<std::string, DevChannels> resolveFromHardware(const Config& cfg) {
    std::map<std::string, DevChannels> m;
    for (const auto& [key, name] : cfg.devices) {
        DeviceInfo di = findDevice(name);
        m[key] = DevChannels{di.uid, di.inputChannels, di.outputChannels};
    }
    return m;
}

static std::string expandTilde(const std::string& p) {
    if (!p.empty() && p[0] == '~') {
        if (const char* home = std::getenv("HOME")) return std::string(home) + p.substr(1);
    }
    return p;
}

static bool listHas(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

// Build a "yyyy-MM-ddTHH-mm-ss" timestamp for the session sub-directory.
static std::string timestamp() {
    std::time_t now = std::time(nullptr);
    std::tm tmv{};
    localtime_r(&now, &tmv);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H-%M-%S", &tmv);
    return buf;
}

static int run(const std::string& configPath, double seconds) {
    Config cfg = loadConfig(configPath);

    std::printf("mixer: resolving devices...\n");
    auto devs = resolveFromHardware(cfg);
    MatrixPlan plan = planMatrix(cfg, devs);
    std::printf("%s", describePlan(plan).c_str());

    std::printf("mixer: creating private aggregate (%g Hz, %u frames)...\n",
                cfg.sampleRate, cfg.bufferFrames);
    AggregateDevice agg(plan.subDeviceUIDs, plan.mainUID, cfg.sampleRate);

    Router router;
    router.setup(agg.id(), cfg.sampleRate, cfg.bufferFrames, plan);

    // Recording: one WAV per source (dry stem) and/or per bus (mix), opt-in via config.
    std::vector<std::unique_ptr<Recorder>> recorders;
    if (cfg.recording.enabled) {
        const std::string base =
            expandTilde(cfg.recording.directory.value_or("~/Documents/mixer-sessions"));
        std::filesystem::path sessionDir = std::filesystem::path(base) / timestamp();
        std::filesystem::create_directories(sessionDir);

        auto add = [&](const std::string& name, uint32_t channels, Recorder*& slot) {
            auto rec = std::make_unique<Recorder>(
                (sessionDir / (name + ".wav")).string(), channels, cfg.sampleRate);
            rec->start();
            slot = rec.get();
            recorders.push_back(std::move(rec));
        };

        if (listHas(cfg.recording.record, "sources")) {
            for (size_t i = 0; i < plan.sources.size(); ++i) {
                add(plan.sources[i].key, plan.sources[i].channels, router.sourceRecorders[i]);
            }
        }
        if (listHas(cfg.recording.record, "buses")) {
            for (size_t i = 0; i < plan.busLanes.size(); ++i) {
                add(plan.busLanes[i].name, plan.busLanes[i].channels, router.busRecorders[i]);
            }
        }
        std::printf("mixer: recording -> %s\n", sessionDir.string().c_str());
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    router.start();

    const double bufMs = static_cast<double>(cfg.bufferFrames) / cfg.sampleRate * 1000.0;
    if (seconds > 0) {
        std::printf("mixer: running — buffer %.2f ms one-way. Auto-stop in %gs.\n", bufMs, seconds);
    } else {
        std::printf("mixer: running — buffer %.2f ms one-way. Ctrl-C to stop.\n", bufMs);
    }
    int ticks = 0;
    while (g_keepRunning) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (seconds > 0 && (++ticks) * 0.1 >= seconds) break;
    }

    std::printf("\nmixer: shutting down...\n");
    router.stop();                 // stop audio first — no more recorder.append() calls
    for (auto& r : recorders) r->stop();  // then flush + close files
    std::printf("mixer: clean exit.\n");
    return 0;  // AggregateDevice destroyed (and torn down) here via RAII
}

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv, argv + argc);
    const auto has = [&](std::string_view flag) {
        return std::find(args.begin(), args.end(), flag) != args.end();
    };
    try {
        if (has("--help") || has("-h")) { usage(); return 0; }
        if (has("--list")) { printDeviceList(); return 0; }

        if (has("--check")) {
            const char* path = valueAfter(args, "--check");
            if (!path) { std::fprintf(stderr, "error: --check needs a config path\n"); return 2; }
            Config cfg = loadConfig(path);
            std::printf("%s", describeConfig(cfg).c_str());
            return 0;
        }

        if (has("--plan")) {
            const char* path = valueAfter(args, "--plan");
            if (!path) { std::fprintf(stderr, "error: --plan needs a config path\n"); return 2; }
            Config cfg = loadConfig(path);
            std::map<std::string, DevChannels> devs;
            if (const char* devsPath = valueAfter(args, "--devs")) {
                devs = loadDevChannels(devsPath);
            } else {
                devs = resolveFromHardware(cfg);
            }
            std::printf("%s", describePlan(planMatrix(cfg, devs)).c_str());
            return 0;
        }

        if (argc < 2 || args[1].starts_with('-')) { usage(); return 0; }

        double seconds = 0;
        if (const char* s = valueAfter(args, "--seconds")) seconds = std::atof(s);
        return run(args[1], seconds);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
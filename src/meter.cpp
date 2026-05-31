#include "meter.h"

#include <pthread.h>
#include <pthread/qos.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr int kBarWidth = 25;
constexpr float kMinDb = -60.0f;
constexpr auto kFrame = std::chrono::milliseconds(40);
constexpr float kFrameSec = 0.040f;
constexpr float kPeakHoldSec = 1.2f;
constexpr float kClipHoldSec = 2.0f;
// The bar tracks the signal: fast release so it falls with the sound (~90 dB/s,
// empties in ~0.7 s). The peak-hold dot falls slowly after its hold expires.
constexpr float kBarDecayPerFrame = 90.0f * kFrameSec;
constexpr float kHoldDecayPerFrame = 20.0f * kFrameSec;

float linToDb(float x) { return x <= 1e-6f ? kMinDb : 20.0f * std::log10(x); }

// ANSI colour for a bar cell at the given dBFS (green safe / yellow / red hot).
const char* zoneColor(float db) {
    if (db >= -3.0f) return "\033[31m";   // red
    if (db >= -12.0f) return "\033[33m";  // yellow
    return "\033[32m";                    // green
}

}  // namespace

Meter::Meter(std::vector<std::string> labels) : labels_(std::move(labels)) {
    const std::size_t n = labels_.size();
    peak_ = std::make_unique<std::atomic<float>[]>(n);
    clip_ = std::make_unique<std::atomic<uint32_t>[]>(n);
    for (std::size_t i = 0; i < n; ++i) {
        peak_[i].store(0.0f, std::memory_order_relaxed);
        clip_[i].store(0, std::memory_order_relaxed);
    }
}

Meter::~Meter() {
    stop();
}

void Meter::report(std::size_t row, float v) {
    if (row >= labels_.size()) return;
    float prev = peak_[row].load(std::memory_order_relaxed);
    while (v > prev && !peak_[row].compare_exchange_weak(prev, v, std::memory_order_relaxed)) {}
    if (v >= 0.999f) clip_[row].store(1, std::memory_order_relaxed);
}

void Meter::start() {
    if (!isatty(fileno(stdout))) return;  // not a terminal: skip drawing entirely
    running_.store(1, std::memory_order_release);
    thread_ = std::thread(&Meter::drawLoop, this);
}

void Meter::stop() {
    if (running_.exchange(0, std::memory_order_acq_rel)) {
        if (thread_.joinable()) thread_.join();
    }
}

void Meter::drawLoop() {
    pthread_setname_np("mixer-meter");
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);

    const std::size_t n = labels_.size();
    const std::size_t lineCount = n;

    std::size_t labelW = 4;
    for (const auto& l : labels_) labelW = std::max(labelW, l.size());

    std::vector<float> shownDb(n, kMinDb), heldDb(n, kMinDb), heldAge(n, 0.0f), clipAge(n, 1e9f);
    bool first = true;

    while (running_.load(std::memory_order_acquire)) {
        std::string out;
        if (!first) out += "\033[" + std::to_string(lineCount) + "A";  // cursor up to block top

        for (std::size_t i = 0; i < n; ++i) {
            const float cur = linToDb(peak_[i].exchange(0.0f, std::memory_order_relaxed));

            // Bar level: instant attack, fast decaying release (follows the sound).
            shownDb[i] = cur > shownDb[i] ? cur : std::max(cur, shownDb[i] - kBarDecayPerFrame);
            // Peak-hold dot: hold the max for a bit, then let it fall slowly.
            if (cur >= heldDb[i]) { heldDb[i] = cur; heldAge[i] = 0.0f; }
            else if ((heldAge[i] += kFrameSec) > kPeakHoldSec) {
                heldDb[i] = std::max(cur, heldDb[i] - kHoldDecayPerFrame);
            }
            // Clip latch: refresh on a fresh full-scale hit, hold the indicator.
            if (clip_[i].exchange(0, std::memory_order_relaxed)) clipAge[i] = 0.0f;
            else clipAge[i] += kFrameSec;

            const int filled = static_cast<int>(
                std::lround((shownDb[i] - kMinDb) / (-kMinDb) * kBarWidth));
            const int fill = std::clamp(filled, 0, kBarWidth);

            char buf[64];
            std::snprintf(buf, sizeof buf, "\r\033[K  %-*s ", static_cast<int>(labelW),
                          labels_[i].c_str());
            out += buf;
            for (int k = 0; k < kBarWidth; ++k) {
                if (k < fill) {
                    const float cellDb = kMinDb + (k + 0.5f) / kBarWidth * (-kMinDb);
                    out += zoneColor(cellDb);
                    out += "█";  // full block
                    out += "\033[0m";
                } else {
                    out += "░";  // light shade
                }
            }
            std::snprintf(buf, sizeof buf, " %6.1f dB  pk %6.1f", shownDb[i], heldDb[i]);
            out += buf;
            if (clipAge[i] < kClipHoldSec) out += "  \033[1;31mCLIP\033[0m";
            out += "\n";
        }

        std::fputs(out.c_str(), stdout);
        std::fflush(stdout);
        first = false;
        std::this_thread::sleep_for(kFrame);
    }
}
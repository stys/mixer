// Meter: a generic live terminal level-meter widget.
//
// It knows nothing about audio routing — it is just N labelled rows. A producer
// calls report(row, peak) once per block (RT-safe: a single atomic max); a
// low-QoS display thread reads those atomics every ~40 ms, applies peak-hold and
// a latched clip indicator, and redraws an ANSI bar block in place. Drawing is
// skipped when stdout is not a TTY, so piped/CI output stays clean.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

class Meter {
public:
    // One label per row, drawn top-to-bottom with uniform spacing.
    explicit Meter(std::vector<std::string> labels);
    ~Meter();

    Meter(const Meter&) = delete;
    Meter& operator=(const Meter&) = delete;

    // RT-safe: fold one block's peak (max|sample|, linear) into row `row`.
    void report(std::size_t row, float linearPeak);

    void start();  // spawn the display thread (no-op when stdout isn't a TTY)
    void stop();   // stop + join the display thread

private:
    void drawLoop();

    std::vector<std::string> labels_;

    // One slot per row. peak_ = interval max since the display last read it
    // (exchange-reset); clip_ = sticky "hit full scale" flag the display latches.
    std::unique_ptr<std::atomic<float>[]> peak_;
    std::unique_ptr<std::atomic<uint32_t>[]> clip_;

    std::thread thread_;
    std::atomic<uint32_t> running_{0};
};
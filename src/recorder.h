// Single-file 24-bit PCM WAV recorder. The audio thread calls append() to push
// planar Float32 frames into a lock-free SPSC ring; a utility-QoS writer thread
// drains it and writes via ExtAudioFile. append() never allocates, locks, or does I/O.
#pragma once

#include <AudioToolbox/AudioToolbox.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

class Recorder {
public:
    Recorder(std::string path, uint32_t channels, double sampleRate, double bufferSeconds = 4.0);
    ~Recorder();

    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    // Open the file and launch the writer thread. Throws on file-open failure.
    void start();

    // RT-safe. Called from the audio thread. Copies `frameCount` frames of planar
    // Float32 channel data (planar[c][f]) into the interleaved ring. Drops frames
    // silently if the ring is full (overrun count reported on stop()).
    void append(const float* const* planar, uint32_t frameCount);

    // Stop the writer thread, flush remaining audio, and close the file.
    void stop();

private:
    void writerLoop();

    std::string path_;
    uint32_t channels_;
    double sampleRate_;

    float* buffer_ = nullptr;
    size_t capacitySamples_ = 0;  // power of two
    size_t mask_ = 0;

    // SPSC indices in samples, monotonically increasing.
    std::atomic<int64_t> writeIdx_{0};
    std::atomic<int64_t> readIdx_{0};
    std::atomic<int32_t> stopFlag_{0};
    std::atomic<int64_t> droppedFrames_{0};

    std::thread writerThread_;
    ExtAudioFileRef extFile_ = nullptr;
};
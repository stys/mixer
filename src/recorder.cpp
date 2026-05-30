#include "recorder.h"

#include "ca_error.h"

#include <CoreFoundation/CoreFoundation.h>
#include <pthread.h>
#include <pthread/qos.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <utility>

Recorder::Recorder(std::string path, uint32_t channels, double sampleRate, double bufferSeconds)
    : path_(std::move(path)), channels_(channels), sampleRate_(sampleRate) {
    const size_t nominal = static_cast<size_t>(sampleRate * bufferSeconds) * channels;
    size_t cap = 1;
    while (cap < nominal) cap <<= 1;
    capacitySamples_ = cap;
    mask_ = cap - 1;
    buffer_ = new float[cap]();  // zero-initialized
}

Recorder::~Recorder() {
    delete[] buffer_;
}

void Recorder::start() {
    // File format: 24-bit signed integer PCM, interleaved.
    AudioStreamBasicDescription fileFormat = {};
    fileFormat.mSampleRate = sampleRate_;
    fileFormat.mFormatID = kAudioFormatLinearPCM;
    fileFormat.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    fileFormat.mBytesPerPacket = 3 * channels_;
    fileFormat.mFramesPerPacket = 1;
    fileFormat.mBytesPerFrame = 3 * channels_;
    fileFormat.mChannelsPerFrame = channels_;
    fileFormat.mBitsPerChannel = 24;

    CFURLRef url = CFURLCreateFromFileSystemRepresentation(
        kCFAllocatorDefault, reinterpret_cast<const UInt8*>(path_.c_str()),
        static_cast<CFIndex>(path_.size()), false);
    OSStatus st = ExtAudioFileCreateWithURL(
        url, kAudioFileWAVEType, &fileFormat, nullptr,
        kAudioFileFlags_EraseFile, &extFile_);
    if (url) CFRelease(url);
    check(st, ("ExtAudioFileCreateWithURL(" + path_ + ")").c_str());

    // Client format: 32-bit float interleaved (what we feed in from the ring).
    AudioStreamBasicDescription clientFormat = {};
    clientFormat.mSampleRate = sampleRate_;
    clientFormat.mFormatID = kAudioFormatLinearPCM;
    clientFormat.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    clientFormat.mBytesPerPacket = 4 * channels_;
    clientFormat.mFramesPerPacket = 1;
    clientFormat.mBytesPerFrame = 4 * channels_;
    clientFormat.mChannelsPerFrame = channels_;
    clientFormat.mBitsPerChannel = 32;
    check(ExtAudioFileSetProperty(extFile_, kExtAudioFileProperty_ClientDataFormat,
                                  sizeof(clientFormat), &clientFormat),
          "ExtAudioFileSetProperty(ClientDataFormat)");

    writerThread_ = std::thread(&Recorder::writerLoop, this);
}

void Recorder::append(const float* const* planar, uint32_t frameCount) {
    const size_t chs = channels_;
    const int64_t totalSamples = static_cast<int64_t>(frameCount) * static_cast<int64_t>(chs);
    const int64_t r = readIdx_.load(std::memory_order_acquire);
    const int64_t w = writeIdx_.load(std::memory_order_relaxed);
    const int64_t avail = static_cast<int64_t>(capacitySamples_) - (w - r);
    if (avail < totalSamples) {
        droppedFrames_.fetch_add(frameCount, std::memory_order_relaxed);
        return;
    }

    const size_t base = static_cast<size_t>(w);
    const size_t m = mask_;
    for (uint32_t f = 0; f < frameCount; ++f) {
        const size_t row = base + static_cast<size_t>(f) * chs;
        for (size_t c = 0; c < chs; ++c) {
            buffer_[(row + c) & m] = planar[c][f];
        }
    }
    writeIdx_.store(w + totalSamples, std::memory_order_release);
}

void Recorder::writerLoop() {
    pthread_setname_np("mixer-recorder");
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);

    constexpr size_t scratchCap = 4096;
    auto scratch = std::make_unique<float[]>(scratchCap);
    const size_t chs = channels_;
    AudioBufferList abl;
    abl.mNumberBuffers = 1;

    while (true) {
        const int64_t w = writeIdx_.load(std::memory_order_acquire);
        const int64_t r = readIdx_.load(std::memory_order_relaxed);
        const int64_t avail = w - r;
        const bool stopping = stopFlag_.load(std::memory_order_relaxed) != 0;

        if (avail < static_cast<int64_t>(chs)) {
            if (stopping) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        const size_t take = std::min<size_t>(static_cast<size_t>(avail), scratchCap);
        const size_t frames = take / chs;
        const size_t nSamples = frames * chs;

        const size_t base = static_cast<size_t>(r);
        const size_t m = mask_;
        for (size_t i = 0; i < nSamples; ++i) {
            scratch[i] = buffer_[(base + i) & m];
        }

        abl.mBuffers[0].mNumberChannels = channels_;
        abl.mBuffers[0].mDataByteSize = static_cast<UInt32>(nSamples * sizeof(float));
        abl.mBuffers[0].mData = scratch.get();

        OSStatus st = ExtAudioFileWrite(extFile_, static_cast<UInt32>(frames), &abl);
        if (st != noErr) {
            std::fprintf(stderr, "warning: ExtAudioFileWrite failed (%s)\n", fourCC(st).c_str());
        }
        readIdx_.store(r + static_cast<int64_t>(nSamples), std::memory_order_release);
    }

    if (extFile_) {
        ExtAudioFileDispose(extFile_);
        extFile_ = nullptr;
    }
}

void Recorder::stop() {
    stopFlag_.store(1, std::memory_order_release);
    if (writerThread_.joinable()) {
        writerThread_.join();
    }
    const int64_t dropped = droppedFrames_.load(std::memory_order_relaxed);
    if (dropped > 0) {
        std::fprintf(stderr, "warning: recorder %s dropped %lld frames (ring overrun)\n",
                     path_.c_str(), static_cast<long long>(dropped));
    }
}
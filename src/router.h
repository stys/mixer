// Router: drives a HALOutput AudioUnit bound to the private aggregate, executing
// a MatrixPlan each render cycle — pull every captured source, sum sends into
// per-bus buffers, optionally tap sources/buses to recorders, then sum buses
// into the device output lanes.
#pragma once

#include "matrix.h"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>

#include <cstddef>
#include <cstdint>
#include <vector>

class Recorder;  // forward-declared; only pointers held here

class Router {
public:
    Router() = default;
    ~Router();

    Router(const Router&) = delete;
    Router& operator=(const Router&) = delete;

    // Optional taps, indices aligned with plan.sources / plan.busLanes. Sized by
    // setup(); fill in (before start()) to record a given source/bus, else leave null.
    std::vector<Recorder*> sourceRecorders;
    std::vector<Recorder*> busRecorders;

    void setup(AudioObjectID aggregateID, double sampleRate, uint32_t bufferFrames,
               const MatrixPlan& plan);
    void start();
    void stop();

    // Called from the audio thread by the C render callback. Public so the
    // extern "C" trampoline can reach it; do not call directly.
    OSStatus render(AudioUnitRenderActionFlags* ioActionFlags, const AudioTimeStamp* inTimeStamp,
                    UInt32 inNumberFrames, AudioBufferList* ioData);

private:
    AudioUnit unit_ = nullptr;
    uint32_t bufferFrames_ = 64;
    MatrixPlan plan_;

    std::vector<float*> scratch_;   // one per captured channel (totalCapturedChannels)
    std::vector<float*> busBuf_;    // one per bus channel (totalBusChannels)
    std::vector<std::byte> inputAblStorage_;  // preallocated backing for the input-bus ABL
};
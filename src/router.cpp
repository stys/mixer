#include "router.h"

#include "ca_error.h"
#include "ca_props.h"
#include "recorder.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>

// C trampoline: AURenderCallback has C language linkage. Forwards to the Router.
extern "C" OSStatus mixerRenderCallback(void* inRefCon,
                                        AudioUnitRenderActionFlags* ioActionFlags,
                                        const AudioTimeStamp* inTimeStamp,
                                        UInt32 /*inBusNumber*/,
                                        UInt32 inNumberFrames,
                                        AudioBufferList* ioData) {
    return static_cast<Router*>(inRefCon)->render(ioActionFlags, inTimeStamp, inNumberFrames, ioData);
}

namespace {

// 32-bit float, non-interleaved, `channels` channels.
AudioStreamBasicDescription floatFormat(double sampleRate, uint32_t channels) {
    AudioStreamBasicDescription fmt = {};
    fmt.mSampleRate = sampleRate;
    fmt.mFormatID = kAudioFormatLinearPCM;
    fmt.mFormatFlags =
        kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonInterleaved;
    fmt.mBytesPerPacket = sizeof(float);
    fmt.mFramesPerPacket = 1;
    fmt.mBytesPerFrame = sizeof(float);
    fmt.mChannelsPerFrame = channels;
    fmt.mBitsPerChannel = 32;
    return fmt;
}

}  // namespace

Router::~Router() {
    stop();
}

void Router::setup(AudioObjectID aggregateID, double sampleRate, uint32_t bufferFrames,
                   const MatrixPlan& plan) {
    plan_ = plan;
    bufferFrames_ = bufferFrames;

    // Set the buffer frame size on the aggregate.
    UInt32 bf = bufferFrames;
    AudioObjectPropertyAddress bfAddr = makeAddress(kAudioDevicePropertyBufferFrameSize);
    OSStatus bfStatus =
        AudioObjectSetPropertyData(aggregateID, &bfAddr, 0, nullptr, sizeof(UInt32), &bf);
    if (bfStatus != noErr) {
        std::fprintf(stderr, "warning: could not set buffer frame size to %u (%s)\n",
                     bufferFrames, fourCC(bfStatus).c_str());
    }

    // Find the HALOutput AudioComponent and instantiate it.
    AudioComponentDescription desc = {};
    desc.componentType = kAudioUnitType_Output;
    desc.componentSubType = kAudioUnitSubType_HALOutput;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;
    AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
    if (!comp) throw std::runtime_error("HALOutput AudioComponent not found");
    check(AudioComponentInstanceNew(comp, &unit_), "AudioComponentInstanceNew");

    const bool haveInput = plan.totalCapturedChannels > 0;
    const bool haveOutput = plan.totalInternalOutChannels > 0;

    // Enable input (bus 1) only if we capture anything; output (bus 0) stays on.
    UInt32 enableIn = haveInput ? 1 : 0;
    check(AudioUnitSetProperty(unit_, kAudioOutputUnitProperty_EnableIO,
                               kAudioUnitScope_Input, 1, &enableIn, sizeof(enableIn)),
          "EnableIO(input)");
    UInt32 enableOut = 1;
    check(AudioUnitSetProperty(unit_, kAudioOutputUnitProperty_EnableIO,
                               kAudioUnitScope_Output, 0, &enableOut, sizeof(enableOut)),
          "EnableIO(output)");

    // Bind the unit to the aggregate device.
    AudioObjectID dev = aggregateID;
    check(AudioUnitSetProperty(unit_, kAudioOutputUnitProperty_CurrentDevice,
                               kAudioUnitScope_Global, 0, &dev, sizeof(dev)), "SetCurrentDevice");

    // Stream formats + channel maps.
    if (haveInput) {
        AudioStreamBasicDescription inFmt = floatFormat(sampleRate, plan.totalCapturedChannels);
        check(AudioUnitSetProperty(unit_, kAudioUnitProperty_StreamFormat,
                                   kAudioUnitScope_Output, 1, &inFmt, sizeof(inFmt)),
              "SetFormat(input bus)");
        check(AudioUnitSetProperty(
                  unit_, kAudioOutputUnitProperty_ChannelMap, kAudioUnitScope_Output, 1,
                  const_cast<int32_t*>(plan.inputChannelMap.data()),
                  static_cast<UInt32>(plan.inputChannelMap.size() * sizeof(int32_t))),
              "SetChannelMap(input)");
    }
    if (haveOutput) {
        AudioStreamBasicDescription outFmt = floatFormat(sampleRate, plan.totalInternalOutChannels);
        check(AudioUnitSetProperty(unit_, kAudioUnitProperty_StreamFormat,
                                   kAudioUnitScope_Input, 0, &outFmt, sizeof(outFmt)),
              "SetFormat(output bus)");
        check(AudioUnitSetProperty(
                  unit_, kAudioOutputUnitProperty_ChannelMap, kAudioUnitScope_Output, 0,
                  const_cast<int32_t*>(plan.outputChannelMap.data()),
                  static_cast<UInt32>(plan.outputChannelMap.size() * sizeof(int32_t))),
              "SetChannelMap(output)");
    }

    // Allocate per-channel scratch (input) and bus buffers.
    scratch_.assign(plan.totalCapturedChannels, nullptr);
    for (auto& p : scratch_) p = new float[bufferFrames];
    busBuf_.assign(plan.totalBusChannels, nullptr);
    for (auto& p : busBuf_) p = new float[bufferFrames];

    inputAblStorage_.resize(
        sizeof(AudioBufferList) +
        (plan.totalCapturedChannels > 0 ? plan.totalCapturedChannels - 1 : 0) * sizeof(AudioBuffer));

    sourceRecorders.assign(plan.sources.size(), nullptr);
    busRecorders.assign(plan.busLanes.size(), nullptr);

    // Install the render callback.
    AURenderCallbackStruct callback = {};
    callback.inputProc = mixerRenderCallback;
    callback.inputProcRefCon = this;
    check(AudioUnitSetProperty(unit_, kAudioUnitProperty_SetRenderCallback,
                               kAudioUnitScope_Input, 0, &callback, sizeof(callback)),
          "SetRenderCallback");

    check(AudioUnitInitialize(unit_), "AudioUnitInitialize");
}

void Router::start() {
    check(AudioOutputUnitStart(unit_), "AudioOutputUnitStart");
}

void Router::stop() {
    if (unit_) {
        AudioOutputUnitStop(unit_);
        AudioUnitUninitialize(unit_);
        AudioComponentInstanceDispose(unit_);
        unit_ = nullptr;
    }
    for (float* p : scratch_) delete[] p;
    for (float* p : busBuf_) delete[] p;
    scratch_.clear();
    busBuf_.clear();
    inputAblStorage_.clear();
}

OSStatus Router::render(AudioUnitRenderActionFlags* ioActionFlags,
                        const AudioTimeStamp* inTimeStamp,
                        UInt32 inNumberFrames, AudioBufferList* ioData) {
    if (!unit_) return noErr;
    const UInt32 bytesPerBuffer = inNumberFrames * static_cast<UInt32>(sizeof(float));

    // 1. Pull every captured source channel into scratch.
    if (!scratch_.empty()) {
        auto* abl = reinterpret_cast<AudioBufferList*>(inputAblStorage_.data());
        abl->mNumberBuffers = static_cast<UInt32>(scratch_.size());
        for (size_t i = 0; i < scratch_.size(); ++i) {
            abl->mBuffers[i].mNumberChannels = 1;
            abl->mBuffers[i].mDataByteSize = bytesPerBuffer;
            abl->mBuffers[i].mData = scratch_[i];
        }
        OSStatus st = AudioUnitRender(unit_, ioActionFlags, inTimeStamp, 1, inNumberFrames, abl);
        if (st != noErr) return st;
    }

    // 2. Tap dry sources.
    for (size_t i = 0; i < plan_.sources.size(); ++i) {
        if (Recorder* r = sourceRecorders[i]) {
            r->append(scratch_.data() + plan_.sources[i].scratchOffset, inNumberFrames);
        }
    }

    // 3. Zero bus buffers, then sum each send (gain * source) into its bus.
    for (float* b : busBuf_) std::memset(b, 0, bytesPerBuffer);
    for (const SendOp& s : plan_.sends) {
        for (uint32_t c = 0; c < s.channels; ++c) {
            const float* src = scratch_[s.srcScratchOffset + c];
            float* dst = busBuf_[s.busOffset + c];
            const float g = s.gain;
            for (UInt32 f = 0; f < inNumberFrames; ++f) dst[f] += g * src[f];
        }
    }

    // 4. Tap bus mixes.
    for (size_t i = 0; i < plan_.busLanes.size(); ++i) {
        if (Recorder* r = busRecorders[i]) {
            r->append(busBuf_.data() + plan_.busLanes[i].offset, inNumberFrames);
        }
    }

    // 5. Zero output buffers, then sum buses into each destination's lanes.
    if (!ioData) return noErr;
    const UInt32 nOut = ioData->mNumberBuffers;
    for (UInt32 i = 0; i < nOut; ++i) {
        if (ioData->mBuffers[i].mData) std::memset(ioData->mBuffers[i].mData, 0, bytesPerBuffer);
    }
    for (const OutputOp& o : plan_.outputs) {
        for (uint32_t c = 0; c < o.channels; ++c) {
            const UInt32 outIdx = o.destInternalOutOffset + c;
            if (outIdx >= nOut) continue;
            auto* dst = static_cast<float*>(ioData->mBuffers[outIdx].mData);
            if (!dst) continue;
            const float* src = busBuf_[o.busOffset + c];
            for (UInt32 f = 0; f < inNumberFrames; ++f) dst[f] += src[f];
        }
    }
    return noErr;
}
// DRUMku processor — the audio component (stereo in, stereo out, MIDI in).
//
// The (ignored) stereo audio input exists only because the host negotiates a
// fixed stereo-in/stereo-out arrangement; DRUMku is an instrument and generates
// its output from MIDI note-on events alone.
//
// Real-time contract: process() never allocates, locks, or does file I/O.
// Sample loading happens on the message thread (IConnectionPoint::notify or
// setState) and is handed to the RT thread by the voice engine's atomic
// pending-swap; retired buffers are freed on the message thread.

#pragma once

#include "drumvoices.h"
#include "public.sdk/source/vst/vstaudioeffect.h"

#include <atomic>
#include <string>
#include <vector>

namespace DRUMku {

//------------------------------------------------------------------------
class DrumProcessor : public Steinberg::Vst::AudioEffect
{
public:
    DrumProcessor();
    ~DrumProcessor() override;

    static Steinberg::FUnknown *createInstance(void *)
    {
        return (Steinberg::Vst::IAudioProcessor *)new DrumProcessor();
    }

    Steinberg::tresult PLUGIN_API initialize(Steinberg::FUnknown *context) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API setBusArrangements(Steinberg::Vst::SpeakerArrangement *inputs,
                                                     Steinberg::int32 numIns,
                                                     Steinberg::Vst::SpeakerArrangement *outputs,
                                                     Steinberg::int32 numOuts) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API canProcessSampleSize(Steinberg::int32 symbolicSampleSize)
        SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API setupProcessing(Steinberg::Vst::ProcessSetup &setup)
        SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API setActive(Steinberg::TBool state) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API process(Steinberg::Vst::ProcessData &data) SMTG_OVERRIDE;

    Steinberg::tresult PLUGIN_API setState(Steinberg::IBStream *state) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API getState(Steinberg::IBStream *state) SMTG_OVERRIDE;

    // Controller -> processor sample-load messages (message thread).
    Steinberg::tresult PLUGIN_API notify(Steinberg::Vst::IMessage *message) SMTG_OVERRIDE;

private:
    void handleParameterChanges(Steinberg::Vst::IParameterChanges *changes);
    void processEvents(Steinberg::Vst::IEventList *events);

    DrumEngine mEngine;

    std::atomic<double> mBypass{0.0};
    // Normalized default matching the controller's "Slots" parameter default, so a
    // state saved without ever moving that control restores kDefaultSlotCount rows
    // (not 1). UI-only hint; setState overwrites it.
    std::atomic<double> mSlotCountNorm{(double)(kDefaultSlotCount - 1) /
                                       (double)(kMaxSlots - 1)};

    // Scratch right channel, used only when the host gives a mono output bus.
    std::vector<float> mScratchR;

    double mSampleRate = 48000.0;
    Steinberg::int32 mMaxBlockSize = 2048;
};

} // namespace DRUMku

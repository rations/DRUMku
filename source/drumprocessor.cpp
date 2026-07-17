// DRUMku processor implementation. See drumprocessor.h for the threading and
// real-time contract.

#include "drumprocessor.h"
#include "drumids.h"

#include "base/source/fstreamer.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"

#include <algorithm>
#include <cmath>
#include <cstring>

// Flush-to-zero / denormals-are-zero, re-armed on every process() call: JACK
// does not set FTZ/DAZ on client process threads.
#if defined(__SSE__) || defined(__x86_64__)
#include <pmmintrin.h>
#include <xmmintrin.h>
#define DRUMKU_HAVE_SSE_DENORMAL 1
#endif

static inline void drumku_set_denormal_mode(void)
{
#ifdef DRUMKU_HAVE_SSE_DENORMAL
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#endif
}

using namespace Steinberg;

namespace DRUMku
{

//------------------------------------------------------------------------
DrumProcessor::DrumProcessor()
{
    setControllerClass(FUID::fromTUID(DrumkuControllerUID));
}

DrumProcessor::~DrumProcessor() = default;

//------------------------------------------------------------------------
tresult PLUGIN_API DrumProcessor::initialize(FUnknown *context)
{
    tresult result = AudioEffect::initialize(context);
    if (result != kResultOk)
        return result;

    // Ignored audio input (the host negotiates stereo in/out); stereo output;
    // an active-by-default event input so the host routes MIDI to us.
    addAudioInput(STR16("Input"), Vst::SpeakerArr::kStereo);
    addAudioOutput(STR16("Output"), Vst::SpeakerArr::kStereo);
    addEventInput(STR16("MIDI In"), 16);
    return kResultOk;
}

//------------------------------------------------------------------------
tresult PLUGIN_API DrumProcessor::setBusArrangements(Vst::SpeakerArrangement *inputs, int32 numIns,
                                                     Vst::SpeakerArrangement *outputs,
                                                     int32 numOuts)
{
    // Accept mono or stereo on each side; the host negotiates stereo/stereo.
    if (numIns != 1 || numOuts != 1)
        return kResultFalse;
    if (inputs[0] != Vst::SpeakerArr::kMono && inputs[0] != Vst::SpeakerArr::kStereo)
        return kResultFalse;
    if (outputs[0] != Vst::SpeakerArr::kMono && outputs[0] != Vst::SpeakerArr::kStereo)
        return kResultFalse;
    return AudioEffect::setBusArrangements(inputs, numIns, outputs, numOuts);
}

//------------------------------------------------------------------------
tresult PLUGIN_API DrumProcessor::canProcessSampleSize(int32 symbolicSampleSize)
{
    return symbolicSampleSize == Vst::kSample32 ? kResultTrue : kResultFalse;
}

//------------------------------------------------------------------------
tresult PLUGIN_API DrumProcessor::setupProcessing(Vst::ProcessSetup &setup)
{
    tresult result = AudioEffect::setupProcessing(setup);
    if (result != kResultOk)
        return result;

    mSampleRate = setup.sampleRate;
    mMaxBlockSize = setup.maxSamplesPerBlock;
    mScratchR.assign((size_t)(mMaxBlockSize > 0 ? mMaxBlockSize : 1), 0.0f);

    // Message thread, processing inactive: safe to (re)resample every loaded
    // sample to the new rate.
    mEngine.setup(mSampleRate, mMaxBlockSize);
    return kResultOk;
}

//------------------------------------------------------------------------
tresult PLUGIN_API DrumProcessor::setActive(TBool state)
{
    return AudioEffect::setActive(state);
}

//------------------------------------------------------------------------
void DrumProcessor::handleParameterChanges(Vst::IParameterChanges *changes)
{
    if (!changes)
        return;
    int32 numParams = changes->getParameterCount();
    for (int32 i = 0; i < numParams; ++i) {
        Vst::IParamValueQueue *queue = changes->getParameterData(i);
        if (!queue)
            continue;
        Vst::ParamValue value;
        int32 sampleOffset;
        int32 numPoints = queue->getPointCount();
        if (numPoints < 1 || queue->getPoint(numPoints - 1, sampleOffset, value) != kResultTrue)
            continue;

        const Vst::ParamID id = queue->getParameterId();
        if (id == kBypassId) {
            mBypass.store(value, std::memory_order_relaxed);
        } else if (id == kSlotCountId) {
            mSlotCountNorm.store(value, std::memory_order_relaxed);
        } else if (id >= (Vst::ParamID)kSlotVolumeBase &&
                   id < (Vst::ParamID)(kSlotVolumeBase + kMaxSlots)) {
            mEngine.setSlotVolume((int)(id - kSlotVolumeBase), (float)value);
        } else if (id >= (Vst::ParamID)kSlotNoteBase &&
                   id < (Vst::ParamID)(kSlotNoteBase + kMaxSlots)) {
            int plain = (int)std::lround(value * (double)kNoteUnassigned);
            mEngine.setSlotNote((int)(id - kSlotNoteBase), plain >= kNoteUnassigned ? -1 : plain);
        }
    }
}

//------------------------------------------------------------------------
// Write one point to an output parameter queue. RT-safe under the SDK host
// implementation: ParameterChanges is pre-sized by the host and every queue
// pre-reserves 5 points at construction — the point-count guard keeps us
// inside that reserve so addPoint never grows the vector on the RT thread.
static void writeOutputPoint(Vst::IParameterChanges *outChanges, Vst::ParamID id,
                             Vst::ParamValue value, int32 sampleOffset)
{
    if (!outChanges)
        return;
    int32 queueIndex = 0;
    Vst::IParamValueQueue *queue = outChanges->addParameterData(id, queueIndex);
    if (!queue || queue->getPointCount() >= 4)
        return;
    int32 pointIndex = 0;
    queue->addPoint(sampleOffset, value, pointIndex);
}

//------------------------------------------------------------------------
void DrumProcessor::processEvents(Vst::IEventList *events, Vst::IParameterChanges *outChanges)
{
    if (!events)
        return;
    int32 count = events->getEventCount();
    for (int32 i = 0; i < count; ++i) {
        Vst::Event e;
        if (events->getEvent(i, e) != kResultTrue)
            continue;
        // Only note-on triggers a sample; drums are one-shots, so note-off is
        // ignored. A note-on with zero velocity is a note-off in disguise.
        if (e.type == Vst::Event::kNoteOnEvent && e.noteOn.velocity > 0.0f) {
            const int pitch = e.noteOn.pitch;

            // MIDI learn: bind the armed slot to this note and report the
            // capture back to the controller as an output parameter change.
            int32 learnSlot = mLearnSlot.exchange(-1, std::memory_order_acq_rel);
            if (learnSlot >= 0 && learnSlot < kMaxSlots && pitch >= 0 && pitch <= 127) {
                mEngine.setSlotNote((int)learnSlot, pitch);
                writeOutputPoint(outChanges, (Vst::ParamID)(kSlotNoteBase + learnSlot),
                                 (double)pitch / (double)kNoteUnassigned, e.sampleOffset);
            } else if (learnSlot >= 0) {
                mLearnSlot.store(learnSlot, std::memory_order_release); // stay armed
            }

            mEngine.noteOn(pitch, e.noteOn.velocity, e.sampleOffset);

            // Pad activity pulse for every slot bound to this pitch, so an
            // editor can flash the pads that just fired.
            for (int s = 0; s < kMaxSlots; ++s) {
                if (mEngine.slotNote(s) == pitch)
                    writeOutputPoint(outChanges, (Vst::ParamID)(kSlotActivityBase + s),
                                     (double)e.noteOn.velocity, e.sampleOffset);
            }
        }
    }
}

//------------------------------------------------------------------------
tresult PLUGIN_API DrumProcessor::process(Vst::ProcessData &data)
{
    drumku_set_denormal_mode();

    handleParameterChanges(data.inputParameterChanges);
    mEngine.applyPendingSwaps();

    if (data.numSamples <= 0 || data.numOutputs < 1 || !data.outputs[0].channelBuffers32)
        return kResultOk;

    const int32 numSamples = std::min(data.numSamples, mMaxBlockSize);
    const int32 numOutCh = data.outputs[0].numChannels;
    float *L = data.outputs[0].channelBuffers32[0];
    float *R = (numOutCh > 1 && data.outputs[0].channelBuffers32[1])
                   ? data.outputs[0].channelBuffers32[1]
                   : mScratchR.data();
    if (!L)
        return kResultOk;

    std::memset(L, 0, (size_t)numSamples * sizeof(float));
    std::memset(R, 0, (size_t)numSamples * sizeof(float));

    if (mBypass.load(std::memory_order_relaxed) > 0.5) {
        mEngine.allNotesOff();
    } else {
        processEvents(data.inputEvents, data.outputParameterChanges);
        mEngine.renderBlock(L, R, numSamples);
    }

    data.outputs[0].silenceFlags = 0;
    return kResultOk;
}

//------------------------------------------------------------------------
tresult PLUGIN_API DrumProcessor::notify(Vst::IMessage *message)
{
    if (!message)
        return kInvalidArgument;

    const char *id = message->getMessageID();
    if (id && strcmp(id, kMsgLoadSample) == 0) {
        int64 slot = -1;
        message->getAttributes()->getInt(kSlotAttr, slot);

        std::string path;
        const void *data = nullptr;
        uint32 size = 0;
        if (message->getAttributes()->getBinary(kPathAttr, data, size) == kResultOk && data &&
            size > 0)
            path.assign(static_cast<const char *>(data), size);

        mEngine.loadSample((int)slot, path); // empty path clears the slot
        return kResultOk;
    }
    if (id && strcmp(id, kMsgArmLearn) == 0) {
        int64 slot = -1;
        message->getAttributes()->getInt(kSlotAttr, slot);
        if (slot < -1 || slot >= kMaxSlots)
            return kInvalidArgument;
        mLearnSlot.store((int32)slot, std::memory_order_release);
        return kResultOk;
    }

    return AudioEffect::notify(message);
}

//------------------------------------------------------------------------
tresult PLUGIN_API DrumProcessor::setState(IBStream *state)
{
    if (!state)
        return kResultFalse;
    IBStreamer streamer(state, kLittleEndian);

    int32 version = 0;
    if (!streamer.readInt32(version) || version < 1 || version > 1)
        return kResultFalse;

    double bypass = 0.0, slotCount = 0.0;
    if (!streamer.readDouble(bypass) || !streamer.readDouble(slotCount))
        return kResultFalse;
    mBypass.store(bypass, std::memory_order_relaxed);
    mSlotCountNorm.store(slotCount, std::memory_order_relaxed);

    for (int i = 0; i < kMaxSlots; ++i) {
        double vol = 0.8;
        int32 note = -1;
        if (!streamer.readDouble(vol) || !streamer.readInt32(note))
            return kResultFalse;
        mEngine.setSlotVolume(i, (float)vol);
        mEngine.setSlotNote(i, note);

        char8 *path = streamer.readStr8();
        if (path) {
            if (path[0])
                mEngine.loadSample(i, path);
            else
                mEngine.clearSample(i);
            delete[] path;
        }
    }
    return kResultOk;
}

//------------------------------------------------------------------------
tresult PLUGIN_API DrumProcessor::getState(IBStream *state)
{
    if (!state)
        return kResultFalse;
    IBStreamer streamer(state, kLittleEndian);

    streamer.writeInt32(1); // state version
    streamer.writeDouble(mBypass.load(std::memory_order_relaxed));
    streamer.writeDouble(mSlotCountNorm.load(std::memory_order_relaxed));

    for (int i = 0; i < kMaxSlots; ++i) {
        streamer.writeDouble((double)mEngine.slotVolume(i));
        streamer.writeInt32((int32)mEngine.slotNote(i));
        streamer.writeStr8(mEngine.slotPath(i).c_str());
    }
    return kResultOk;
}

} // namespace DRUMku

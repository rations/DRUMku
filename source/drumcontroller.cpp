// DRUMku edit controller implementation.

#include "drumcontroller.h"
#include "drumids.h"

#include "base/source/fstreamer.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ustring.h"
#include "pluginterfaces/vst/ivstmessage.h"

#include <cstdio>
#include <cstring>

using namespace Steinberg;

namespace DRUMku {

// The one and only definition of the IDrumLoader interface ID.
DEF_CLASS_IID(IDrumLoader)

//------------------------------------------------------------------------
tresult PLUGIN_API DrumController::initialize(FUnknown *context)
{
    tresult result = EditController::initialize(context);
    if (result != kResultOk)
        return result;

    parameters.addParameter(STR16("Bypass"), nullptr, 1, 0.0,
                            Vst::ParameterInfo::kCanAutomate | Vst::ParameterInfo::kIsBypass,
                            kBypassId);

    // How many rack rows the host shows (1 .. kMaxSlots, default 10). The +Add
    // button raises this; it does not affect the DSP.
    auto *slotCount = new Vst::RangeParameter(STR16("Slots"), kSlotCountId, nullptr, 1.0,
                                              (double)kMaxSlots, (double)kDefaultSlotCount,
                                              kMaxSlots - 1, Vst::ParameterInfo::kCanAutomate);
    parameters.addParameter(slotCount);

    // Per-slot Volume (linear 0..1) and Note (assigned MIDI pitch; the top step
    // means "unassigned"). All slots are declared so the IDs never shift.
    for (int32 i = 0; i < kMaxSlots; ++i) {
        char16 title[64];
        char ascii[64];

        std::snprintf(ascii, sizeof(ascii), "Slot %d Vol", i + 1);
        UString(title, 64).fromAscii(ascii);
        auto *vol = new Vst::RangeParameter(title, (Vst::ParamID)(kSlotVolumeBase + i), nullptr, 0.0,
                                            1.0, 0.8, 0, Vst::ParameterInfo::kCanAutomate);
        vol->setPrecision(2);
        parameters.addParameter(vol);

        std::snprintf(ascii, sizeof(ascii), "Slot %d Note", i + 1);
        UString(title, 64).fromAscii(ascii);
        auto *note = new Vst::RangeParameter(title, (Vst::ParamID)(kSlotNoteBase + i), nullptr, 0.0,
                                             (double)kNoteUnassigned, (double)kNoteUnassigned,
                                             kNoteUnassigned, Vst::ParameterInfo::kCanAutomate);
        note->setPrecision(0);
        parameters.addParameter(note);
    }

    return kResultOk;
}

//------------------------------------------------------------------------
tresult PLUGIN_API DrumController::setComponentState(IBStream *state)
{
    // Mirror of DrumProcessor::getState — keep the two in sync.
    if (!state)
        return kResultFalse;
    IBStreamer streamer(state, kLittleEndian);

    int32 version = 0;
    if (!streamer.readInt32(version) || version < 1 || version > 1)
        return kResultFalse;

    double bypass = 0.0, slotCount = 0.0;
    if (!streamer.readDouble(bypass) || !streamer.readDouble(slotCount))
        return kResultFalse;
    setParamNormalized(kBypassId, bypass);
    setParamNormalized(kSlotCountId, slotCount);

    for (int32 i = 0; i < kMaxSlots; ++i) {
        double vol = 0.8;
        int32 note = -1;
        if (!streamer.readDouble(vol) || !streamer.readInt32(note))
            return kResultFalse;
        setParamNormalized((Vst::ParamID)(kSlotVolumeBase + i), vol);
        double noteNorm = (double)(note < 0 ? kNoteUnassigned : note) / (double)kNoteUnassigned;
        setParamNormalized((Vst::ParamID)(kSlotNoteBase + i), noteNorm);

        char8 *path = streamer.readStr8();
        mSlotPath[i] = path ? path : "";
        if (path)
            delete[] path;
    }
    return kResultOk;
}

//------------------------------------------------------------------------
tresult DrumController::sendSample(int32 slot, const char8 *path)
{
    // Forward the path (with its slot index) to the processor over the
    // connection. When no peer is connected the local copy is still updated.
    IPtr<Vst::IMessage> message = owned(allocateMessage());
    if (!message)
        return kResultFalse;
    message->setMessageID(kMsgLoadSample);
    message->getAttributes()->setInt(kSlotAttr, slot);
    const char *p = path ? path : "";
    message->getAttributes()->setBinary(kPathAttr, p, static_cast<uint32>(strlen(p)));
    return sendMessage(message);
}

//------------------------------------------------------------------------
tresult PLUGIN_API DrumController::setSampleFile(int32 slot, const char8 *path)
{
    if (slot < 0 || slot >= kMaxSlots)
        return kInvalidArgument;
    mSlotPath[slot] = path ? path : "";
    return sendSample(slot, path);
}

tresult PLUGIN_API DrumController::getSampleFile(int32 slot, char8 *buffer, int32 bufferSize)
{
    if (slot < 0 || slot >= kMaxSlots)
        return kInvalidArgument;
    const std::string &src = mSlotPath[slot];
    if (!buffer || bufferSize <= (int32)src.size())
        return kResultFalse;
    memcpy(buffer, src.c_str(), src.size() + 1);
    return kResultOk;
}

} // namespace DRUMku

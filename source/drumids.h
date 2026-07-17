// DRUMku — a native VST3 MIDI drum sampler for Haiku.
//
// A drum rack: each slot loads a .wav sample, has a volume, and is bound to a
// MIDI note. Incoming note-on events trigger the sample(s) bound to that pitch.
// Written against the VST3 SDK only — no plug-in framework.

#pragma once

#include "idrumloader.h" // shared parameter-ID layout + IDrumLoader interface

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/vst/vsttypes.h"

namespace DRUMku
{

// Message IDs for controller -> processor sample loading (IConnectionPoint).
// Attribute "slot" (setInt) selects the slot; attribute "path" (setBinary)
// carries a UTF-8 byte string (empty = clear the slot).
inline constexpr const char *kMsgLoadSample = "DRUMkuLoadSample";
inline constexpr const char *kSlotAttr = "slot";
inline constexpr const char *kPathAttr = "path";

// Controller -> processor MIDI-learn arming (IConnectionPoint). Attribute
// "slot" (setInt) selects the slot to learn; -1 disarms. The processor
// captures the next note-on, binds it to the slot, and reports it back as an
// output parameter change on (kSlotNoteBase + slot).
inline constexpr const char *kMsgArmLearn = "DRUMkuArmLearn";

static DECLARE_UID(DrumkuProcessorUID, 0x4A6B9B74, 0x782537E2, 0xD563E015, 0x745DD2B7);
static DECLARE_UID(DrumkuControllerUID, 0xEEF20674, 0xEBD75618, 0x10163F1D, 0xB7904A0D);

} // namespace DRUMku

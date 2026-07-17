// DRUMku edit controller — parameters plus the IDrumLoader interface that lets
// GUI-less hosts load a .wav sample into each rack slot.

#pragma once

#include "idrumloader.h"
#include "public.sdk/source/vst/vsteditcontroller.h"

#include <string>

namespace DRUMku
{

class DrumEditorView;

//------------------------------------------------------------------------
class DrumController : public Steinberg::Vst::EditController, public IDrumLoader
{
public:
    static Steinberg::FUnknown *createInstance(void *)
    {
        return (Steinberg::Vst::IEditController *)new DrumController();
    }

    Steinberg::tresult PLUGIN_API initialize(Steinberg::FUnknown *context) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API setComponentState(Steinberg::IBStream *state) SMTG_OVERRIDE;

    // Native Haiku editor (IPlugView). The live view is tracked through the
    // EditorView attach hooks; setParamNormalized pushes every value change to
    // it. Both run on the host window's looper thread (host contract), so no
    // locking is needed around mView.
    Steinberg::IPlugView *PLUGIN_API createView(Steinberg::FIDString name) SMTG_OVERRIDE;
    void editorAttached(Steinberg::Vst::EditorView *editor) SMTG_OVERRIDE;
    void editorRemoved(Steinberg::Vst::EditorView *editor) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API
    setParamNormalized(Steinberg::Vst::ParamID tag, Steinberg::Vst::ParamValue value) SMTG_OVERRIDE;

    // Arm plug-in-side MIDI learn for a slot (-1 disarms). Called by the native
    // editor; the captured note comes back as an output parameter change on
    // (kSlotNoteBase + slot) and reaches the view through setParamNormalized.
    Steinberg::tresult armLearn(Steinberg::int32 slot);

    // IDrumLoader
    Steinberg::tresult PLUGIN_API setSampleFile(Steinberg::int32 slot,
                                                const Steinberg::char8 *path) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API getSampleFile(Steinberg::int32 slot, Steinberg::char8 *buffer,
                                                Steinberg::int32 bufferSize) SMTG_OVERRIDE;

    //---Interface---------
    OBJ_METHODS(DrumController, EditController)
    DEFINE_INTERFACES
    DEF_INTERFACE(IDrumLoader)
    END_DEFINE_INTERFACES(EditController)
    REFCOUNT_METHODS(EditController)

private:
    Steinberg::tresult sendSample(Steinberg::int32 slot, const Steinberg::char8 *path);

    std::string mSlotPath[kMaxSlots];
    DrumEditorView *mView = nullptr; // live editor, host window looper thread only
};

} // namespace DRUMku

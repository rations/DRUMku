// DRUMku edit controller — parameters plus the IDrumLoader interface that lets
// GUI-less hosts load a .wav sample into each rack slot.

#pragma once

#include "idrumloader.h"
#include "public.sdk/source/vst/vsteditcontroller.h"

#include <string>

namespace DRUMku {

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
};

} // namespace DRUMku

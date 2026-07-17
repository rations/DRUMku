// DRUMku native Haiku editor (IPlugView / kPlatformTypeHaikuBView).
//
// DrumEditorView is the IPlugView the controller returns from createView();
// the BView it creates lives on the host window's looper thread. The
// controller pushes value changes in through ParamChanged() (same looper
// thread — see DrumController::setParamNormalized); user edits go out through
// EditController::beginEdit/performEdit/endEdit, which reach the host's
// IComponentHandler.

#pragma once

#include "idrumloader.h"

#include "haikuplugview.h"

namespace DRUMku
{

class DrumController;
class DrumKitView;

//------------------------------------------------------------------------
class DrumEditorView : public Steinberg::HaikuPlugView
{
public:
    explicit DrumEditorView(DrumController *controller);

    // Called by the controller on the host window's looper thread whenever a
    // parameter value changes (automation, generic UI, state load, learn).
    void ParamChanged(Steinberg::Vst::ParamID id, Steinberg::Vst::ParamValue value);

protected:
    BView *createHaikuView(BRect frame) SMTG_OVERRIDE;
    void removedFromParent() SMTG_OVERRIDE;

private:
    DrumKitView *mKitView = nullptr; // owned by HaikuPlugView (fView), only cached here
};

} // namespace DRUMku

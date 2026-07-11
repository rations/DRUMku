// DRUMku plug-in factory.

#include "drumcontroller.h"
#include "drumids.h"
#include "drumprocessor.h"
#include "version.h"

#include "public.sdk/source/main/pluginfactory_constexpr.h"

BEGIN_FACTORY_DEF (stringCompanyName, stringCompanyWeb, stringCompanyEmail, 2)

DEF_CLASS (DRUMku::DrumkuProcessorUID, Steinberg::PClassInfo::kManyInstances,
           kVstAudioEffectClass,
           stringPluginName,
           Steinberg::Vst::kDistributable,
           "Instrument|Drum",
           FULL_VERSION_STR,
           kVstVersionString,
           DRUMku::DrumProcessor::createInstance,
           nullptr)

DEF_CLASS (DRUMku::DrumkuControllerUID, Steinberg::PClassInfo::kManyInstances,
           kVstComponentControllerClass,
           stringPluginName "Controller",
           0,
           "",
           FULL_VERSION_STR,
           kVstVersionString,
           DRUMku::DrumController::createInstance,
           nullptr)

END_FACTORY

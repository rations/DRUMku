// PluginHost implementation. See pluginhost.h for the threading contract.

#include "pluginhost.h"

#include "public.sdk/samples/vst-hosting/audiohost/source/media/jdbackend/jdbackendclient.h"
#include "public.sdk/source/common/memorystream.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstmessage.h"

#include <cstdio>

using namespace Steinberg;

namespace DRUMku
{

namespace HostAudio = Vst::HostAudio;

//------------------------------------------------------------------------
// A PlugProvider whose component <-> controller connection works from any
// non-RT thread. The SDK's ConnectionProxy drops messages sent off the thread
// it was created on; on Haiku the editor lives on its window's thread, so the
// proxies would drop every message the editor starts (sample loads). A
// direct IConnectionPoint connection has no thread guard; the plug-in's
// notify() must be message-thread-safe, as the VST3 threading model requires.
// (The same as VST3-renku's vst3host.)
class PluginHost::Provider : public Vst::PlugProvider
{
public:
    using Vst::PlugProvider::PlugProvider;

    bool connectDirect()
    {
        if (!component || !controller)
            return false;
        disconnectComponents(); // drop the thread-affine proxies
        auto compICP = U::cast<Vst::IConnectionPoint>(component);
        auto contrICP = U::cast<Vst::IConnectionPoint>(controller);
        if (!compICP || !contrICP)
            return false;
        return compICP->connect(contrICP) == kResultTrue &&
               contrICP->connect(compICP) == kResultTrue;
    }

    void disconnectDirect()
    {
        auto compICP = U::cast<Vst::IConnectionPoint>(component);
        auto contrICP = U::cast<Vst::IConnectionPoint>(controller);
        if (compICP && contrICP) {
            compICP->disconnect(contrICP);
            contrICP->disconnect(compICP);
        }
    }
};

//------------------------------------------------------------------------
PluginHost::PluginHost() = default;

PluginHost::~PluginHost()
{
    stopAudio();
    if (fController)
        fController->setComponentHandler(nullptr);
    if (fMidiMapping)
        fMidiMapping->release();
    if (fProvider)
        fProvider->disconnectDirect();
    fProvider = nullptr;
    fModule = nullptr;
}

//------------------------------------------------------------------------
bool PluginHost::load(const std::string &bundlePath, std::string &error)
{
    // The host context must be in place before the plug-in is instantiated:
    // the plug-in asks it for the IMessage objects that carry sample paths
    // from the controller to the processor.
    static Vst::HostApplication hostContext;
    Vst::PluginContextFactory::instance().setPluginContext(&hostContext);

    std::string moduleError;
    fModule = VST3::Hosting::Module::create(bundlePath, moduleError);
    if (!fModule) {
        error = "Could not load " + bundlePath + ": " + moduleError;
        return false;
    }
    auto factory = fModule->getFactory();
    for (auto &classInfo : factory.classInfos()) {
        if (classInfo.category() != kVstAudioEffectClass)
            continue;
        fProvider = owned(new Provider(factory, classInfo, true));
        if (fProvider->initialize())
            break;
        fProvider = nullptr;
    }
    if (!fProvider) {
        error = "No audio module in " + bundlePath;
        return false;
    }
    if (!fProvider->connectDirect())
        fprintf(stderr, "DRUMku: direct component/controller connection failed\n");

    // PlugProvider keeps its own references; these are borrowed.
    fComponent = fProvider->getComponentPtr();
    fController = fProvider->getControllerPtr();
    if (!fComponent || !fController) {
        error = "The plug-in did not provide both of its parts";
        return false;
    }
    if (fController->queryInterface(Vst::IMidiMapping::iid, (void **)&fMidiMapping) != kResultOk)
        fMidiMapping = nullptr;
    if (fController->setComponentHandler(this) != kResultOk)
        fprintf(stderr, "DRUMku: the plug-in refused the component handler\n");
    return true;
}

//------------------------------------------------------------------------
bool PluginHost::startAudio(const std::string &backend, std::string &error)
{
    stopAudio();
    fBackend = backend;
    if (!HostAudio::selectBackend(backend, "DRUMku", "DRUMKU_BACKEND_DIR", error))
        return false;
    fAudio = Vst::AudioClient::create("DRUMku", fComponent, fMidiMapping);
    if (!fAudio || !HostAudio::isRunning()) {
        error = HostAudio::lastError();
        if (error.empty())
            error = "The audio client could not start.";
        fAudio = nullptr;
        return false;
    }
    return true;
}

//------------------------------------------------------------------------
void PluginHost::stopAudio()
{
    // Closes the backend (its audio thread stops first), then stops the
    // processor. The MIDI connections go with the backend.
    fAudio = nullptr;
}

//------------------------------------------------------------------------
bool PluginHost::audioRunning() const
{
    return fAudio && HostAudio::isRunning();
}

//------------------------------------------------------------------------
std::vector<std::string> PluginHost::midiSources() const
{
    return fAudio ? HostAudio::midiSources() : std::vector<std::string>();
}

//------------------------------------------------------------------------
std::vector<std::string> PluginHost::connectedMidi() const
{
    return fAudio ? HostAudio::midiInputConnections(0) : std::vector<std::string>();
}

//------------------------------------------------------------------------
bool PluginHost::setMidiSource(const std::string &source, bool on, std::string &error)
{
    if (!fAudio) {
        error = "Audio is not running.";
        return false;
    }
    if (HostAudio::numMidiInputs() < 1) {
        error = "The plug-in has no MIDI input.";
        return false;
    }
    const bool ok =
        on ? HostAudio::connectMidiInput(0, source) : HostAudio::disconnectMidiInput(0, source);
    if (!ok)
        error = std::string("Could not ") + (on ? "connect " : "disconnect ") + source + ".";
    return ok;
}

//------------------------------------------------------------------------
std::string PluginHost::statusLine() const
{
    char line[512];
    if (!audioRunning()) {
        snprintf(line, sizeof line, "No audio (%s): %s", fBackend == "jack" ? "JACK" : "Media Kit",
                 HostAudio::lastError().c_str());
        return line;
    }
    std::string midi;
    for (const auto &source : connectedMidi()) {
        if (!midi.empty())
            midi += ", ";
        // The Media Kit names sources "midi:<endpoint>".
        midi += source.compare(0, 5, "midi:") == 0 ? source.substr(5) : source;
    }
    const uint32_t rate = HostAudio::sampleRate();
    const uint32_t block = HostAudio::blockSize();
    snprintf(line, sizeof line, "%s  %u Hz  %u frames (%.1f ms)  late %d  MIDI: %s",
             HostAudio::backendName().c_str(), rate, block, rate ? 1000.0 * block / rate : 0.0,
             (int)HostAudio::xrunCount(), midi.empty() ? "none (MIDI menu)" : midi.c_str());
    return line;
}

//------------------------------------------------------------------------
bool PluginHost::getState(std::vector<char> &component, std::vector<char> &controller)
{
    MemoryStream componentStream;
    MemoryStream controllerStream;
    if (fComponent->getState(&componentStream) != kResultOk)
        return false;
    component.assign(componentStream.getData(),
                     componentStream.getData() + componentStream.getSize());
    controller.clear();
    if (fController->getState(&controllerStream) == kResultOk)
        controller.assign(controllerStream.getData(),
                          controllerStream.getData() + controllerStream.getSize());
    return true;
}

//------------------------------------------------------------------------
void PluginHost::setState(const std::vector<char> &component, const std::vector<char> &controller)
{
    // The order a DAW uses on project load: the processor takes its state,
    // the controller mirrors it, then takes its own.
    if (!component.empty()) {
        MemoryStream stream(const_cast<char *>(component.data()),
                            static_cast<TSize>(component.size()));
        if (fComponent->setState(&stream) != kResultOk) {
            fprintf(stderr, "DRUMku: the saved plug-in state was refused\n");
            return;
        }
        int64 pos = 0;
        stream.seek(0, IBStream::kIBSeekSet, &pos);
        fController->setComponentState(&stream);
    }
    if (!controller.empty()) {
        MemoryStream stream(const_cast<char *>(controller.data()),
                            static_cast<TSize>(controller.size()));
        fController->setState(&stream);
    }
}

//------------------------------------------------------------------------
Vst::ParameterChangeTransfer *PluginHost::outputTransfer() const
{
    return fAudio ? &fAudio->getOutputParamTransferrer() : nullptr;
}

//------------------------------------------------------------------------
tresult PLUGIN_API PluginHost::performEdit(Vst::ParamID id, Vst::ParamValue value)
{
    if (fAudio)
        fAudio->setParameter(id, value, 0);
    return kResultOk;
}

//------------------------------------------------------------------------
tresult PLUGIN_API PluginHost::restartComponent(int32)
{
    // Parameter titles and values are shown by DRUMku's own editor.
    return kResultOk;
}

//------------------------------------------------------------------------
tresult PLUGIN_API PluginHost::queryInterface(const TUID iid, void **obj)
{
    if (!obj)
        return kInvalidArgument;
    if (FUnknownPrivate::iidEqual(iid, Vst::IComponentHandler::iid) ||
        FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
        *obj = static_cast<Vst::IComponentHandler *>(this);
        return kResultOk;
    }
    *obj = nullptr;
    return kNoInterface;
}

} // namespace DRUMku

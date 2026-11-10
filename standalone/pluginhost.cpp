// PluginHost implementation. See pluginhost.h for the threading contract.

#include "pluginhost.h"

#include "public.sdk/samples/vst-hosting/audiohost/source/media/jdbackend/jdbackendclient.h"
#include "public.sdk/source/common/memorystream.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstmessage.h"

#include <ByteOrder.h>
#include <Entry.h>
#include <File.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

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
        if (fProvider->initialize()) {
            memcpy(fClassID, classInfo.ID().data(), sizeof fClassID);
            break;
        }
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
    // The plug-in's stereo input is there for hosts that insist on one; it is
    // never read, so the app publishes no input ports for it.
    HostAudio::publishAudioInputs(false);
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
bool PluginHost::setState(const std::vector<char> &component, const std::vector<char> &controller)
{
    // The order a DAW uses on project load: the processor takes its state,
    // the controller mirrors it, then takes its own.
    if (!component.empty()) {
        MemoryStream stream(const_cast<char *>(component.data()),
                            static_cast<TSize>(component.size()));
        if (fComponent->setState(&stream) != kResultOk) {
            fprintf(stderr, "DRUMku: the saved plug-in state was refused\n");
            return false;
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
    return true;
}

//------------------------------------------------------------------------
// The .kasenpreset format, as Kasen writes it (pluginhost_preset_save in
// Kasen's src/host/pluginhost.cpp); keep the two the same:
//   [0]   8  magic "JDPRESET"
//   [8]   4  format version, uint32 little-endian: 1
//   [12] 16  the plug-in's VST3 class ID (a preset for another plug-in is
//            refused)
//   [28]  4  component state length C, int32 little-endian
//   [32]  4  controller state length E, int32 little-endian
//   [36]  C  component state, then E bytes of controller state
namespace
{
const char kPresetMagic[8] = {'J', 'D', 'P', 'R', 'E', 'S', 'E', 'T'};
const uint32 kPresetVersion = 1;
const size_t kPresetHeader = 8 + 4 + 16;
const off_t kPresetMaxSize = 64 * 1024 * 1024; // a preset is untrusted input

void PutLE32(std::vector<char> &out, uint32 value)
{
    value = B_HOST_TO_LENDIAN_INT32(value);
    const char *bytes = reinterpret_cast<const char *>(&value);
    out.insert(out.end(), bytes, bytes + 4);
}

uint32 GetLE32(const char *bytes)
{
    uint32 value;
    memcpy(&value, bytes, 4);
    return B_LENDIAN_TO_HOST_INT32(value);
}
} // namespace

//------------------------------------------------------------------------
bool PluginHost::savePreset(const char *path, std::string &error)
{
    std::vector<char> component, controller;
    if (!getState(component, controller)) {
        error = "The plug-in did not hand over its state.";
        return false;
    }
    if (component.size() > INT32_MAX || controller.size() > INT32_MAX) {
        error = "The plug-in's state is too large for a preset.";
        return false;
    }
    std::vector<char> data;
    data.reserve(kPresetHeader + 8 + component.size() + controller.size());
    data.insert(data.end(), kPresetMagic, kPresetMagic + sizeof kPresetMagic);
    PutLE32(data, kPresetVersion);
    data.insert(data.end(), fClassID, fClassID + sizeof fClassID);
    PutLE32(data, static_cast<uint32>(component.size()));
    PutLE32(data, static_cast<uint32>(controller.size()));
    data.insert(data.end(), component.begin(), component.end());
    data.insert(data.end(), controller.begin(), controller.end());

    // Written next to the target and renamed over it, so a failed save never
    // leaves half a preset (or half of an older one).
    const std::string part = std::string(path) + ".part";
    BFile file;
    status_t status = file.SetTo(part.c_str(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
    if (status == B_OK) {
        ssize_t written = file.Write(data.data(), data.size());
        if (written < 0)
            status = static_cast<status_t>(written);
        else if (static_cast<size_t>(written) != data.size())
            status = B_IO_ERROR;
        else
            status = file.Sync();
        file.Unset();
    }
    if (status == B_OK) {
        BEntry entry(part.c_str());
        status = entry.InitCheck();
        if (status == B_OK)
            status = entry.Rename(path, true);
    }
    if (status != B_OK) {
        BEntry(part.c_str()).Remove();
        error = std::string("Could not save ") + path + ": " + strerror(status);
        return false;
    }
    return true;
}

//------------------------------------------------------------------------
bool PluginHost::loadPreset(const char *path, std::string &error)
{
    BFile file(path, B_READ_ONLY);
    off_t size = 0;
    status_t status = file.InitCheck();
    if (status == B_OK)
        status = file.GetSize(&size);
    if (status != B_OK) {
        error = std::string("Could not open ") + path + ": " + strerror(status);
        return false;
    }
    if (size < static_cast<off_t>(kPresetHeader + 8) || size > kPresetMaxSize) {
        error = std::string(path) + " is not a preset.";
        return false;
    }
    std::vector<char> data(static_cast<size_t>(size));
    ssize_t got = file.ReadAt(0, data.data(), data.size());
    if (got != static_cast<ssize_t>(data.size())) {
        error = std::string("Could not read ") + path + ".";
        return false;
    }
    const char *p = data.data();
    if (memcmp(p, kPresetMagic, sizeof kPresetMagic) != 0) {
        error = std::string(path) + " is not a preset.";
        return false;
    }
    if (GetLE32(p + 8) != kPresetVersion) {
        error = std::string(path) + " was saved by a newer version.";
        return false;
    }
    if (memcmp(p + 12, fClassID, sizeof fClassID) != 0) {
        error = std::string(path) + " is a preset for a different plug-in.";
        return false;
    }
    const uint32 componentSize = GetLE32(p + kPresetHeader);
    const uint32 controllerSize = GetLE32(p + kPresetHeader + 4);
    const uint64_t body = data.size() - (kPresetHeader + 8);
    if (componentSize > INT32_MAX || controllerSize > INT32_MAX ||
        static_cast<uint64_t>(componentSize) + controllerSize > body) {
        error = std::string(path) + " is damaged.";
        return false;
    }
    const char *state = p + kPresetHeader + 8;
    std::vector<char> component(state, state + componentSize);
    std::vector<char> controller(state + componentSize, state + componentSize + controllerSize);
    if (!setState(component, controller)) {
        error = std::string("The plug-in refused ") + path + ".";
        return false;
    }
    return true;
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

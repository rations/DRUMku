// PluginHost — DRUMku.vst3 loaded as a bundle, played on the Media Kit or JACK
// from MIDI sources (a drum kit, a keyboard, a sequencer).
//
// The plug-in is loaded through the SDK's module loader rather than linked in,
// so the editor finds its pad art relative to its own .so exactly as it does
// inside a DAW. MIDI arrives on the client's MIDI input (the Media Kit's
// "DRUMku MIDI in 1" endpoint, or a JACK MIDI port) and the SDK's AudioClient
// turns it into the plug-in's note events. Audio goes through the SDK audiohost's AudioClient on
// one of JackDAW's audio backends (VST3-renku's jdbackendclient): the native Media Kit, or JACK
// through the jack.so add-on, so the app does not need libjack.
//
// Threading: every call here is made on one thread at a time — the app thread
// until the window opens, then the window's thread — which is the single
// "UI thread" the VST3 controller and the component's non-RT calls expect.
// The audio thread only ever runs inside AudioClient::process.

#pragma once

#include "public.sdk/samples/vst-hosting/audiohost/source/media/audioclient.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"

#include <string>
#include <vector>

namespace DRUMku
{

class PluginHost : public Steinberg::Vst::IComponentHandler
{
public:
    PluginHost();
    ~PluginHost();

    // Loads and instantiates the plug-in. False, with a message, on failure.
    bool load(const std::string &bundlePath, std::string &error);

    // Starts audio on "mediakit" or "jack" (stopping any running first).
    // False, with a message, if it could not start; audio is then stopped.
    bool startAudio(const std::string &backend, std::string &error);
    void stopAudio();
    bool audioRunning() const;
    // The backend asked for last ("mediakit" or "jack").
    const std::string &backend() const
    {
        return fBackend;
    }

    // MIDI sources of the running backend ("midi:<endpoint>" on the Media Kit,
    // JACK MIDI ports on JACK).
    std::vector<std::string> midiSources() const;
    // The sources connected to the plug-in's MIDI input.
    std::vector<std::string> connectedMidi() const;
    // Connects (on) or disconnects `source`. False, with a message, on failure.
    bool setMidiSource(const std::string &source, bool on, std::string &error);

    // One line for the status bar: backend, rate, buffer, late cycles, MIDI.
    std::string statusLine() const;

    // Plug-in state (processor + controller), as a DAW saves it in a project.
    bool getState(std::vector<char> &component, std::vector<char> &controller);
    // False if the plug-in refused the component state.
    bool setState(const std::vector<char> &component, const std::vector<char> &controller);

    // Presets: the whole state in a .kasenpreset file, the format Kasen's
    // effects window uses, so presets move between the app and Kasen. The
    // app reads and writes them itself; Kasen does not need to be installed.
    // False, with a message for the user, on failure.
    bool savePreset(const char *path, std::string &error);
    bool loadPreset(const char *path, std::string &error);

    Steinberg::Vst::IEditController *controller() const
    {
        return fController;
    }
    // The audio thread's parameter output (pad activity, MIDI learn); null
    // without audio.
    Steinberg::Vst::ParameterChangeTransfer *outputTransfer() const;

    // IComponentHandler: the editor's edits go to the audio thread.
    Steinberg::tresult PLUGIN_API beginEdit(Steinberg::Vst::ParamID) override
    {
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API performEdit(Steinberg::Vst::ParamID id,
                                              Steinberg::Vst::ParamValue value) override;
    Steinberg::tresult PLUGIN_API endEdit(Steinberg::Vst::ParamID) override
    {
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API restartComponent(Steinberg::int32 flags) override;
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void **obj) override;
    Steinberg::uint32 PLUGIN_API addRef() override
    {
        return 1000;
    }
    Steinberg::uint32 PLUGIN_API release() override
    {
        return 1000;
    }

private:
    class Provider;

    VST3::Hosting::Module::Ptr fModule;
    Steinberg::IPtr<Provider> fProvider;
    Steinberg::Vst::IComponent *fComponent = nullptr;
    Steinberg::Vst::IEditController *fController = nullptr;
    Steinberg::Vst::IMidiMapping *fMidiMapping = nullptr;
    Steinberg::Vst::AudioClientPtr fAudio;
    char fClassID[16] = {}; // the plug-in's VST3 class ID, a preset's identity
    std::string fBackend = "mediakit";
};

} // namespace DRUMku

// MainWindow — the standalone app's one window: a menu bar, DRUMku's own editor
// (IPlugView, kPlatformTypeHaikuBView) and a status line.
//
// The window's thread is the plug-in's UI thread once the window is shown:
// the editor, the controller, the audio backend switch and the MIDI sources
// all run here. A ~30 Hz BMessageRunner moves the audio thread's output
// parameters (AudioClient's lock-free transfer: pad activity and MIDI-learned
// notes) into the controller, which flashes the editor's pads; a 1 s one
// refreshes the status line and reconnects chosen MIDI sources that have
// been plugged back in.

#pragma once

#include "pluginterfaces/gui/iplugview.h"

#include <Message.h>
#include <set>
#include <string>
#include <Window.h>

class BFilePanel;
class BMenu;
class BMenuItem;
class BMessageRunner;
class BStringView;

namespace DRUMku
{

class PluginHost;

class MainWindow : public BWindow, public Steinberg::IPlugFrame
{
public:
    // `host` must outlive the window. `settings` holds the saved backend,
    // MIDI sources and frame; the window updates it and saves it when it
    // closes.
    MainWindow(PluginHost *host, const BMessage &settings);
    ~MainWindow() override;

    bool QuitRequested() override;
    void MessageReceived(BMessage *message) override;
    void MenusBeginning() override;

    // IPlugFrame: the editor asks for a new size.
    Steinberg::tresult PLUGIN_API resizeView(Steinberg::IPlugView *view,
                                             Steinberg::ViewRect *newSize) override;
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
    void StartAudio(const char *backend, bool tellUser);
    void ToggleMidi(const char *source);
    void CheckMidi();
    void SaveMidiChoice();
    void UpdateMenus();
    void UpdateStatus();
    void ShowPresetPanel(bool save);
    void PresetChosen(BMessage *message, bool save);
    void FitToEditor(float width, float height);
    void DetachEditor();

    PluginHost *fHost;
    BMessage fSettings;
    Steinberg::IPtr<Steinberg::IPlugView> fEditor;
    BView *fEditorParent = nullptr;
    BStringView *fStatus = nullptr;
    BMenu *fMidiMenu = nullptr;
    BMenuItem *fMediaKitItem = nullptr;
    BMenuItem *fJackItem = nullptr;
    BMessageRunner *fMeterPump = nullptr;
    BMessageRunner *fStatusPump = nullptr;
    BFilePanel *fOpenPanel = nullptr;
    BFilePanel *fSavePanel = nullptr;
    // The MIDI sources chosen on this backend, plugged in or not, and the
    // ones that were there at the last CheckMidi.
    std::set<std::string> fWantedMidi;
    std::set<std::string> fSeenMidi;
    float fMenuHeight = 0;
    float fStatusHeight = 0;
};

} // namespace DRUMku

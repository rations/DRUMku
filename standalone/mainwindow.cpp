// MainWindow implementation. See mainwindow.h for the threading contract.

#include "mainwindow.h"

#include "pluginhost.h"
#include "settings.h"

#include "pluginterfaces/base/funknown.h"

#include <Alert.h>
#include <Application.h>
#include <Directory.h>
#include <Entry.h>
#include <FilePanel.h>
#include <FindDirectory.h>
#include <Menu.h>
#include <MenuBar.h>
#include <MenuItem.h>
#include <MessageRunner.h>
#include <Path.h>
#include <String.h>
#include <StringView.h>
#include <View.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

using namespace Steinberg;

namespace DRUMku
{

namespace
{

enum : uint32 {
    kMsgBackend = 'dkBk',   // "backend": "mediakit" | "jack"
    kMsgMidi = 'dkMi',      // "source": MIDI source to connect or disconnect
    kMsgMeters = 'dkMt',    // pump the output parameters into the controller
    kMsgStatus = 'dkSt',    // refresh the status line
    kMsgAudioInfo = 'dkAI', // show the audio status in an alert
    kMsgOpenPreset = 'dkPo', // show the open panel
    kMsgSavePreset = 'dkPs', // show the save panel
    kMsgPresetOpened = 'dkPO', // "refs": the preset to load
    kMsgPresetSaved = 'dkPS',  // "directory" + "name": where to save
};

// The preset file name extension (Kasen's effects window uses it too).
const char *kPresetExtension = ".kasenpreset";

// The editor's size if it cannot say (DRUMku's is 760 x 480).
const float kDefaultWidth = 760;
const float kDefaultHeight = 480;

void Tell(const char *title, const std::string &text)
{
    BAlert *alert =
        new BAlert(title, text.c_str(), "OK", nullptr, nullptr, B_WIDTH_AS_USUAL, B_WARNING_ALERT);
    alert->SetFlags(alert->Flags() | B_CLOSE_ON_ESCAPE);
    alert->Go(nullptr); // asynchronous: the window keeps running
}

} // namespace

//------------------------------------------------------------------------
MainWindow::MainWindow(PluginHost *host, const BMessage &settings)
    : BWindow(BRect(100, 100, 100 + kDefaultWidth - 1, 100 + kDefaultHeight - 1), "DRUMku",
              B_TITLED_WINDOW,
              B_NOT_RESIZABLE | B_NOT_ZOOMABLE | B_ASYNCHRONOUS_CONTROLS | B_QUIT_ON_WINDOW_CLOSE),
      fHost(host), fSettings(settings)
{
    // Menus
    BMenuBar *bar = new BMenuBar(BRect(0, 0, kDefaultWidth - 1, 0), "menu");
    BMenu *app = new BMenu("DRUMku");
    app->AddItem(new BMenuItem("About DRUMku" B_UTF8_ELLIPSIS, new BMessage(B_ABOUT_REQUESTED)));
    app->AddSeparatorItem();
    app->AddItem(new BMenuItem("Open preset" B_UTF8_ELLIPSIS, new BMessage(kMsgOpenPreset), 'O'));
    app->AddItem(new BMenuItem("Save preset" B_UTF8_ELLIPSIS, new BMessage(kMsgSavePreset), 'S'));
    app->AddSeparatorItem();
    app->AddItem(new BMenuItem("Quit", new BMessage(B_QUIT_REQUESTED), 'Q'));
    app->SetTargetForItems(this);
    bar->AddItem(app);

    BMenu *audio = new BMenu("Audio");
    BMessage *mk = new BMessage(kMsgBackend);
    mk->AddString("backend", "mediakit");
    audio->AddItem(fMediaKitItem = new BMenuItem("Media Kit", mk));
    BMessage *jack = new BMessage(kMsgBackend);
    jack->AddString("backend", "jack");
    audio->AddItem(fJackItem = new BMenuItem("JACK", jack));
    audio->AddSeparatorItem();
    audio->AddItem(new BMenuItem("Audio status" B_UTF8_ELLIPSIS, new BMessage(kMsgAudioInfo)));
    bar->AddItem(audio);

    // Filled each time it opens (MenusBeginning): MIDI devices come and go.
    fMidiMenu = new BMenu("MIDI");
    bar->AddItem(fMidiMenu);
    AddChild(bar);
    fMenuHeight = bar->Frame().Height() + 1;

    // The editor, below the menu bar
    fEditorParent =
        new BView(BRect(0, fMenuHeight, kDefaultWidth - 1, fMenuHeight + kDefaultHeight - 1),
                  "editor", B_FOLLOW_NONE, 0);
    fEditorParent->SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
    AddChild(fEditorParent);

    // The status line, below the editor (FitToEditor places and sizes it)
    fStatus = new BStringView(BRect(4, 2, kDefaultWidth - 5, 2), "status", "");
    fStatus->ResizeToPreferred(); // the height of one line of the font
    fStatusHeight = fStatus->Frame().Height() + 5;
    BView *statusBack = new BView(BRect(0, 0, kDefaultWidth - 1, fStatusHeight - 1), "status-back",
                                  B_FOLLOW_NONE, 0);
    statusBack->SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
    statusBack->AddChild(fStatus);
    AddChild(statusBack);

    float width = kDefaultWidth;
    float height = kDefaultHeight;
    IPlugView *view = host->controller()->createView(Vst::ViewType::kEditor);
    if (view && view->isPlatformTypeSupported(kPlatformTypeHaikuBView) == kResultTrue) {
        fEditor = owned(view);
        ViewRect size{};
        if (fEditor->getSize(&size) == kResultTrue && size.getWidth() > 0 && size.getHeight() > 0) {
            width = size.getWidth();
            height = size.getHeight();
        }
    } else if (view) {
        view->release();
    }
    FitToEditor(width, height);

    // A new BWindow is locked by the thread that made it until Show(), so the
    // editor is attached under the looper lock, as the platform contract says.
    if (fEditor) {
        fEditor->setFrame(this);
        if (fEditor->attached(fEditorParent, kPlatformTypeHaikuBView) != kResultOk) {
            fprintf(stderr, "DRUMku: the editor could not be attached\n");
            fEditor->setFrame(nullptr);
            fEditor = nullptr;
        }
    }

    BRect frame;
    if (fSettings.FindRect("frame", &frame) == B_OK)
        MoveTo(frame.LeftTop());
    MoveOnScreen(B_MOVE_IF_PARTIALLY_OFFSCREEN);

    BMessage meters(kMsgMeters);
    fMeterPump = new BMessageRunner(BMessenger(this), &meters, 33000); // ~30 Hz
    BMessage status(kMsgStatus);
    fStatusPump = new BMessageRunner(BMessenger(this), &status, 1000000);

    const char *backend = fSettings.GetString("backend", "mediakit");
    StartAudio(backend, false);
}

//------------------------------------------------------------------------
MainWindow::~MainWindow()
{
    delete fOpenPanel;
    delete fSavePanel;
    delete fMeterPump;
    delete fStatusPump;
    DetachEditor();
}

//------------------------------------------------------------------------
void MainWindow::FitToEditor(float width, float height)
{
    fEditorParent->ResizeTo(width - 1, height - 1);
    BView *statusBack = FindView("status-back");
    if (statusBack) {
        statusBack->MoveTo(0, fMenuHeight + height);
        statusBack->ResizeTo(width - 1, fStatusHeight - 1);
        fStatus->ResizeTo(width - 9, fStatusHeight - 5);
    }
    ResizeTo(width - 1, fMenuHeight + height + fStatusHeight - 1);
}

//------------------------------------------------------------------------
void MainWindow::DetachEditor()
{
    if (!fEditor)
        return;
    fEditor->setFrame(nullptr);
    if (fEditor->removed() != kResultOk)
        fprintf(stderr, "DRUMku: the editor could not be detached\n");
    fEditor = nullptr;
}

//------------------------------------------------------------------------
void MainWindow::StartAudio(const char *backend, bool tellUser)
{
    // The choice on the backend being left, before it stops.
    if (fHost->audioRunning()) {
        CheckMidi();
        SaveMidiChoice();
    }
    std::string error;
    if (!fHost->startAudio(backend, error)) {
        // JACK not running (or not installed): fall back to the Media Kit so
        // the app still plays, and say why.
        if (strcmp(backend, "jack") == 0) {
            std::string mkError;
            if (fHost->startAudio("mediakit", mkError))
                error += "\n\nPlaying on the Media Kit instead.";
        }
        Tell("Audio", error);
    } else if (tellUser) {
        UpdateStatus();
    }
    fSettings.SetString("backend", fHost->backend().c_str());

    // The MIDI sources last chosen on this backend; on a first run (nothing
    // saved) every source, so a plugged-in kit plays. The ones there now are
    // connected; the others are connected when they appear (CheckMidi).
    fWantedMidi.clear();
    fSeenMidi.clear();
    if (fHost->audioRunning()) {
        BString key("midi:");
        key << fHost->backend().c_str();
        const bool chosen = fSettings.HasString(key.String()) || fSettings.HasBool(key.String());
        const char *saved = nullptr;
        for (int32 i = 0; fSettings.FindString(key.String(), i, &saved) == B_OK; i++)
            fWantedMidi.insert(saved);
        if (!chosen)
            for (const auto &source : fHost->midiSources())
                fWantedMidi.insert(source);
        CheckMidi();
    }
    UpdateMenus();
    UpdateStatus();
}

//------------------------------------------------------------------------
void MainWindow::ToggleMidi(const char *source)
{
    const std::vector<std::string> connected = fHost->connectedMidi();
    const bool on = std::find(connected.begin(), connected.end(), source) == connected.end();
    std::string error;
    if (!fHost->setMidiSource(source, on, error))
        Tell("MIDI", error);
    if (on)
        fWantedMidi.insert(source);
    else
        fWantedMidi.erase(source);
    SaveMidiChoice();
    UpdateStatus();
}

//------------------------------------------------------------------------
void MainWindow::CheckMidi()
{
    // Called once a second. A chosen source that has just appeared (a kit
    // plugged back in, or present when audio starts) is connected. A source
    // that stays plugged in follows its real connection, so a patchbay
    // (dis)connection is remembered and not undone here. A chosen source that
    // is unplugged stays chosen, so it is reconnected when it comes back.
    if (!fHost->audioRunning())
        return;
    const std::vector<std::string> sources = fHost->midiSources();
    std::set<std::string> present(sources.begin(), sources.end());
    std::set<std::string> failed;
    for (const auto &source : sources) {
        if (fSeenMidi.count(source) || !fWantedMidi.count(source))
            continue;
        std::string error;
        if (!fHost->setMidiSource(source, true, error)) {
            fprintf(stderr, "DRUMku: %s\n", error.c_str());
            failed.insert(source); // tried again next second
        }
    }
    const std::vector<std::string> connected = fHost->connectedMidi();
    std::set<std::string> wanted(connected.begin(), connected.end());
    for (const auto &source : fWantedMidi)
        if (!present.count(source) || failed.count(source) || !fSeenMidi.count(source))
            wanted.insert(source);
    fWantedMidi.swap(wanted);
    for (const auto &source : failed)
        present.erase(source);
    fSeenMidi.swap(present);
}

//------------------------------------------------------------------------
void MainWindow::SaveMidiChoice()
{
    // The chosen sources, per backend, plugged in or not. A bool marks
    // "chosen, and none" so an empty choice is not taken for a first run.
    BString key("midi:");
    key << fHost->backend().c_str();
    fSettings.RemoveName(key.String());
    if (fWantedMidi.empty())
        fSettings.AddBool(key.String(), false);
    for (const auto &source : fWantedMidi)
        fSettings.AddString(key.String(), source.c_str());
}

//------------------------------------------------------------------------
void MainWindow::UpdateMenus()
{
    const bool jack = fHost->backend() == "jack";
    fMediaKitItem->SetMarked(!jack);
    fJackItem->SetMarked(jack);
}

//------------------------------------------------------------------------
void MainWindow::MenusBeginning()
{
    // MIDI devices come and go: list the sources afresh, ticking the
    // connected ones. Several can play at once (a kit and a keyboard).
    while (BMenuItem *item = fMidiMenu->RemoveItem((int32)0))
        delete item;
    const std::vector<std::string> sources = fHost->midiSources();
    const std::vector<std::string> connected = fHost->connectedMidi();
    for (const auto &source : sources) {
        BMessage *message = new BMessage(kMsgMidi);
        message->AddString("source", source.c_str());
        // The Media Kit lists sources as "midi:<endpoint>".
        const char *label = source.c_str();
        if (source.compare(0, 5, "midi:") == 0)
            label += 5;
        BMenuItem *item = new BMenuItem(label, message);
        for (const auto &c : connected)
            if (c == source)
                item->SetMarked(true);
        fMidiMenu->AddItem(item);
    }
    if (sources.empty()) {
        BMenuItem *none = new BMenuItem(
            fHost->audioRunning() ? "No MIDI sources found" : "Audio is not running", nullptr);
        none->SetEnabled(false);
        fMidiMenu->AddItem(none);
    }
    fMidiMenu->SetTargetForItems(this);
    UpdateMenus();
    BWindow::MenusBeginning();
}

//------------------------------------------------------------------------
void MainWindow::ShowPresetPanel(bool save)
{
    BFilePanel *&panel = save ? fSavePanel : fOpenPanel;
    if (!panel) {
        BMessenger target(this);
        BMessage message(save ? kMsgPresetSaved : kMsgPresetOpened);
        panel = new BFilePanel(save ? B_SAVE_PANEL : B_OPEN_PANEL, &target, nullptr, B_FILE_NODE,
                               false, &message);
        panel->Window()->SetTitle(save ? "DRUMku: Save preset" : "DRUMku: Open preset");
        // The folder last used for presets, else ~/config/settings/DRUMku/presets.
        const char *dir = fSettings.GetString("presetDir", nullptr);
        BPath path;
        if (dir) {
            panel->SetPanelDirectory(dir);
        } else if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) == B_OK &&
                   path.Append("DRUMku/presets") == B_OK &&
                   create_directory(path.Path(), 0755) == B_OK) {
            panel->SetPanelDirectory(path.Path());
        }
        if (save) {
            BString name("DRUMku");
            name << kPresetExtension;
            panel->SetSaveText(name.String());
        }
    }
    panel->Show();
}

//------------------------------------------------------------------------
void MainWindow::PresetChosen(BMessage *message, bool save)
{
    BPath path;
    if (save) {
        entry_ref dir;
        const char *name = nullptr;
        if (message->FindRef("directory", &dir) != B_OK ||
            message->FindString("name", &name) != B_OK || !name)
            return;
        BString file(name);
        if (!file.EndsWith(kPresetExtension))
            file << kPresetExtension;
        if (path.SetTo(&dir) != B_OK || path.Append(file.String()) != B_OK)
            return;
    } else {
        entry_ref ref;
        if (message->FindRef("refs", &ref) != B_OK || path.SetTo(&ref) != B_OK)
            return;
    }
    std::string error;
    const bool ok = save ? fHost->savePreset(path.Path(), error)
                         : fHost->loadPreset(path.Path(), error);
    if (!ok) {
        Tell(save ? "Save preset" : "Open preset", error);
        return;
    }
    BPath parent;
    if (path.GetParent(&parent) == B_OK)
        fSettings.SetString("presetDir", parent.Path());
    BString title("DRUMku: ");
    title << path.Leaf();
    SetTitle(title.String());
}

//------------------------------------------------------------------------
void MainWindow::UpdateStatus()
{
    fStatus->SetText(fHost->statusLine().c_str());
}

//------------------------------------------------------------------------
bool MainWindow::QuitRequested()
{
    // Save what to restore next time, then take the editor down on this
    // thread with the looper locked, before the window and its views go.
    std::vector<char> component, controller;
    if (fHost->getState(component, controller)) {
        fSettings.RemoveName("component");
        fSettings.RemoveName("controller");
        fSettings.AddData("component", B_RAW_TYPE, component.data(), component.size());
        if (!controller.empty())
            fSettings.AddData("controller", B_RAW_TYPE, controller.data(), controller.size());
    }
    // The MIDI sources as they are now, so ones (dis)connected in a patchbay
    // since the last menu change are remembered too.
    if (fHost->audioRunning()) {
        CheckMidi();
        SaveMidiChoice();
    }
    fSettings.SetRect("frame", Frame());
    if (SaveSettings(fSettings) != B_OK)
        fprintf(stderr, "DRUMku: could not save the settings\n");

    delete fMeterPump;
    fMeterPump = nullptr;
    delete fStatusPump;
    fStatusPump = nullptr;
    DetachEditor();
    fHost->stopAudio();
    return true;
}

//------------------------------------------------------------------------
void MainWindow::MessageReceived(BMessage *message)
{
    switch (message->what) {
        case kMsgMeters: {
            Vst::ParameterChangeTransfer *transfer = fHost->outputTransfer();
            Vst::ParamID id;
            Vst::ParamValue value;
            int32 sampleOffset;
            while (transfer && transfer->getNextChange(id, value, sampleOffset))
                fHost->controller()->setParamNormalized(id, value);
            break;
        }
        case kMsgStatus:
            CheckMidi();
            UpdateStatus();
            break;
        case kMsgBackend: {
            const char *backend = message->GetString("backend", "mediakit");
            if (fHost->backend() != backend || !fHost->audioRunning())
                StartAudio(backend, true);
            break;
        }
        case kMsgMidi:
            ToggleMidi(message->GetString("source", ""));
            break;
        case kMsgAudioInfo:
            Tell("Audio status", fHost->statusLine());
            break;
        case kMsgOpenPreset:
            ShowPresetPanel(false);
            break;
        case kMsgSavePreset:
            ShowPresetPanel(true);
            break;
        case kMsgPresetOpened:
            PresetChosen(message, false);
            break;
        case kMsgPresetSaved:
            PresetChosen(message, true);
            break;
        case B_ABOUT_REQUESTED:
            be_app->PostMessage(B_ABOUT_REQUESTED);
            break;
        default:
            BWindow::MessageReceived(message);
            break;
    }
}

//------------------------------------------------------------------------
tresult PLUGIN_API MainWindow::resizeView(IPlugView *view, ViewRect *newSize)
{
    if (!view || !newSize || view != fEditor)
        return kInvalidArgument;
    FitToEditor(newSize->getWidth(), newSize->getHeight());
    return fEditor->onSize(newSize);
}

//------------------------------------------------------------------------
tresult PLUGIN_API MainWindow::queryInterface(const TUID iid, void **obj)
{
    if (!obj)
        return kInvalidArgument;
    if (FUnknownPrivate::iidEqual(iid, IPlugFrame::iid) ||
        FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
        *obj = static_cast<IPlugFrame *>(this);
        return kResultOk;
    }
    *obj = nullptr;
    return kNoInterface;
}

} // namespace DRUMku

// DRUMku native Haiku editor implementation.
//
// The drum-kit view: an electronic kit seen from the player's seat, composited
// from ImageMagick-generated PNG layers (gui/make_*.sh -> resource/gui/,
// loaded from the bundle's Contents/Resources/gui). Hex pads (kick, snare,
// toms) and round cymbal pads (hi-hat, crashes, ride) map to fixed slots 0-8;
// the control strip below drives the selected pad: Load/Clear .wav (via the
// controller's IDrumLoader), MIDI Learn (plug-in-side, fed back through the
// note parameter), and a volume slider. "Expand kit" opens the kit rack page,
// one row per shown slot, which is the only way to reach slots beyond the nine
// pads; its footer grows and shrinks the shown row count. Pads flash on the
// hidden activity parameters and every sprite load is NULL-checked with a flat-color
// fallback, so a missing Resources dir degrades, never crashes.
//
// Threading: everything in this file runs on the host window's looper thread
// (the kPlatformTypeHaikuBView contract); the controller is same-thread.

#include "drumview.h"
#include "drumcontroller.h"
#include "drumgeometry.h"

#include <Bitmap.h>
#include <Button.h>
#include <FilePanel.h>
#include <MessageRunner.h>
#include <Path.h>
#include <ScrollBar.h>
#include <ScrollView.h>
#include <Slider.h>
#include <StringView.h>
#include <TranslationUtils.h>
#include <View.h>
#include <Window.h>

#include <image.h>

#include <cmath>
#include <cstdio>
#include <cstring>

using namespace Steinberg;

namespace DRUMku
{

namespace
{
const uint32 kMsgVolSlider = 'dkVl';
const uint32 kMsgLoadBtn = 'dkLd';
const uint32 kMsgClearBtn = 'dkCl';
const uint32 kMsgLearnBtn = 'dkLn';
const uint32 kMsgTick = 'dkTk';
const uint32 kMsgExpand = 'dkEx';
const uint32 kMsgRackClose = 'dkRc';
const uint32 kMsgRackAdd = 'dkRa';
const uint32 kMsgRackRemove = 'dkRr';
const int32 kSliderScale = 1000; // BSlider is integer-valued; 0..1000 -> 0..1

// Kit rack page layout (editor canvas coordinates).
const float kRackRowsTop = 56;
const float kRackRowH = 32;
const int kRackVisibleRows = 11;
const float kRackFooterTop = kRackRowsTop + kRackVisibleRows * kRackRowH + 10;
const rgb_color kRackBg = {20, 23, 28, 255};
const rgb_color kRackRowBg = {26, 29, 35, 255};

const rgb_color kStripBg = {12, 14, 17, 255}; // geometry.sh STRIP_BG
const rgb_color kLabelColor = {205, 210, 215, 255};
const rgb_color kDimColor = {140, 146, 152, 255};
const rgb_color kPadFallback = {24, 26, 31, 255}; // geometry.sh PAD_BODY
const rgb_color kRimFallback = {58, 63, 70, 255}; // geometry.sh PAD_RIM

// Resolve <bundle>/Contents/Resources/gui from this plug-in's own image:
// walk the loaded images for the one whose text segment contains this
// function, then go .so -> x86_64-haiku -> Contents -> Resources/gui.
bool resourceDir(BPath &out)
{
    image_info info;
    int32 cookie = 0;
    addr_t marker = (addr_t)&resourceDir;
    while (get_next_image_info(0, &cookie, &info) == B_OK) {
        addr_t text = (addr_t)info.text;
        if (marker < text || marker >= text + (addr_t)info.text_size)
            continue;
        BPath module(info.name);
        BPath archDir, contents;
        if (module.GetParent(&archDir) != B_OK || archDir.GetParent(&contents) != B_OK)
            return false;
        out = contents;
        return out.Append("Resources/gui") == B_OK;
    }
    return false;
}

BBitmap *loadSprite(const BPath &dir, const char *name)
{
    BPath p(dir);
    if (p.Append(name) != B_OK)
        return nullptr;
    BBitmap *bmp = BTranslationUtils::GetBitmapFile(p.Path());
    if (!bmp)
        fprintf(stderr, "DRUMku: missing sprite %s (flat fallback)\n", p.Path());
    return bmp;
}

void noteName(int note, char *out, size_t outSize)
{
    static const char *kNames[12] = {"C",  "C#", "D",  "D#", "E",  "F",
                                     "F#", "G",  "G#", "A",  "A#", "B"};
    if (note < 0 || note > 127)
        std::snprintf(out, outSize, "unassigned");
    else
        std::snprintf(out, outSize, "%s%d (%d)", kNames[note % 12], note / 12 - 1, note);
}

// Point-in-pad tests (dx/dy relative to the pad center, r = circumradius).
bool insideHexFlatTop(float dx, float dy, float r)
{
    const float s3 = 1.7320508f;
    dx = std::fabs(dx);
    dy = std::fabs(dy);
    return dy <= s3 * 0.5f * r && s3 * dx + dy <= s3 * r;
}

bool insideCircle(float dx, float dy, float r)
{
    return dx * dx + dy * dy <= r * r;
}

} // namespace

//------------------------------------------------------------------------
// DrumRackView — the kit rack page: every shown slot as a row, so the slots
// the nine pads do not reach can still be loaded, learned and mixed. It covers
// the whole editor while open. It holds no state of its own: every value is
// read from the controller, and every edit is posted to the kit view, which
// owns the file panel and the learn state and applies it exactly as the strip
// does.
//------------------------------------------------------------------------
class DrumRackView : public BView
{
public:
    DrumRackView(BRect frame, BHandler *target)
        : BView(frame, "DRUMku-rack", B_FOLLOW_NONE, B_WILL_DRAW), mTarget(target)
    {
        SetViewColor(kRackBg);
        const float w = frame.Width();

        mClose = new BButton(BRect(w - 96, 14, w - 16, 40), "rack-close", "Close",
                             new BMessage(kMsgRackClose));
        AddChild(mClose);

        // The rows live in a plain view taller than its scroll view: the
        // scroll bar's range is the hidden part, set in SetSlotCount().
        BRect rowsFrame(0, 0, w - B_V_SCROLL_BAR_WIDTH - 1, kRackVisibleRows * kRackRowH - 1);
        mRows = new BView(rowsFrame, "rack-rows", B_FOLLOW_NONE, B_WILL_DRAW);
        mRows->SetViewColor(kRackBg);
        for (int i = 0; i < kMaxSlots; ++i)
            buildRow(i, rowsFrame.Width());
        mScroll = new BScrollView("rack-scroll", mRows, B_FOLLOW_NONE, 0, false, true, B_NO_BORDER);
        mScroll->MoveTo(0, kRackRowsTop);
        AddChild(mScroll);

        const float fy = kRackFooterTop;
        mAdd = new BButton(BRect(16, fy, 136, fy + 28), "rack-add", "+ Add slot",
                           new BMessage(kMsgRackAdd));
        AddChild(mAdd);
        mRemove = new BButton(BRect(144, fy, 276, fy + 28), "rack-remove",
                              "\xe2\x88\x92 Remove slot", new BMessage(kMsgRackRemove));
        AddChild(mRemove);
        BStringView *hint = new BStringView(
            BRect(290, fy + 4, w - 16, fy + 24), "rack-hint",
            "Hidden slots keep their sample and keep playing \xc2\xb7 wheel scrolls");
        hint->SetViewColor(kRackBg);
        hint->SetHighColor(kDimColor);
        AddChild(hint);
    }

    void AttachedToWindow() override
    {
        BView::AttachedToWindow();
        mClose->SetTarget(mTarget);
        mAdd->SetTarget(mTarget);
        mRemove->SetTarget(mTarget);
        for (int i = 0; i < kMaxSlots; ++i) {
            mRow[i].load->SetTarget(mTarget);
            mRow[i].clear->SetTarget(mTarget);
            mRow[i].learn->SetTarget(mTarget);
            mRow[i].volume->SetTarget(mTarget);
        }
    }

    void Draw(BRect) override
    {
        SetHighColor(kLabelColor);
        SetFontSize(15);
        DrawString("KIT RACK", BPoint(20, 34));
        char text[64];
        std::snprintf(text, sizeof(text), "%d of %d slots shown", mSlotCount, (int)kMaxSlots);
        SetHighColor(kDimColor);
        SetFontSize(12);
        DrawString(text, BPoint(140, 33));
        SetHighColor(58, 63, 70);
        StrokeLine(BPoint(16, kRackRowsTop - 6), BPoint(Bounds().right - 16, kRackRowsTop - 6));
        StrokeLine(BPoint(16, kRackFooterTop - 6), BPoint(Bounds().right - 16, kRackFooterTop - 6));
    }

    // Show rows 0 .. count-1. Growing scrolls the new row into view; shrinking
    // keeps the scroll position inside the shorter list.
    void SetSlotCount(int count)
    {
        if (count < 1)
            count = 1;
        if (count > kMaxSlots)
            count = kMaxSlots;
        const bool grew = count > mSlotCount;
        for (int i = 0; i < kMaxSlots; ++i) {
            BView *row = mRow[i].row;
            if (i < count && row->IsHidden())
                row->Show();
            else if (i >= count && !row->IsHidden())
                row->Hide();
        }
        mSlotCount = count;

        const float hidden = (count - kRackVisibleRows) * kRackRowH;
        if (BScrollBar *bar = mScroll->ScrollBar(B_VERTICAL)) {
            bar->SetRange(0, hidden > 0 ? hidden : 0);
            bar->SetProportion((float)kRackVisibleRows / (float)(count > 0 ? count : 1));
            bar->SetSteps(kRackRowH, kRackRowH * (kRackVisibleRows - 1));
            if (grew && hidden > 0)
                bar->SetValue(hidden);
        }
        mRemove->SetEnabled(count > 1);
        mAdd->SetEnabled(count < kMaxSlots);
        Invalidate();
    }

    void RefreshRow(int slot, DrumController *controller, int armedSlot, const char *padName)
    {
        if (slot < 0 || slot >= kMaxSlots)
            return;
        Row &r = mRow[slot];
        char text[B_PATH_NAME_LENGTH + 32];

        if (padName)
            std::snprintf(text, sizeof(text), "%d  %s", slot + 1, padName);
        else
            std::snprintf(text, sizeof(text), "%d  Slot %d", slot + 1, slot + 1);
        r.name->SetText(text);

        char path[B_PATH_NAME_LENGTH] = "";
        if (controller->getSampleFile(slot, path, sizeof(path)) == kResultOk && path[0]) {
            const char *base = std::strrchr(path, '/');
            r.file->SetText(base ? base + 1 : path);
            r.file->SetHighColor(kLabelColor);
        } else {
            r.file->SetText("(empty)");
            r.file->SetHighColor(kDimColor);
        }

        if (armedSlot == slot) {
            r.note->SetText("learning" B_UTF8_ELLIPSIS);
        } else {
            double norm = controller->getParamNormalized((Vst::ParamID)(kSlotNoteBase + slot));
            int note = (int)(norm * (double)kNoteUnassigned + 0.5);
            noteName(note >= kNoteUnassigned ? -1 : note, text, sizeof(text));
            r.note->SetText(text);
        }
        r.learn->SetLabel(armedSlot == slot ? "Stop" : "Learn");

        SetVolume(slot, controller->getParamNormalized((Vst::ParamID)(kSlotVolumeBase + slot)));
    }

    void SetVolume(int slot, double norm)
    {
        if (slot < 0 || slot >= kMaxSlots)
            return;
        int32 v = (int32)(norm * kSliderScale + 0.5);
        if (mRow[slot].volume->Value() != v)
            mRow[slot].volume->SetValue(v);
    }

private:
    struct Row {
        BView *row = nullptr;
        BStringView *name = nullptr;
        BStringView *file = nullptr;
        BButton *load = nullptr;
        BButton *clear = nullptr;
        BButton *learn = nullptr;
        BSlider *volume = nullptr;
        BStringView *note = nullptr;
    };

    static BMessage *slotMessage(uint32 what, int slot)
    {
        BMessage *msg = new BMessage(what);
        msg->AddInt32("slot", slot);
        return msg;
    }

    void buildRow(int slot, float width)
    {
        Row &r = mRow[slot];
        r.row = new BView(BRect(0, slot * kRackRowH, width, (slot + 1) * kRackRowH - 1), "rack-row",
                          B_FOLLOW_NONE, 0);
        r.row->SetViewColor((slot % 2) ? kRackRowBg : kRackBg);
        const rgb_color bg = r.row->ViewColor();

        r.name = new BStringView(BRect(8, 6, 116, 26), "name", "");
        r.file = new BStringView(BRect(120, 6, 296, 26), "file", "");
        r.note = new BStringView(BRect(642, 6, width - 4, 26), "note", "");
        BStringView *labels[3] = {r.name, r.file, r.note};
        for (BStringView *label : labels) {
            label->SetViewColor(bg);
            label->SetLowColor(bg);
            label->SetHighColor(kLabelColor);
            r.row->AddChild(label);
        }

        r.load = new BButton(BRect(300, 3, 360, 29), "load", "Load" B_UTF8_ELLIPSIS,
                             slotMessage(kMsgLoadBtn, slot));
        r.clear =
            new BButton(BRect(364, 3, 420, 29), "clear", "Clear", slotMessage(kMsgClearBtn, slot));
        r.learn =
            new BButton(BRect(424, 3, 480, 29), "learn", "Learn", slotMessage(kMsgLearnBtn, slot));
        r.row->AddChild(r.load);
        r.row->AddChild(r.clear);
        r.row->AddChild(r.learn);

        r.volume = new BSlider(BRect(486, 2, 636, 30), "volume", nullptr,
                               slotMessage(kMsgVolSlider, slot), 0, kSliderScale);
        r.volume->SetModificationMessage(slotMessage(kMsgVolSlider, slot));
        r.volume->SetViewColor(bg);
        r.volume->SetLowColor(bg);
        r.row->AddChild(r.volume);

        if (slot >= kDefaultSlotCount)
            r.row->Hide();
        mRows->AddChild(r.row);
    }

    BHandler *mTarget;
    BButton *mClose = nullptr;
    BButton *mAdd = nullptr;
    BButton *mRemove = nullptr;
    BView *mRows = nullptr;
    BScrollView *mScroll = nullptr;
    Row mRow[kMaxSlots];
    int mSlotCount = kDefaultSlotCount;
};

//------------------------------------------------------------------------
// DrumKitView — the actual BView inside the host's parent view.
//------------------------------------------------------------------------
class DrumKitView : public BView
{
public:
    DrumKitView(BRect frame, DrumController *controller)
        : BView(frame, "DRUMku-editor", B_FOLLOW_NONE, B_WILL_DRAW), mController(controller)
    {
        SetViewColor(B_TRANSPARENT_COLOR); // we repaint everything ourselves

        // Sprites: background + the four layers of each pad's family.
        BPath dir;
        if (resourceDir(dir)) {
            mBackground = loadSprite(dir, "background.png");
            char name[64];
            for (int i = 0; i < geo::kPadCount; ++i) {
                int f = familyIndex(geo::kPads[i].sprite);
                if (f >= 0 && !mFamilyLoaded[f]) {
                    mFamilyLoaded[f] = true;
                    std::snprintf(name, sizeof(name), "pad_%s_base.png", geo::kPads[i].sprite);
                    mBase[f] = loadSprite(dir, name);
                    std::snprintf(name, sizeof(name), "pad_%s_hit.png", geo::kPads[i].sprite);
                    mHit[f] = loadSprite(dir, name);
                    std::snprintf(name, sizeof(name), "pad_%s_sel.png", geo::kPads[i].sprite);
                    mSel[f] = loadSprite(dir, name);
                    std::snprintf(name, sizeof(name), "pad_%s_arm.png", geo::kPads[i].sprite);
                    mArm[f] = loadSprite(dir, name);
                }
            }
        } else {
            fprintf(stderr, "DRUMku: cannot locate bundle Resources (flat fallback)\n");
        }

        buildStrip(frame);
    }

    ~DrumKitView() override
    {
        delete mFilePanel;
        delete mBackground;
        for (int f = 0; f < kMaxFamilies; ++f) {
            delete mBase[f];
            delete mHit[f];
            delete mSel[f];
            delete mArm[f];
            delete mHitScratch[f];
        }
        delete mOff;
    }

    void AttachedToWindow() override
    {
        BView::AttachedToWindow();
        mVolSlider->SetTarget(this);
        mLoadButton->SetTarget(this);
        mClearButton->SetTarget(this);
        mLearnButton->SetTarget(this);
        mExpandButton->SetTarget(this);

        // Offscreen double buffer (falls back to direct drawing if refused).
        BRect b = Bounds();
        mOff = new BBitmap(b, B_BITMAP_ACCEPTS_VIEWS, B_RGBA32);
        if (mOff && mOff->IsValid()) {
            mOffView = new BView(b, "offscreen", B_FOLLOW_NONE, 0);
            mOff->AddChild(mOffView);
        } else {
            delete mOff;
            mOff = nullptr;
            mOffView = nullptr;
        }

        BMessage tick(kMsgTick);
        mTick = new BMessageRunner(BMessenger(this), &tick, 33000); // ~30 Hz

        SelectPad(0);
    }

    void DetachedFromWindow() override
    {
        delete mTick;
        mTick = nullptr;
        BView::DetachedFromWindow();
    }

    void Draw(BRect) override
    {
        if (mOff && mOffView && mOff->Lock()) {
            Compose(mOffView);
            mOffView->Sync();
            mOff->Unlock();
            SetDrawingMode(B_OP_COPY);
            DrawBitmap(mOff, B_ORIGIN);
        } else {
            Compose(this);
        }
    }

    void MouseDown(BPoint where) override
    {
        for (int i = 0; i < geo::kPadCount; ++i) {
            const geo::PadSpec &p = geo::kPads[i];
            float dx = where.x - p.x, dy = where.y - p.y;
            bool inside =
                p.hex ? insideHexFlatTop(dx, dy, (float)p.r) : insideCircle(dx, dy, (float)p.r);
            if (inside) {
                SelectPad(i);
                return;
            }
        }
        BView::MouseDown(where);
    }

    void MessageReceived(BMessage *message) override
    {
        switch (message->what) {
            case kMsgVolSlider: {
                // From the strip (selected pad) or a rack row ("slot"); both
                // carry the slider's own value. The controller pushes the
                // change back through ParamChanged, which syncs the other one.
                const int slot = messageSlot(message);
                int32 raw = 0;
                if (message->FindInt32("be:value", &raw) != B_OK)
                    raw = mVolSlider->Value();
                Vst::ParamID id = (Vst::ParamID)(kSlotVolumeBase + slot);
                Vst::ParamValue norm = (double)raw / (double)kSliderScale;
                mController->beginEdit(id);
                mController->setParamNormalized(id, norm);
                mController->performEdit(id, norm);
                mController->endEdit(id);
                break;
            }
            case kMsgLoadBtn: {
                if (!mFilePanel)
                    mFilePanel =
                        new BFilePanel(B_OPEN_PANEL, new BMessenger(this), nullptr, 0, false);
                mLoadSlot = messageSlot(message);
                mFilePanel->Show();
                break;
            }
            case B_REFS_RECEIVED: {
                entry_ref ref;
                if (message->FindRef("refs", &ref) == B_OK) {
                    BPath path(&ref);
                    if (path.InitCheck() == B_OK &&
                        mController->setSampleFile(mLoadSlot, path.Path()) != kResultOk)
                        fprintf(stderr, "DRUMku: setSampleFile failed for '%s'\n", path.Path());
                    RefreshStrip();
                    RefreshRackRow(mLoadSlot);
                }
                break;
            }
            case kMsgClearBtn: {
                const int slot = messageSlot(message);
                if (mController->setSampleFile(slot, "") != kResultOk)
                    fprintf(stderr, "DRUMku: clearing the sample failed\n");
                RefreshStrip();
                RefreshRackRow(slot);
                break;
            }
            case kMsgLearnBtn: {
                const int slot = messageSlot(message);
                const int previous = mArmedSlot;
                if (mArmedSlot == slot) {
                    mController->armLearn(-1); // toggle off
                    mArmedSlot = -1;
                } else if (mController->armLearn(slot) == kResultOk) {
                    mArmedSlot = slot;
                } else {
                    fprintf(stderr, "DRUMku: arming MIDI learn failed\n");
                }
                RefreshStrip();
                RefreshRackRow(previous);
                RefreshRackRow(slot);
                Invalidate();
                break;
            }
            case kMsgExpand:
                OpenRack();
                break;
            case kMsgRackClose:
                CloseRack();
                break;
            case kMsgRackAdd:
                SetSlotCount(SlotCount() + 1);
                break;
            case kMsgRackRemove:
                SetSlotCount(SlotCount() - 1);
                break;
            case kMsgTick: {
                bool animating = mArmedSlot >= 0;
                for (int i = 0; i < geo::kPadCount; ++i) {
                    if (mHitLevel[i] > 0.0f) {
                        mHitLevel[i] *= 0.80f;
                        if (mHitLevel[i] < 0.06f)
                            mHitLevel[i] = 0.0f;
                        animating = true;
                    }
                }
                ++mPulsePhase;
                if (animating)
                    Invalidate();
                break;
            }
            default:
                BView::MessageReceived(message);
                break;
        }
    }

    // Called by DrumEditorView on the window looper thread.
    void ParamChanged(Vst::ParamID id, Vst::ParamValue value)
    {
        if (id >= (Vst::ParamID)kSlotActivityBase &&
            id < (Vst::ParamID)(kSlotActivityBase + kMaxSlots)) {
            int slot = (int)(id - kSlotActivityBase);
            int pad = padForSlot(slot);
            if (pad >= 0) {
                float level = 0.45f + 0.55f * (float)value;
                mHitLevel[pad] = level > 1.0f ? 1.0f : level;
                Invalidate();
            }
            return;
        }
        if (id >= (Vst::ParamID)kSlotNoteBase && id < (Vst::ParamID)(kSlotNoteBase + kMaxSlots)) {
            int slot = (int)(id - kSlotNoteBase);
            if (slot == mArmedSlot) {
                mArmedSlot = -1; // learn completed
                Invalidate();
            }
            if (slot == selectedSlot())
                RefreshStrip();
            RefreshRackRow(slot);
            return;
        }
        if (id >= (Vst::ParamID)kSlotVolumeBase &&
            id < (Vst::ParamID)(kSlotVolumeBase + kMaxSlots)) {
            const int slot = (int)(id - kSlotVolumeBase);
            if (slot == selectedSlot()) {
                int32 v = (int32)(value * kSliderScale + 0.5);
                if (mVolSlider->Value() != v)
                    mVolSlider->SetValue(v);
            }
            if (mRack)
                mRack->SetVolume(slot, value);
            return;
        }
        if (id == (Vst::ParamID)kSlotCountId && mRack)
            mRack->SetSlotCount(SlotCount());
    }

private:
    static const int kMaxFamilies = geo::kPadCount;

    int familyIndex(const char *sprite)
    {
        for (int f = 0; f < mFamilyCount; ++f) {
            if (std::strcmp(mFamilyKey[f], sprite) == 0)
                return f;
        }
        if (mFamilyCount >= kMaxFamilies)
            return -1;
        mFamilyKey[mFamilyCount] = sprite;
        return mFamilyCount++;
    }

    int selectedSlot() const
    {
        return geo::kPads[mSelected].slot;
    }

    // A rack row names its slot in the message; the strip acts on the
    // selected pad's slot.
    int messageSlot(const BMessage *message) const
    {
        int32 slot = -1;
        if (message->FindInt32("slot", &slot) == B_OK && slot >= 0 && slot < kMaxSlots)
            return (int)slot;
        return selectedSlot();
    }

    // Rows the rack shows: kSlotCountId is a 1..kMaxSlots range parameter.
    int SlotCount() const
    {
        double norm = mController->getParamNormalized((Vst::ParamID)kSlotCountId);
        return 1 + (int)(norm * (double)(kMaxSlots - 1) + 0.5);
    }

    // Growing the rack only reveals rows: every slot already has its
    // parameters, and the engine plays any slot with a sample and a bound
    // note whether or not its row is shown.
    void SetSlotCount(int count)
    {
        if (count < 1)
            count = 1;
        if (count > kMaxSlots)
            count = kMaxSlots;
        if (count == SlotCount())
            return;
        const Vst::ParamID id = (Vst::ParamID)kSlotCountId;
        const double norm = (double)(count - 1) / (double)(kMaxSlots - 1);
        mController->beginEdit(id);
        mController->setParamNormalized(id, norm);
        mController->performEdit(id, norm);
        mController->endEdit(id);
        if (mRack)
            mRack->SetSlotCount(count); // ParamChanged may not echo an edit we made
    }

    // The rack is built on first use (it is 64 rows of controls) and then
    // shown and hidden. While it is open it covers the whole editor, so the
    // strip controls, its siblings, are hidden rather than overlapped.
    void OpenRack()
    {
        if (!mRack) {
            mRack = new DrumRackView(Bounds(), this);
            AddChild(mRack);
        } else if (mRack->IsHidden()) {
            mRack->Show();
        }
        SetStripVisible(false);
        mRack->SetSlotCount(SlotCount());
        for (int i = 0; i < kMaxSlots; ++i)
            RefreshRackRow(i);
    }

    void CloseRack()
    {
        if (mRack && !mRack->IsHidden())
            mRack->Hide();
        SetStripVisible(true);
        RefreshStrip();
        Invalidate();
    }

    void SetStripVisible(bool visible)
    {
        BView *strip[] = {mPadLabel,    mFileLabel,   mNoteLabel, mLoadButton,
                          mClearButton, mLearnButton, mVolSlider, mExpandButton};
        for (BView *v : strip) {
            if (visible && v->IsHidden())
                v->Show();
            else if (!visible && !v->IsHidden())
                v->Hide();
        }
    }

    void RefreshRackRow(int slot)
    {
        if (!mRack || slot < 0 || slot >= kMaxSlots)
            return;
        const int pad = padForSlot(slot);
        mRack->RefreshRow(slot, mController, mArmedSlot, pad >= 0 ? geo::kPads[pad].name : nullptr);
    }

    int padForSlot(int slot) const
    {
        for (int i = 0; i < geo::kPadCount; ++i) {
            if (geo::kPads[i].slot == slot)
                return i;
        }
        return -1;
    }

    void buildStrip(BRect frame)
    {
        const float y0 = (float)geo::kStripY;

        mPadLabel = new BStringView(BRect(16, y0 + 8, 190, y0 + 28), "pad-name", "");
        stripLabel(mPadLabel, kLabelColor);
        mFileLabel = new BStringView(BRect(200, y0 + 8, 580, y0 + 28), "file-name", "");
        stripLabel(mFileLabel, kDimColor);
        mNoteLabel = new BStringView(BRect(590, y0 + 8, frame.Width() - 12, y0 + 28), "note", "");
        stripLabel(mNoteLabel, kDimColor);

        mLoadButton = new BButton(BRect(16, y0 + 40, 106, y0 + 68), "load", "Load" B_UTF8_ELLIPSIS,
                                  new BMessage(kMsgLoadBtn));
        AddChild(mLoadButton);
        mClearButton = new BButton(BRect(114, y0 + 40, 194, y0 + 68), "clear", "Clear",
                                   new BMessage(kMsgClearBtn));
        AddChild(mClearButton);
        mLearnButton = new BButton(BRect(202, y0 + 40, 282, y0 + 68), "learn", "Learn",
                                   new BMessage(kMsgLearnBtn));
        AddChild(mLearnButton);

        mVolSlider = new BSlider(BRect(300, y0 + 36, frame.Width() - 16, y0 + 80), "volume",
                                 "Volume", new BMessage(kMsgVolSlider), 0, kSliderScale);
        mVolSlider->SetModificationMessage(new BMessage(kMsgVolSlider));
        mVolSlider->SetViewColor(kStripBg);
        mVolSlider->SetLowColor(kStripBg);
        mVolSlider->SetHighColor(kLabelColor);
        AddChild(mVolSlider);

        // The nine pads reach slots 1-9 only; the rack reaches all of them.
        mExpandButton =
            new BButton(BRect(frame.Width() - 168, y0 + 86, frame.Width() - 16, y0 + 114), "expand",
                        "Expand kit \xe2\x80\xba", new BMessage(kMsgExpand));
        AddChild(mExpandButton);
    }

    void stripLabel(BStringView *label, rgb_color color)
    {
        label->SetViewColor(kStripBg);
        label->SetHighColor(color);
        AddChild(label);
    }

    void SelectPad(int pad)
    {
        mSelected = pad;
        RefreshStrip();
        Invalidate();
    }

    void RefreshStrip()
    {
        const geo::PadSpec &p = geo::kPads[mSelected];
        const int slot = p.slot;

        char text[B_PATH_NAME_LENGTH + 32];
        std::snprintf(text, sizeof(text), "%s  (slot %d)", p.name, slot + 1);
        mPadLabel->SetText(text);

        char path[B_PATH_NAME_LENGTH] = "";
        if (mController->getSampleFile(slot, path, sizeof(path)) == kResultOk && path[0]) {
            const char *base = std::strrchr(path, '/');
            std::snprintf(text, sizeof(text), "sample: %s", base ? base + 1 : path);
        } else {
            std::snprintf(text, sizeof(text), "sample: (none)");
        }
        mFileLabel->SetText(text);

        if (mArmedSlot == slot) {
            mNoteLabel->SetText("note: learning" B_UTF8_ELLIPSIS);
        } else {
            double norm = mController->getParamNormalized((Vst::ParamID)(kSlotNoteBase + slot));
            int note = (int)(norm * (double)kNoteUnassigned + 0.5);
            char name[32];
            noteName(note >= kNoteUnassigned ? -1 : note, name, sizeof(name));
            std::snprintf(text, sizeof(text), "note: %s", name);
            mNoteLabel->SetText(text);
        }

        double vol = mController->getParamNormalized((Vst::ParamID)(kSlotVolumeBase + slot));
        int32 v = (int32)(vol * kSliderScale + 0.5);
        if (mVolSlider->Value() != v)
            mVolSlider->SetValue(v);
    }

    void Compose(BView *v)
    {
        v->SetDrawingMode(B_OP_COPY);
        if (mBackground) {
            v->DrawBitmap(mBackground, B_ORIGIN);
        } else {
            v->SetHighColor(20, 23, 28);
            v->FillRect(v->Bounds());
            v->SetHighColor(kStripBg);
            v->FillRect(BRect(0, geo::kStripY, v->Bounds().right, v->Bounds().bottom));
        }

        for (int i = 0; i < geo::kPadCount; ++i) {
            const geo::PadSpec &p = geo::kPads[i];
            int f = familyIndex(p.sprite);
            const int side = 2 * (p.r + geo::kPadMargin);
            const BPoint origin(p.x - side / 2.0f, p.y - side / 2.0f);

            v->SetDrawingMode(B_OP_ALPHA);
            v->SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_COMPOSITE);
            if (f >= 0 && mBase[f]) {
                v->DrawBitmap(mBase[f], origin);
            } else {
                drawFlatPad(v, p);
            }

            if (mHitLevel[i] > 0.0f && f >= 0 && mHit[f])
                drawWithAlpha(v, mHit[f], origin, mHitLevel[i], mHitScratch[f]);

            if (i == mSelected && f >= 0 && mSel[f])
                v->DrawBitmap(mSel[f], origin);

            if (geo::kPads[i].slot == mArmedSlot && f >= 0 && mArm[f]) {
                float pulse = 0.55f + 0.45f * std::sin((float)mPulsePhase * 0.35f);
                drawWithAlpha(v, mArm[f], origin, pulse, mArmScratch);
            }

            // Pad label.
            v->SetDrawingMode(B_OP_OVER);
            v->SetHighColor(i == mSelected ? kLabelColor : kDimColor);
            v->SetFontSize(11);
            float w = v->StringWidth(p.name);
            v->DrawString(p.name, BPoint(p.x - w / 2.0f, (float)p.y + 4.0f));
        }
    }

    void drawFlatPad(BView *v, const geo::PadSpec &p)
    {
        v->SetDrawingMode(B_OP_OVER);
        v->SetHighColor(kPadFallback);
        if (p.hex) {
            BPoint pts[6];
            for (int k = 0; k < 6; ++k) {
                float a = (float)(M_PI / 180.0 * (60.0 * k));
                pts[k].Set(p.x + p.r * std::cos(a), p.y + p.r * std::sin(a));
            }
            v->FillPolygon(pts, 6);
            v->SetHighColor(kRimFallback);
            v->StrokePolygon(pts, 6);
        } else {
            BRect r((float)(p.x - p.r), (float)(p.y - p.r), (float)(p.x + p.r), (float)(p.y + p.r));
            v->FillEllipse(r);
            v->SetHighColor(kRimFallback);
            v->StrokeEllipse(r);
        }
    }

    // Draw `src` with its per-pixel alpha scaled by `level` (0..1): copy the
    // pixels into a scratch bitmap and scale the alpha bytes. UI thread only;
    // scratch is (re)allocated lazily and cached by the caller.
    void drawWithAlpha(BView *v, const BBitmap *src, BPoint origin, float level, BBitmap *&scratch)
    {
        if (level >= 0.98f) {
            v->DrawBitmap(src, origin);
            return;
        }
        if (src->ColorSpace() != B_RGBA32 && src->ColorSpace() != B_RGB32) {
            if (level > 0.4f)
                v->DrawBitmap(src, origin); // cannot modulate; threshold instead
            return;
        }
        if (!scratch || scratch->Bounds() != src->Bounds() ||
            scratch->ColorSpace() != src->ColorSpace()) {
            delete scratch;
            scratch = new BBitmap(src->Bounds(), src->ColorSpace());
        }
        if (!scratch->IsValid())
            return;
        const uint8 *in = (const uint8 *)src->Bits();
        uint8 *out = (uint8 *)scratch->Bits();
        int32 len =
            src->BitsLength() < scratch->BitsLength() ? src->BitsLength() : scratch->BitsLength();
        uint16 mul = (uint16)(level * 256.0f);
        std::memcpy(out, in, (size_t)len);
        for (int32 o = 3; o < len; o += 4) // BGRA little-endian: alpha at byte 3
            out[o] = (uint8)((out[o] * mul) >> 8);
        v->DrawBitmap(scratch, origin);
    }

    DrumController *mController;

    BBitmap *mBackground = nullptr;
    const char *mFamilyKey[kMaxFamilies] = {};
    bool mFamilyLoaded[kMaxFamilies] = {};
    int mFamilyCount = 0;
    BBitmap *mBase[kMaxFamilies] = {};
    BBitmap *mHit[kMaxFamilies] = {};
    BBitmap *mSel[kMaxFamilies] = {};
    BBitmap *mArm[kMaxFamilies] = {};
    BBitmap *mHitScratch[kMaxFamilies] = {};
    BBitmap *mArmScratch = nullptr;

    BBitmap *mOff = nullptr;
    BView *mOffView = nullptr;

    BStringView *mPadLabel = nullptr;
    BStringView *mFileLabel = nullptr;
    BStringView *mNoteLabel = nullptr;
    BButton *mLoadButton = nullptr;
    BButton *mClearButton = nullptr;
    BButton *mLearnButton = nullptr;
    BSlider *mVolSlider = nullptr;
    BButton *mExpandButton = nullptr;
    DrumRackView *mRack = nullptr; // child view, built on first Expand
    BFilePanel *mFilePanel = nullptr;
    BMessageRunner *mTick = nullptr;

    int mSelected = 0;
    int mArmedSlot = -1;
    int mLoadSlot = 0;
    int mPulsePhase = 0;
    float mHitLevel[geo::kPadCount] = {};
};

//------------------------------------------------------------------------
// DrumEditorView
//------------------------------------------------------------------------

DrumEditorView::DrumEditorView(DrumController *controller) : HaikuPlugView(controller)
{
    Steinberg::ViewRect size(0, 0, geo::kWinW, geo::kWinH);
    setRect(size);
}

BView *DrumEditorView::createHaikuView(BRect frame)
{
    mKitView = new DrumKitView(frame, static_cast<DrumController *>(getController()));
    return mKitView;
}

void DrumEditorView::removedFromParent()
{
    mKitView = nullptr; // HaikuPlugView deletes the BView
    HaikuPlugView::removedFromParent();
}

void DrumEditorView::ParamChanged(Vst::ParamID id, Vst::ParamValue value)
{
    if (mKitView)
        mKitView->ParamChanged(id, value);
}

} // namespace DRUMku

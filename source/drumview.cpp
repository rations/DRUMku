// DRUMku native Haiku editor implementation.
//
// The drum-kit view: an electronic kit seen from the player's seat, composited
// from ImageMagick-generated PNG layers (gui/make_*.sh -> resource/gui/,
// loaded from the bundle's Contents/Resources/gui). Hex pads (kick, snare,
// toms) and round cymbal pads (hi-hat, crashes, ride) map to fixed slots 0-8;
// the control strip below drives the selected pad: Load/Clear .wav (via the
// controller's IDrumLoader), MIDI Learn (plug-in-side, fed back through the
// note parameter), and a volume slider. Pads flash on the hidden activity
// parameters and every sprite load is NULL-checked with a flat-color
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
const int32 kSliderScale = 1000; // BSlider is integer-valued; 0..1000 -> 0..1

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
                Vst::ParamID id = (Vst::ParamID)(kSlotVolumeBase + selectedSlot());
                Vst::ParamValue norm = (double)mVolSlider->Value() / (double)kSliderScale;
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
                mLoadSlot = selectedSlot();
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
                }
                break;
            }
            case kMsgClearBtn: {
                if (mController->setSampleFile(selectedSlot(), "") != kResultOk)
                    fprintf(stderr, "DRUMku: clearing the sample failed\n");
                RefreshStrip();
                break;
            }
            case kMsgLearnBtn: {
                int slot = selectedSlot();
                if (mArmedSlot == slot) {
                    mController->armLearn(-1); // toggle off
                    mArmedSlot = -1;
                } else if (mController->armLearn(slot) == kResultOk) {
                    mArmedSlot = slot;
                } else {
                    fprintf(stderr, "DRUMku: arming MIDI learn failed\n");
                }
                RefreshStrip();
                Invalidate();
                break;
            }
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
            return;
        }
        if (id >= (Vst::ParamID)kSlotVolumeBase &&
            id < (Vst::ParamID)(kSlotVolumeBase + kMaxSlots)) {
            if ((int)(id - kSlotVolumeBase) == selectedSlot()) {
                int32 v = (int32)(value * kSliderScale + 0.5);
                if (mVolSlider->Value() != v)
                    mVolSlider->SetValue(v);
            }
        }
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

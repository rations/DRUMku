// DRUMku voice engine implementation. See drumvoices.h for the threading and
// real-time contract.

#include "drumvoices.h"

#include <algorithm>
#include <utility>

namespace DRUMku {

DrumEngine::DrumEngine() = default;

static inline bool slot_in_range(int slot)
{
    return slot >= 0 && slot < kMaxSlots;
}

//------------------------------------------------------------------------
void DrumEngine::setup(double sampleRate, int maxBlockSize)
{
    mSampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    mMaxBlockSize = maxBlockSize > 0 ? maxBlockSize : 2048;

    // Re-resample every loaded sample to the (possibly new) engine rate.
    // Processing is inactive here, so staging + an immediate swap is safe.
    for (int i = 0; i < kMaxSlots; ++i) {
        if (!mSlots[i].path.empty()) {
            loadSample(i, mSlots[i].path);
        }
    }
    applyPendingSwaps();
}

//------------------------------------------------------------------------
bool DrumEngine::loadSample(int slot, const std::string &path)
{
    if (!slot_in_range(slot))
        return false;
    if (path.empty()) {
        clearSample(slot);
        return true;
    }
    auto sample = std::make_unique<DrumSample>();
    if (!wav_load_resampled(path, mSampleRate, *sample))
        return false;
    mSlots[slot].pending = std::move(sample); // frees any prior pending here
    mSlots[slot].path = path;
    mSlots[slot].hasPending.store(true, std::memory_order_release);
    return true;
}

//------------------------------------------------------------------------
void DrumEngine::clearSample(int slot)
{
    if (!slot_in_range(slot))
        return;
    mSlots[slot].pending.reset();
    mSlots[slot].path.clear();
    mSlots[slot].hasPending.store(true, std::memory_order_release);
}

const std::string &DrumEngine::slotPath(int slot) const
{
    return slot_in_range(slot) ? mSlots[slot].path : mEmpty;
}

//------------------------------------------------------------------------
void DrumEngine::setSlotVolume(int slot, float vol01)
{
    if (!slot_in_range(slot))
        return;
    if (vol01 < 0.0f)
        vol01 = 0.0f;
    if (vol01 > 1.0f)
        vol01 = 1.0f;
    mSlots[slot].volume.store(vol01, std::memory_order_relaxed);
}

void DrumEngine::setSlotNote(int slot, int note)
{
    if (!slot_in_range(slot))
        return;
    if (note < 0 || note > 127)
        note = -1;
    mSlots[slot].note.store(note, std::memory_order_relaxed);
}

float DrumEngine::slotVolume(int slot) const
{
    return slot_in_range(slot) ? mSlots[slot].volume.load(std::memory_order_relaxed) : 0.0f;
}

int DrumEngine::slotNote(int slot) const
{
    return slot_in_range(slot) ? mSlots[slot].note.load(std::memory_order_relaxed) : -1;
}

//------------------------------------------------------------------------
void DrumEngine::applyPendingSwaps()
{
    for (int i = 0; i < kMaxSlots; ++i) {
        if (mSlots[i].hasPending.exchange(false, std::memory_order_acq_rel)) {
            // Move the staged sample into `current`; the retired buffer moves
            // back into `pending` and is freed on the message thread later.
            std::swap(mSlots[i].current, mSlots[i].pending);
            mSlots[i].currentPtr.store(mSlots[i].current.get(), std::memory_order_release);
        }
    }
}

//------------------------------------------------------------------------
DrumEngine::Voice *DrumEngine::allocVoice()
{
    for (int i = 0; i < kMaxVoices; ++i) {
        if (!mVoices[i].active)
            return &mVoices[i];
    }
    // All busy: steal round-robin.
    Voice *v = &mVoices[mStealCursor];
    mStealCursor = (mStealCursor + 1) % kMaxVoices;
    return v;
}

//------------------------------------------------------------------------
void DrumEngine::noteOn(int pitch, float velocity, int sampleDelay)
{
    if (pitch < 0 || pitch > 127)
        return;
    if (velocity < 0.0f)
        velocity = 0.0f;
    if (velocity > 1.0f)
        velocity = 1.0f;

    // Trigger every slot bound to this pitch that has a loaded sample.
    for (int i = 0; i < kMaxSlots; ++i) {
        if (mSlots[i].note.load(std::memory_order_relaxed) != pitch)
            continue;
        if (mSlots[i].currentPtr.load(std::memory_order_acquire) == nullptr)
            continue;
        Voice *v = allocVoice();
        v->active = true;
        v->slot = i;
        v->pos = 0;
        v->delay = sampleDelay > 0 ? sampleDelay : 0;
        v->gain = velocity;
    }
}

//------------------------------------------------------------------------
void DrumEngine::allNotesOff()
{
    for (int i = 0; i < kMaxVoices; ++i)
        mVoices[i].active = false;
}

//------------------------------------------------------------------------
void DrumEngine::renderBlock(float *outL, float *outR, int numSamples)
{
    if (!outL || !outR || numSamples <= 0)
        return;

    for (int vi = 0; vi < kMaxVoices; ++vi) {
        Voice &v = mVoices[vi];
        if (!v.active)
            continue;

        DrumSample *s = mSlots[v.slot].currentPtr.load(std::memory_order_acquire);
        if (!s || v.pos >= s->frames) {
            v.active = false;
            continue;
        }

        const float vol = mSlots[v.slot].volume.load(std::memory_order_relaxed);
        const float g = v.gain * vol;
        const float *L = s->ch0.data();
        const float *R = (s->channels > 1 && !s->ch1.empty()) ? s->ch1.data() : L;
        const int frames = s->frames;

        int i = 0;
        // Onset delay within this block (sample-accurate trigger).
        if (v.delay > 0) {
            int skip = std::min(v.delay, numSamples);
            v.delay -= skip;
            i = skip;
        }
        for (; i < numSamples; ++i) {
            if (v.pos >= frames) {
                v.active = false;
                break;
            }
            outL[i] += L[v.pos] * g;
            outR[i] += R[v.pos] * g;
            ++v.pos;
        }
        if (v.pos >= frames)
            v.active = false;
    }
}

} // namespace DRUMku

// DRUMku voice engine: per-slot sample management plus a fixed voice pool.
//
// Real-time contract (see also RULES for the parent project):
//   - loadSample/clearSample/setup run on the message thread only; they parse
//     and resample the .wav and stage the result in a per-slot `pending` slot.
//   - process-side calls (applyPendingSwaps / noteOn / renderBlock) run on the
//     RT thread and never allocate, lock, or do I/O. A staged sample is made
//     live by an atomic pointer swap at block start; the retired buffer moves
//     into the `pending` slot and is freed on the message thread at the next
//     load — never on the RT thread.
//   - Voices reference a slot by index and re-read the slot's current buffer
//     each block, so a hot-swap can never leave a voice reading freed memory.

#pragma once

#include "idrumloader.h" // kMaxSlots
#include "wavsample.h"

#include <atomic>
#include <memory>
#include <string>

namespace DRUMku {

class DrumEngine
{
public:
    static constexpr int kMaxVoices = 64;

    DrumEngine();

    // ---- message thread (processing inactive for setup) ----
    void setup(double sampleRate, int maxBlockSize);
    double sampleRate() const { return mSampleRate; }

    // Load / clear the sample for a slot; returns false if the file cannot be
    // read or parsed (the slot is left unchanged in that case).
    bool loadSample(int slot, const std::string &path);
    void clearSample(int slot);
    const std::string &slotPath(int slot) const;

    // Parameter mirrors (also driven from RT param handling — the stored values
    // are plain atomics, so either thread may set them).
    void setSlotVolume(int slot, float vol01);
    void setSlotNote(int slot, int note); // 0..127, or < 0 = unassigned
    float slotVolume(int slot) const;
    int slotNote(int slot) const;

    // ---- RT thread ----
    void applyPendingSwaps();
    void noteOn(int pitch, float velocity, int sampleDelay);
    void allNotesOff();
    void renderBlock(float *outL, float *outR, int numSamples); // adds into out

private:
    struct Slot {
        std::unique_ptr<DrumSample> current; // touched on RT only (via swap)
        std::unique_ptr<DrumSample> pending; // staged on message thread
        std::atomic<bool> hasPending{false};
        std::atomic<DrumSample *> currentPtr{nullptr}; // what RT voices read
        std::atomic<int> note{-1};
        std::atomic<float> volume{0.8f};
        std::string path; // message thread only
    };

    struct Voice {
        bool active = false;
        int slot = -1;
        int pos = 0;
        int delay = 0;
        float gain = 0.0f; // velocity
    };

    Voice *allocVoice();

    Slot mSlots[kMaxSlots];
    Voice mVoices[kMaxVoices];
    int mStealCursor = 0;
    double mSampleRate = 48000.0;
    int mMaxBlockSize = 2048;
    std::string mEmpty;
};

} // namespace DRUMku

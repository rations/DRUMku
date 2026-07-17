// DRUMku editor geometry — mirrored BY HAND from gui/geometry.sh (the single
// source of truth for the art pipeline). Keep the two files in sync: every
// value here must match its shell counterpart, and the sprite base names must
// match what gui/make_pads.sh emits into resource/gui/.

#pragma once

namespace DRUMku
{
namespace geo
{

// Editor canvas (gui/geometry.sh: WIN_W/WIN_H).
constexpr int kWinW = 760;
constexpr int kWinH = 480;

// Control strip (STRIP_Y/STRIP_H).
constexpr int kStripY = 356;
constexpr int kStripH = 124;

// Transparent margin baked into every pad sprite (PAD_MARGIN): sprite side is
// 2 * (radius + kPadMargin), centered on the pad center.
constexpr int kPadMargin = 14;

// One pad of the kit. `sprite` selects the sprite family emitted by
// make_pads.sh (pad_<sprite>_{base,hit,sel,arm}.png); `slot` is the plug-in
// slot the pad drives (fixed mapping, matches the parameter layout).
struct PadSpec {
    int slot;
    bool hex; // hexagonal pad (true) or round cymbal pad (false)
    int x, y; // center, canvas coordinates
    int r;    // hex circumradius / circle radius
    const char *name;
    const char *sprite;
};

constexpr int kPadCount = 9;
constexpr PadSpec kPads[kPadCount] = {
    {0, true, 365, 245, 62, "Kick", "hex62"},     // KICK_*
    {1, true, 215, 245, 54, "Snare", "hex54"},    // SNARE_*
    {2, true, 295, 115, 48, "Tom 1", "hex48"},    // TOM1_*
    {3, true, 435, 115, 48, "Tom 2", "hex48"},    // TOM2_*
    {4, true, 520, 270, 54, "Floor Tom", "hex54"} // FLOOR_*
    ,
    {5, false, 88, 225, 46, "Hi-Hat", "cym46"},  // HIHAT_*
    {6, false, 105, 95, 52, "Crash 1", "cym52"}, // CRASH1_*
    {7, false, 585, 85, 52, "Crash 2", "cym52"}, // CRASH2_*
    {8, false, 650, 215, 58, "Ride", "cym58"},   // RIDE_*
};

} // namespace geo
} // namespace DRUMku

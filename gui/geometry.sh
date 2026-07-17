# Master geometry for the DRUMku kit art. Sourced by every make_*.sh; the
# same numbers are mirrored as constants in source/drumgeometry.h. All values
# are in FINAL (1x) pixels; scripts render at 2x and downscale for crispness.
#
# Layout: an electronic drum kit seen from the player's seat (after the
# reference photo drum-kit-idea.png): hexagonal rubber pads for kick, snare,
# two rack toms and floor tom; round ridged pads for hi-hat, two crashes and
# the ride. The bottom strip hosts the per-pad controls (Load/Learn/volume).
#
# Slot map (fixed, matches the plug-in's parameter layout):
#   slot 0 kick   1 snare   2 tom1   3 tom2   4 floor
#   slot 5 hihat  6 crash1  7 crash2 8 ride

# Editor canvas.
WIN_W=760
WIN_H=480

# Kit area is everything above the control strip.
STRIP_Y=356
STRIP_H=124

# Pad sprite padding: transparent margin around the shape so glow/ring
# overlays register pixel-perfect on the same canvas. Sprite side is
# 2 * (R + PAD_MARGIN).
PAD_MARGIN=14

# Pad centers and radii (hexes: circumradius, flat-top orientation).
KICK_X=365;   KICK_Y=245;   KICK_R=62
SNARE_X=215;  SNARE_Y=245;  SNARE_R=54
TOM1_X=295;   TOM1_Y=115;   TOM1_R=48
TOM2_X=435;   TOM2_Y=115;   TOM2_R=48
FLOOR_X=520;  FLOOR_Y=270;  FLOOR_R=54
HIHAT_X=88;   HIHAT_Y=225;  HIHAT_R=46
CRASH1_X=105; CRASH1_Y=95;  CRASH1_R=52
CRASH2_X=585; CRASH2_Y=85;  CRASH2_R=52
RIDE_X=650;   RIDE_Y=215;   RIDE_R=58

# Bass-drum shell drawn in the background under the kick pad.
BASSDRUM_X=365
BASSDRUM_Y=258
BASSDRUM_R=92

# Palette.
BG_TOP='#14171c'
BG_BOTTOM='#1e232a'
STRIP_BG='#0c0e11'
SEPARATOR='#2f343b'
TUBE='#2a2e35'
PAD_BODY='#181a1f'
PAD_BODY_LIGHT='#23262c'
PAD_RIM='#3a3f46'
CYM_BODY='#101317'
CYM_GROOVE='#000000'
CYM_BELL='#2c3138'
HIT_GLOW='#ffd24a'
SEL_RING='#4aa8ff'
ARM_RING='#ff4a3a'

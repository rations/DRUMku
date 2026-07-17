#!/usr/bin/env bash
# One-shot: DRUMku editor background. Dark stage backdrop with a vignette,
# subtle rack tubes, the bass-drum shell under the kick pad, and the control
# strip area at the bottom. Pad sprites are composited over this by the view.
# Rendered at 2x and downscaled. Run on the dev host; commit the output.
set -euo pipefail
cd "$(dirname "$0")"
. ./geometry.sh

OUT=../resource/gui
mkdir -p "$OUT"

W=$((WIN_W * 2))
H=$((WIN_H * 2))
SY=$((STRIP_Y * 2))

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# Base: vertical gradient with a radial vignette.
magick -size "${W}x${H}" "gradient:${BG_TOP}-${BG_BOTTOM}" "$tmp/base.png"
magick -size "${W}x${H}" radial-gradient:none-black \
    -channel A -evaluate multiply 0.5 +channel "$tmp/vignette.png"

# Rack tubes: two horizontal bars and four posts, low contrast.
magick -size "${W}x${H}" xc:none -stroke "$TUBE" -strokewidth 8 -fill none \
    -draw "stroke-opacity 0.8 line $((70 * 2)),$((105 * 2)) $((690 * 2)),$((105 * 2))" \
    -draw "stroke-opacity 0.8 line $((60 * 2)),$((250 * 2)) $((150 * 2)),$((250 * 2))" \
    -draw "stroke-opacity 0.8 line $((95 * 2)),$((105 * 2)) $((95 * 2)),$((340 * 2))" \
    -draw "stroke-opacity 0.8 line $((295 * 2)),$((105 * 2)) $((295 * 2)),$((170 * 2))" \
    -draw "stroke-opacity 0.8 line $((435 * 2)),$((105 * 2)) $((435 * 2)),$((170 * 2))" \
    -draw "stroke-opacity 0.8 line $((660 * 2)),$((105 * 2)) $((660 * 2)),$((340 * 2))" \
    -blur 0x1 "$tmp/tubes.png"

# Bass-drum shell under the kick pad: dark disc with a lighter hoop.
BX=$((BASSDRUM_X * 2))
BY=$((BASSDRUM_Y * 2))
BR=$((BASSDRUM_R * 2))
magick -size "${W}x${H}" xc:none \
    -fill '#0e1013' -draw "circle $BX,$BY $((BX + BR)),$BY" \
    -fill none -stroke "$PAD_RIM" -strokewidth 6 \
    -draw "circle $BX,$BY $((BX + BR)),$BY" \
    -stroke '#23262c' -strokewidth 3 \
    -draw "circle $BX,$BY $((BX + BR - 12)),$BY" \
    "$tmp/bassdrum.png"

# Control strip: darker area with a separator line.
magick -size "${W}x${H}" xc:none \
    -fill "$STRIP_BG" -draw "rectangle 0,$SY $W,$H" \
    -fill none -stroke "$SEPARATOR" -strokewidth 2 \
    -draw "line 0,$SY $W,$SY" \
    "$tmp/strip.png"

magick "$tmp/base.png" \
    "$tmp/vignette.png" -compose over -composite \
    "$tmp/tubes.png" -compose over -composite \
    "$tmp/bassdrum.png" -compose over -composite \
    "$tmp/strip.png" -compose over -composite \
    -resize 50% -depth 8 "PNG32:$OUT/background.png"

identify "$OUT/background.png"

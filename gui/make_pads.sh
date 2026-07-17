#!/usr/bin/env bash
# One-shot: pad sprites for the DRUMku kit view. For every pad size two
# shapes (flat-top hexagon "hex", round cymbal pad "cym") and four layers:
#   *_base.png  the pad itself (rubber body + rim)
#   *_hit.png   additive glow overlay, drawn while the pad fires
#   *_sel.png   selection ring overlay (pad whose controls are shown)
#   *_arm.png   MIDI-learn armed ring overlay
# Rendered at 2x and downscaled; padded canvas (PAD_MARGIN) so all layers of
# one pad register on the same rect. Run on the dev host; commit the outputs
# to ../resource/gui/.
set -euo pipefail
cd "$(dirname "$0")"
. ./geometry.sh

OUT=../resource/gui
mkdir -p "$OUT"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# Flat-top hexagon polygon points (2x canvas coords): center c, radius r.
hexpoints() { # c r
    awk -v c="$1" -v r="$2" 'BEGIN {
        pi = atan2(0, -1);
        for (i = 0; i < 6; i++) {
            a = pi / 180 * (60 * i);
            printf "%s%.2f,%.2f", (i ? " " : ""), c + r * cos(a), c + r * sin(a);
        }
    }'
}

make_hex() { # R
    local R=$1
    local S=$(((R + PAD_MARGIN) * 4)) # 2x canvas side
    local C=$((S / 2))
    local R2=$((R * 2))
    local pts_outer pts_rim pts_inner
    pts_outer=$(hexpoints "$C" "$R2")
    pts_rim=$(hexpoints "$C" $((R2 - 8)))
    pts_inner=$(hexpoints "$C" $((R2 - 26)))

    # Shape mask.
    magick -size "${S}x${S}" xc:none -fill white -draw "polygon $pts_outer" "$tmp/mask.png"

    # Base: soft drop shadow, rim, rubber body with a top-lit gradient, inner line.
    magick -size "${S}x${S}" xc:none \
        \( "$tmp/mask.png" -channel A -evaluate multiply 0.6 +channel -blur 0x10 \
           -channel RGB -evaluate set 0 +channel -roll +0+8 \) -compose over -composite \
        \( -size "${S}x${S}" xc:none -fill "$PAD_RIM" -draw "polygon $pts_outer" \) -composite \
        \( -size "${S}x${S}" "gradient:${PAD_BODY_LIGHT}-${PAD_BODY}" \
           \( -size "${S}x${S}" xc:none -fill white -draw "polygon $pts_rim" \) \
           -alpha off -compose CopyOpacity -composite \) -compose over -composite \
        \( -size "${S}x${S}" xc:none -fill none -stroke "$PAD_RIM" -strokewidth 3 \
           -draw "polygon $pts_inner" \) -composite \
        -resize 50% -depth 8 "PNG32:$OUT/pad_hex${R}_base.png"

    overlays hex "$R" "$S" "polygon $pts_outer"
}

make_cym() { # R
    local R=$1
    local S=$(((R + PAD_MARGIN) * 4))
    local C=$((S / 2))
    local R2=$((R * 2))

    magick -size "${S}x${S}" xc:none -fill white \
        -draw "circle $C,$C $((C + R2)),$C" "$tmp/mask.png"

    # Grooved cymbal body: radial-lit disc, concentric grooves, center bell.
    local grooves=""
    local g
    for g in 45 57 69 81 92; do
        local gr=$((R2 * g / 100))
        grooves="$grooves circle $C,$C $((C + gr)),$C"
    done
    magick -size "${S}x${S}" xc:none \
        \( "$tmp/mask.png" -channel A -evaluate multiply 0.6 +channel -blur 0x10 \
           -channel RGB -evaluate set 0 +channel -roll +0+8 \) -compose over -composite \
        \( -size "${S}x${S}" "radial-gradient:${PAD_BODY_LIGHT}-${CYM_BODY}" \
           "$tmp/mask.png" -alpha off -compose CopyOpacity -composite \) -compose over -composite \
        \( -size "${S}x${S}" xc:none -fill none -stroke "$CYM_GROOVE" -strokewidth 2 \
           -draw "stroke-opacity 0.45 $grooves" \) -composite \
        \( -size "${S}x${S}" xc:none -fill "$CYM_BELL" \
           -draw "circle $C,$C $((C + R2 * 18 / 100)),$C" \) -composite \
        \( -size "${S}x${S}" xc:none -fill none -stroke "$PAD_RIM" -strokewidth 3 \
           -draw "circle $C,$C $((C + R2)),$C" \) -composite \
        -resize 50% -depth 8 "PNG32:$OUT/pad_cym${R}_base.png"

    overlays cym "$R" "$S" "circle $C,$C $((C + R2)),$C"
}

overlays() { # shape R S drawcmd
    local shape=$1 R=$2 S=$3 draw=$4

    # Hit glow: blurred warm halo from the shape mask plus a hotter core.
    magick -size "${S}x${S}" xc:none -fill white -draw "$draw" \
        -channel A -evaluate multiply 1.0 +channel "$tmp/hitmask.png"
    magick -size "${S}x${S}" "xc:${HIT_GLOW}" \
        \( "$tmp/hitmask.png" -alpha extract -blur 0x14 \) \
        -alpha off -compose CopyOpacity -composite \
        -channel A -evaluate multiply 0.9 +channel "$tmp/halo.png"
    magick -size "${S}x${S}" xc:white \
        \( "$tmp/hitmask.png" -alpha extract -morphology Erode Disk:10 -blur 0x18 \) \
        -alpha off -compose CopyOpacity -composite \
        -channel A -evaluate multiply 0.55 +channel "$tmp/core.png"
    magick "$tmp/halo.png" "$tmp/core.png" -compose over -composite \
        -resize 50% -depth 8 "PNG32:$OUT/pad_${shape}${R}_hit.png"

    # Selection / learn rings: stroked outline plus a soft glow of itself.
    ring "$SEL_RING" "$S" "$draw" "$OUT/pad_${shape}${R}_sel.png"
    ring "$ARM_RING" "$S" "$draw" "$OUT/pad_${shape}${R}_arm.png"
}

ring() { # color S drawcmd outfile
    local color=$1 S=$2 draw=$3 out=$4
    magick -size "${S}x${S}" xc:none -fill none -stroke "$color" -strokewidth 6 \
        -draw "$draw" "$tmp/ring.png"
    magick "$tmp/ring.png" \( +clone -blur 0x6 \) +swap -compose over -composite \
        -resize 50% -depth 8 "PNG32:$out"
}

make_hex "$KICK_R"
make_hex "$SNARE_R"   # also floor tom (same radius)
make_hex "$TOM1_R"    # also tom2 (same radius)
make_cym "$RIDE_R"
make_cym "$CRASH1_R"  # also crash2 (same radius)
make_cym "$HIHAT_R"

identify "$OUT"/pad_*_base.png

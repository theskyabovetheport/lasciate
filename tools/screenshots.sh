#!/bin/sh
# Capture the auth dialog, per theme, under a throwaway X server.
#
# Needs xvfb and xdotool alongside the things lasciate already needs. Nothing
# here touches the display you are sitting at: it starts its own server on a
# spare number and every command names it explicitly.
#
# The saver is told not to blank, because the screen going black five seconds
# in is exactly right for a lock screen and useless for a picture of one.
#
# Each capture is trimmed to what was drawn and given a margin in the theme's
# own background, so the image is the dialog rather than a screenful of unlit
# pixels around it.

set -eu

out=${1:-img}
disp=${DISPLAY_NUM:-:97}
size=${SIZE:-1100x420x24}
themes=${THEMES:-"inferno matrix moria"}
lasciate=${LASCIATE:-lasciate}

for b in Xvfb xdotool import setsid "$lasciate"; do
    command -v "$b" >/dev/null 2>&1 || { echo "need $b"; exit 1; }
done

mkdir -p "$out"
Xvfb "$disp" -screen 0 "$size" >/dev/null 2>&1 &
xvfb=$!
trap 'kill $xvfb 2>/dev/null || true' EXIT INT TERM
sleep 1

shoot() {
    theme=$1; typed=$2

    # setsid, so what is started here is a process group of its own and can be
    # ended by that group. Matching on the name would reach a lock screen this
    # script did not start -- including a real one, on the display you are
    # sitting at.
    setsid env DISPLAY="$disp" LASCIATE_THEME="$theme" \
        XSECURELOCK_BLANK_TIMEOUT=3600 XSECURELOCK_SAVER=saver_blank \
        "$lasciate" >/dev/null 2>&1 &
    lock=$!
    sleep 2

    # A keystroke is what raises the auth dialog; the rest fill the feedback
    # line, which is a fixed count of glyphs however much is typed.
    DISPLAY="$disp" xdotool type --delay 60 "$typed" || true

    DISPLAY="$disp" import -window root "$out/$theme.png"
    kill -- "-$lock" 2>/dev/null || kill "$lock" 2>/dev/null || true
    sleep 1

    bg=$( . "themes/$theme" 2>/dev/null; printf '%s' "${XSECURELOCK_BACKGROUND_COLOR:-black}" )
    convert "$out/$theme.png" -trim +repage \
        -bordercolor "$bg" -border 40x28 "$out/$theme.png"
    printf '  %-16s %s\n' "$out/$theme.png" "$(identify -format '%wx%h' "$out/$theme.png")"
}

for t in $themes; do
    shoot "$t" "lasciate"
done

echo
echo "done. the images are in $out/"

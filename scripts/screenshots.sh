#!/bin/sh
# Takes the documentation pictures.
#
# Against a throwaway daemon on a fake control surface, never against the one
# running the machine. Three reasons, and all three are about the pictures being
# documentation rather than a snapshot of somebody's afternoon:
#
#   - the readings are the same every time, so regenerating them is not a diff
#     of temperatures;
#   - nothing from a real machine ends up in a public repository;
#   - the daemon answers every command the console asks about, including the
#     saved definitions, which an older installed build would not.
#
# The throwaway daemon owns nothing: its sysfs root is a directory of ordinary
# files, so it writes to those and never to the hardware.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
build=${BUILD_DIR:-$root/build}
work=/tmp/ombra-screenshots
socket=$work/control.sock

for binary in "$build/ombrad" "$build/ombra-tray" "$build/tests/ombra-fake-tree"; do
    if [ ! -x "$binary" ]; then
        echo "$binary is missing — run 'make' first" >&2
        exit 1
    fi
done

rm -rf "$work"
mkdir -p "$work"
"$build/tests/ombra-fake-tree" "$work/sys" >/dev/null

# --no-dbus, because claiming the real names would disturb the desktop this is
# being run on.
"$build/ombrad" --no-dbus --root "$work/sys" --socket "$socket" \
    --state "$work/state" --config "$work/settings.conf" \
    --ring "$work/metrics.ring" --capacity 64 --interval-ms 200 &
daemon=$!
trap 'kill "$daemon" 2>/dev/null || true; rm -rf "$work"' EXIT INT TERM

waited=0
while [ ! -S "$socket" ]; do
    waited=$((waited + 1))
    if [ "$waited" -gt 100 ]; then
        echo "the throwaway daemon never came up" >&2
        exit 1
    fi
    sleep 0.1
done

# A socket answers before the first tick has taken any readings, and a console
# photographed in that instant documents a machine with no temperature.
waited=0
while ! "$build/ombra" --socket "$socket" status 2>/dev/null | grep -q '"pkg_c":[0-9]'; do
    waited=$((waited + 1))
    if [ "$waited" -gt 100 ]; then
        echo "the throwaway daemon never took a reading" >&2
        exit 1
    fi
    sleep 0.1
done

# A live setting and a verdict, so the pictures show the console doing its job
# rather than sitting at its defaults: the indicators on the first tab exist to
# be seen.
"$build/ombra" --socket "$socket" set pl1=17 >/dev/null
"$build/ombra" --socket "$socket" mark good >/dev/null

# English, whatever the language of the machine taking the pictures: the
# documents around them are English, and a picture whose labels are not is a
# picture that documents somebody's locale. LANGUAGE alone is not enough, because
# Qt reads LC_ALL first.
#
# Offscreen and pointed at a session bus that does not exist: the console is a
# widget being photographed, and it must not register an icon in the panel of
# whoever is running this.
LC_ALL=C.UTF-8 LANGUAGE=en \
    QT_QPA_PLATFORM=offscreen \
    DBUS_SESSION_BUS_ADDRESS=unix:path=/nonexistent/ombra-screenshots-have-no-bus \
    "$build/ombra-tray" --socket "$socket" --screenshot "$root/screenshots"

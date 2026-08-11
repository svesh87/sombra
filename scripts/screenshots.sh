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
work=/tmp/sombra-screenshots
# Decided here rather than printed by the bus, so that it is known before the bus
# exists and can be handed to everything that has to reach it.
address=unix:path=$work/bus

for binary in "$build/sombrad" "$build/sombra-tray" "$build/tests/sombra-fake-tree" \
              "$build/tests/sombra-fake-authority"; do
    if [ ! -x "$binary" ]; then
        echo "$binary is missing — run 'make' first" >&2
        exit 1
    fi
done

rm -rf "$work"
mkdir -p "$work"
"$build/tests/sombra-fake-tree" "$work/sys" >/dev/null

# A bus of this harness's own, and something on it to answer for polkit: the daemon
# checks every command against an action, and on a private bus there is no polkit to
# ask. The system bus is not an option — claiming the daemon's name there would
# replace the service running the machine.
dbus-daemon --session --nofork --address="$address" &
bus=$!
trap 'kill "$bus" "${authority:-}" "${daemon:-}" 2>/dev/null || true; rm -rf "$work"' \
    EXIT INT TERM

waited=0
while [ ! -S "$work/bus" ]; do
    waited=$((waited + 1))
    if [ "$waited" -gt 100 ]; then
        echo "the private bus never came up" >&2
        exit 1
    fi
    sleep 0.1
done

"$build/tests/sombra-fake-authority" "$address" >/dev/null &
authority=$!

# --no-dbus, because claiming the desktop's power-profile names would disturb the
# desktop this is being run on. The daemon's own name goes on the private bus.
"$build/sombrad" --no-dbus --root "$work/sys" --bus-address "$address" \
    --state "$work/state" --config "$work/settings.conf" \
    --ring "$work/metrics.ring" --capacity 64 --interval-ms 200 &
daemon=$!

# The name, not the process: a daemon that started and claimed nothing looks exactly
# like a slow one otherwise.
waited=0
while ! dbus-send --bus="$address" --print-reply --dest=org.freedesktop.DBus \
    /org/freedesktop/DBus org.freedesktop.DBus.GetNameOwner \
    string:io.github.svesh87.Sombra >/dev/null 2>&1; do
    waited=$((waited + 1))
    if [ "$waited" -gt 100 ]; then
        echo "the throwaway daemon never claimed its name" >&2
        exit 1
    fi
    sleep 0.1
done

# The name is claimed before the first tick has taken any readings, and a console
# photographed in that instant documents a machine with no temperature. The tick
# counter is what to wait for rather than a reading: with no sample taken yet the
# reply carries a temperature of zero, which any test on the digits accepts.
waited=0
while ! "$build/sombra" --bus-address "$address" status 2>/dev/null \
    | grep -q '"ticks":[1-9]'; do
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
"$build/sombra" --bus-address "$address" set pl1=17 >/dev/null
"$build/sombra" --bus-address "$address" mark good >/dev/null

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
    DBUS_SESSION_BUS_ADDRESS=unix:path=/nonexistent/sombra-screenshots-have-no-bus \
    "$build/sombra-tray" --bus-address "$address" --screenshot "$root/docs/screenshots"

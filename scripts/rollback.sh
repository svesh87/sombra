#!/bin/sh
# Gives the thermal controls back to the vendor daemon.
#
# This is the escape hatch the design promises: if the modes here turn out
# worse than the vendor's, no code is involved. By default it hands ownership
# back and leaves the package and the recorded telemetry alone, because the
# telemetry is the material the modes are tuned from and a bad afternoon is not
# a reason to throw it away.
#
#   scripts/rollback.sh              hand the controls back
#   scripts/rollback.sh --remove     ... and uninstall, keeping the telemetry
#   scripts/rollback.sh --purge      ... and delete the recorded telemetry too
set -eu

VENDOR_UNIT=com.system76.PowerDaemon.service
OUR_UNIT=cpu-throttlerd.service
FAN_NODES='/sys/devices/platform/asus-nb-wmi/hwmon/hwmon*/pwm1_enable'

self=$(cd "$(dirname "$0")" && pwd)/$(basename "$0")

mode=keep
for argument in "$@"; do
    case "$argument" in
    --remove) mode=remove ;;
    --purge) mode=purge ;;
    *)
        echo "usage: $(basename "$0") [--remove | --purge]" >&2
        exit 2
        ;;
    esac
done

if [ "$(id -u)" -ne 0 ]; then
    exec sudo "$self" "$@"
fi

echo "==> stopping the daemon"
systemctl disable --now "$OUR_UNIT" 2>/dev/null || true

# The daemon restores automatic fan control on every exit path it controls, and
# the unit's ExecStopPost covers the one it does not. A fan left at full speed
# by a stopped daemon is a defect, so it is checked rather than assumed.
for node in $FAN_NODES; do
    [ -e "$node" ] || continue
    value=$(cat "$node" 2>/dev/null || echo "?")
    if [ "$value" = 2 ]; then
        echo "    fan control is back to automatic"
    else
        echo "    fan control reads '$value', forcing it back to automatic" >&2
        echo 2 >"$node" || true
    fi
done

echo "==> handing the thermal controls back"
# Checked on disk rather than through systemctl: the answers systemctl gives
# for a unit that is present-but-masked and one that is absent overlap.
if [ -e "/usr/lib/systemd/system/$VENDOR_UNIT" ] || [ -e "/lib/systemd/system/$VENDOR_UNIT" ]; then
    systemctl unmask "$VENDOR_UNIT" || true
    systemctl enable --now "$VENDOR_UNIT"
else
    echo "    (no $VENDOR_UNIT on this system — nothing owns the controls now)"
fi

case "$mode" in
remove)
    echo "==> removing the package, keeping /var/lib/cpu_throttler"
    dpkg -r cpu-throttler || true
    ;;
purge)
    echo "==> purging the package AND the recorded telemetry"
    dpkg -P cpu-throttler || true
    ;;
esac

echo
echo "Power limits and platform profile are whatever the vendor daemon now sets;"
echo "every sysfs write is lost on reboot in any case."

#!/bin/sh
# Builds the package, installs it and hands the thermal controls over.
#
# This is also the upgrade path: every step is idempotent, and the daemon is
# restarted rather than merely enabled, because `systemctl enable --now` on a
# unit that is already running does nothing and would leave the old binary in
# memory after a rebuild. Run it again after any change and it picks the new
# one up.
#
# The package itself deliberately does none of the handing over: installing
# software and giving it exclusive ownership of the machine's power controls
# are different decisions, and `dpkg -i` must not make the second one. This
# script is where the second decision is expressed, which is why it is a
# separate, obvious step rather than a postinst.
#
# Run it as an ordinary user; it builds first and elevates only for the parts
# that need root. Reverse it with scripts/rollback.sh.
set -eu

VENDOR_UNIT=com.system76.PowerDaemon.service
OUR_UNIT=ombrad.service
# The hardware this project was built and measured on. Every sysfs path it
# writes was verified here and nowhere else.
SUPPORTED_MODEL=UX3405

self=$(cd "$(dirname "$0")" && pwd)/$(basename "$0")
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

force=no
for argument in "$@"; do
    case "$argument" in
    --force) force=yes ;;
    *)
        echo "usage: $(basename "$0") [--force]" >&2
        exit 2
        ;;
    esac
done

if [ "$(id -u)" -ne 0 ]; then
    model=$(cat /sys/class/dmi/id/product_name 2>/dev/null || echo unknown)
    case "$model" in
    *"$SUPPORTED_MODEL"*) ;;
    *)
        echo "This machine reports '$model'. Everything this daemon writes was" >&2
        echo "verified on an ASUS Zenbook 14 $SUPPORTED_MODEL and nowhere else;" >&2
        echo "other models expose a different control surface." >&2
        if [ "$force" != yes ]; then
            echo "Re-run with --force if you know what you are doing." >&2
            exit 1
        fi
        echo "Continuing because --force was given." >&2
        ;;
    esac

    # Built as the invoking user on purpose: a build under sudo leaves
    # root-owned objects behind in the source tree.
    echo "==> building and checking the package"
    "$root/scripts/check_package.sh"

    echo "==> elevating for the install"
    exec sudo "$self" "$@"
fi

deb=$(ls "$root"/build-package/ombra_*.deb 2>/dev/null | head -n 1)
if [ -z "$deb" ]; then
    echo "no package in build-package/ — run scripts/check_package.sh first" >&2
    exit 1
fi

echo "==> installing $(basename "$deb")"
dpkg -i "$deb"

# Stop before masking: masking a running unit does not stop it, and two owners
# of one control surface is the single thing this architecture forbids. The
# real unit name is used rather than the system76-power.service alias, because
# masking an alias leaves the real unit enabled and it returns at the next boot.
echo "==> handing the thermal controls over"
# Checked on disk rather than through systemctl: the answers systemctl gives
# for a unit that is present-but-masked and one that is absent overlap.
if [ -e "/usr/lib/systemd/system/$VENDOR_UNIT" ] || [ -e "/lib/systemd/system/$VENDOR_UNIT" ]; then
    systemctl disable --now "$VENDOR_UNIT" || true
    systemctl mask "$VENDOR_UNIT"
else
    echo "    (no $VENDOR_UNIT on this system, nothing to hand over)"
fi

# enable, then restart rather than `enable --now`: on a re-run the unit is
# already active and `--now` would leave the previous binary running.
systemctl enable "$OUR_UNIT"
systemctl restart "$OUR_UNIT"

# The tray runs as a person, not as root, and reaches the daemon over a socket
# owned by this group.
user=${SUDO_USER:-}
if [ -n "$user" ] && [ "$user" != root ]; then
    if ! id -nG "$user" | tr ' ' '\n' | grep -qx ombra; then
        usermod -aG ombra "$user"
        echo "==> added $user to the ombra group — log out and back in for the tray"
    fi
fi

echo
systemctl --no-pager --lines=0 status "$OUR_UNIT" || true
echo
echo "==> what the daemon says"
ombra status || true
echo

# The tray is a separate process running as a person; a rebuild does not reach
# it, and nothing here is going to kill somebody's window without asking.
if pgrep -x ombra-tray >/dev/null 2>&1; then
    echo "The tray is still running the previous build — restart it to pick this one up."
fi

echo "Watch for a while, then:"
echo "  ombra status                readings and setpoints"
echo "  ombra export --format csv   the recorded session"
echo "  scripts/rollback.sh         give the controls back"

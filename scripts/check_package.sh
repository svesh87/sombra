#!/bin/sh
# Builds the deb and checks what came out of it.
#
# Everything here works without root and without installing anything: the
# package is inspected with `dpkg-deb`, never unpacked onto the system. That is
# what lets this run as an ordinary test rather than as a manual step, and a
# packaging mistake is exactly the kind that is only ever found by looking.
#
# The source tree is copied into the build directory first, because
# dpkg-buildpackage writes its artefacts to the parent of the directory it
# builds in. Running it in place would drop them beside the repository.
set -eu

# dpkg translates its own prose, and this script reads that output.
LC_ALL=C
export LC_ALL

root=$(dirname "$0")/..
cd "$root"
root=$(pwd)

builddir=${1:-$root/build-package}
stage=$builddir/source
log=$builddir/build.log

status=0
report() {
    echo "package: $1" >&2
    status=1
}

rm -rf "$builddir"
mkdir -p "$stage"
tar -C "$root" --exclude=./.git --exclude=./tmp --exclude='./build*' -cf - . \
    | tar -C "$stage" -xf -

if ! (cd "$stage" && dpkg-buildpackage -b -us -uc) >"$log" 2>&1; then
    echo "package: dpkg-buildpackage failed" >&2
    tail -n 40 "$log" >&2
    exit 1
fi

deb=$(ls "$builddir"/sombra_*.deb 2>/dev/null | head -n 1)
if [ -z "$deb" ]; then
    echo "package: the build produced no .deb" >&2
    exit 1
fi

contents=$(dpkg-deb -c "$deb")
control=$(dpkg-deb -I "$deb")

# The package version and the project version are the same number kept in two
# places, and the whole point of a version is that it identifies a build.
project_version=$(sed -n 's/^project(.*VERSION \([0-9.]*\).*/\1/p' "$root/CMakeLists.txt")
package_version=$(printf '%s\n' "$control" | sed -n 's/^ Version: //p')
if [ "$project_version" != "$package_version" ]; then
    report "version is $package_version, CMakeLists.txt says $project_version"
fi

# Mode: the field dpkg-deb prints first, e.g. -rwxr-xr-x.
expect_file() {
    path=$1
    mode=$2
    line=$(printf '%s\n' "$contents" | awk -v p=".$path" '$NF == p { print; exit }')
    if [ -z "$line" ]; then
        report "$path is missing from the package"
        return
    fi
    actual=$(printf '%s\n' "$line" | awk '{ print $1 }')
    if [ "$actual" != "$mode" ]; then
        report "$path is $actual, expected $mode"
    fi
}

expect_file /usr/bin/sombrad -rwxr-xr-x
expect_file /usr/bin/sombra -rwxr-xr-x
expect_file /usr/bin/sombra-tray -rwxr-xr-x
expect_file /usr/lib/systemd/system/sombrad.service -rw-r--r--
expect_file /usr/share/applications/sombra-tray.desktop -rw-r--r--
expect_file /usr/share/bash-completion/completions/sombra -rw-r--r--
expect_file /usr/share/zsh/vendor-completions/_sombra -rw-r--r--
expect_file /usr/share/fish/vendor_completions.d/sombra.fish -rw-r--r--
# Manual pages are compressed by debhelper, which is the one place in this
# package where that is the right trade: `man` decompresses them itself.
# The console's catalogues. Compiled from the .ts files at build time, so their
# absence here means the build step was dropped rather than that a translation is
# missing.
# The icon, and that the desktop entry asks for it by that exact name. The two
# drifting apart is invisible by eye and shows up as a launcher drawing a generic
# cog, which is what it did before this was checked.
expect_file /usr/share/icons/hicolor/scalable/apps/sombra.svg -rw-r--r--
icon_name=$(dpkg-deb --fsys-tarfile "$deb" \
    | tar -xO ./usr/share/applications/sombra-tray.desktop 2>/dev/null \
    | sed -n 's/^Icon=//p')
if [ "$icon_name" != "sombra" ]; then
    report "the desktop entry asks for icon '$icon_name', not 'sombra'"
fi

expect_file /usr/share/sombra/translations/sombra_ru.qm -rw-r--r--
expect_file /usr/share/sombra/translations/sombra_es.qm -rw-r--r--

# The page behind the console's "?" button, one per language the window speaks. A
# missing one is not a crash — the button says there is no page — which is exactly
# why nothing but this check would notice.
expect_file /usr/share/sombra/help/sombra-help.en.html -rw-r--r--
expect_file /usr/share/sombra/help/sombra-help.ru.html -rw-r--r--
expect_file /usr/share/sombra/help/sombra-help.es.html -rw-r--r--

expect_file /usr/share/man/man1/sombra.1.gz -rw-r--r--
expect_file /usr/share/man/man1/sombra-tray.1.gz -rw-r--r--
expect_file /usr/share/man/man8/sombrad.8.gz -rw-r--r--
expect_file /usr/share/doc/sombra/README.md -rw-r--r--
expect_file /usr/share/doc/sombra/HARDWARE.md -rw-r--r--
expect_file /usr/share/doc/sombra/copyright -rw-r--r--

# Nothing may land outside /usr. A stray path is how a package quietly starts
# owning files it has no business owning.
strays=$(printf '%s\n' "$contents" | awk '{ print $NF }' \
    | grep -v '/$' \
    | grep -v '^\./usr/' || true)
if [ -n "$strays" ]; then
    report "unexpected paths: $(printf '%s' "$strays" | tr '\n' ' ')"
fi

# An autostart entry belongs to the person, not to the package: dpkg -i must not
# add a process to a session, for the same reason it does not take the thermal
# controls. The tray writes its own entry when the switch in its window is
# thrown, and that path is under the person's own config directory.
autostarts=$(printf '%s\n' "$contents" | awk '{ print $NF }' | grep autostart || true)
if [ -n "$autostarts" ]; then
    report "the package ships an autostart entry: $(printf '%s' "$autostarts" | tr '\n' ' ')"
fi

# The daemon carries its defaults compiled in and writes its own settings under
# /var/lib/sombra. A conffile would be a second source of truth, and an upgrade
# would then argue with values the daemon itself had written.
conffiles=$(dpkg-deb -I "$deb" conffiles 2>/dev/null || true)
if [ -n "$(printf '%s' "$conffiles" | tr -d '[:space:]')" ]; then
    report "the package registers conffiles: $(printf '%s' "$conffiles" | tr '\n' ' ')"
fi

# The scripts the package is expected to carry. There is no preinst any more:
# the only thing it did was withdraw a conffile that this package never had.
for script in postinst prerm postrm; do
    if ! dpkg-deb -I "$deb" "$script" >/dev/null 2>&1; then
        report "$script is missing from the package"
    fi
done

# The daemon under its previous name must not be installable beside this one.
# Two owners of one fan is the arrangement the whole project is built to
# prevent, and this is the line that makes dpkg enforce it.
case "$control" in
*"Conflicts: ombra"*) ;;
*) report "the package does not conflict with the old ombra package" ;;
esac

# Installing must not start the daemon and must not enable it. Handing over the
# thermal controls is a deliberate act with a rollback procedure, and this is
# what keeps a dropped --no-enable or --no-start in debian/rules from turning
# it into a side effect of `dpkg -i`. Both are checked on the generated
# snippet rather than on debian/rules, because the snippet is what runs:
#   --no-start  leaves out deb-systemd-invoke, which is what starts a unit;
#   --no-enable wraps the enable call in a `debian-installed` guard, so a
#               fresh install enables nothing, while debhelper's default
#               template enables unconditionally.
postinst=$(dpkg-deb -I "$deb" postinst)
case "$postinst" in
*deb-systemd-invoke*) report "postinst starts the unit" ;;
esac
case "$postinst" in
*"deb-systemd-helper enable"*)
    case "$postinst" in
    *"deb-systemd-helper debian-installed"*) ;;
    *) report "postinst enables the unit on a fresh install" ;;
    esac
    ;;
esac

# Derived from the binaries by dpkg-shlibdeps, not listed by hand: a hand-kept
# list goes stale the first time the distribution renames a Qt package.
depends=$(printf '%s\n' "$control" | sed -n 's/^ Depends: //p')
case "$depends" in
*libqt6core*) ;;
*) report "Depends does not mention Qt: '$depends'" ;;
esac

# Optional: not installed on the build machine, and the package targets one
# machine rather than an archive, so its absence is not a failure.
if command -v lintian >/dev/null 2>&1; then
    lintian --no-tag-display-limit "$deb" || report "lintian is unhappy"
fi

if [ "$status" -eq 0 ]; then
    echo "package: clean ($(basename "$deb"))"
fi
exit "$status"

#!/bin/sh
# Static checks that need no tooling beyond a shell.
#
# The compiler carries most of the load: every target is built with -Wall
# -Wextra -Wpedantic -Werror, so a warning is already a failed build. What is
# left is the house style, and the two things that are easy to leave behind by
# accident.
set -eu

root=$(dirname "$0")/..
cd "$root"

status=0
sources=$(find src tests -name '*.cpp' -o -name '*.h')

report() {
    echo "lint: $1" >&2
    status=1
}

for file in $sources; do
    if awk 'length > 100 { exit 1 }' "$file"; then :; else
        report "$file has lines longer than 100 columns"
    fi
    if grep -nP '\t' "$file" >/dev/null 2>&1; then
        report "$file contains tabs"
    fi
    if grep -nP ' +$' "$file" >/dev/null 2>&1; then
        report "$file has trailing whitespace"
    fi
    # Every source and header carries the licence line, because the repository
    # is public and a file without one is ambiguous.
    if ! head -n 1 "$file" | grep -q 'MIT licensed'; then
        report "$file has no licence header"
    fi
done

# Nothing tracked may point at the working directory, which is not in the
# repository and does not travel with a clone. AGENTS.md is the exception: it is
# where the convention itself is written down.
#
# The paths are listed rather than swept, so that a directory disappearing takes
# its entry with it — grep exits non-zero on a path that is not there, and this
# check quietly passed for exactly that reason once already.
#
# The system's own /tmp is a different thing and is not what this forbids, which
# is why a leading slash disqualifies a match.
tracked_tmp=$(grep -rn --exclude-dir=.git --exclude=lint.sh -e 'tmp/' \
    src tests scripts systemd desktop debian man completions \
    README.md README.ru.md HARDWARE.md HARDWARE.ru.md \
    DESIGN.md DESIGN.ru.md SCREENSHOTS.md SCREENSHOTS.ru.md 2>&1 \
    | grep -v '/tmp/' || true)
case "$tracked_tmp" in
*"No such file or directory"*)
    report "the tmp/ check names a path that is gone: $tracked_tmp"
    ;;
?*)
    report "a tracked file references tmp/: $(printf '%s' "$tracked_tmp" | head -n 3)"
    ;;
esac

# A placeholder that survives into a commit is a silent gap in the build.
if grep -rln 'PLACEHOLDER' src tests >/dev/null 2>&1; then
    report "placeholder sources are still present: $(grep -rln PLACEHOLDER src tests | tr '\n' ' ')"
fi

if [ "$status" -eq 0 ]; then
    echo "lint: clean"
fi
exit "$status"

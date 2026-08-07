#!/bin/sh
# Line coverage of the production sources, for machines without gcovr.
#
# Reports every file under src/ and the weighted total. A source with no
# coverage data at all is an error rather than a zero: it means the file was
# never linked into a test, which the quality gate is specifically there to
# catch.
set -eu

build_dir=${1:-build-coverage}
minimum=${2:-80}
results=

for source in ./src/*.cpp; do
    name=$(basename "$source")
    best=$(find "$build_dir" -name "$name.gcno" -type f -print | while read -r notes; do
        gcov -n "$notes" 2>/dev/null | awk -v source="$(realpath "$source")" '
            $0 == "File '\''" source "'\''" {
                getline
                sub(/^Lines executed:/, "")
                sub(/% of /, " ")
                print $1, $2
            }'
    done | sort -nr | head -n 1)
    if [ -z "$best" ]; then
        echo "No coverage data for $name" >&2
        exit 2
    fi
    results="${results}${name} ${best}
"
done

printf '%s' "$results" | LC_ALL=C awk -v minimum="$minimum" '
    NF == 3 {
        covered += ($2 / 100) * $3
        total += $3
        printf "%-32s %6.2f%% of %d lines\n", $1, $2, $3
    }
    END {
        percentage = total ? covered * 100 / total : 0
        printf "%-32s %6.2f%% of %d lines\n", "TOTAL", percentage, total
        if (percentage + 0.0001 < minimum) exit 1
    }'

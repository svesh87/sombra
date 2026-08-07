#!/bin/bash
# Calibration sweep: apply several (fan curve x power limit) combinations and
# record the resulting temperature and fan speed under identical conditions.
#
# The point of the sweep is to separate two knobs that the vendor tooling ties
# together: platform_profile (the EC fan curve) and the RAPL power limit.
# Neither system76-power nor the COSMIC shell can express "battery-grade power
# limit with a balanced fan curve", which is the combination worth evaluating.
#
# Run: sudo ./scripts/measure-combos.sh [--load N] [--settle S] [--sample S]
#
# Every setting is restored on exit, including on Ctrl-C and on the thermal
# abort. The original state is captured before the first change.
set -euo pipefail

LOAD_THREADS=8      # busy-loop workers; 0 measures the idle case instead
SETTLE_S=90         # the heatsink time constant is tens of seconds; wait it out
SAMPLE_S=90         # steady-state window actually recorded
ABORT_TEMP_C=95     # skip the current combination if the package gets this hot
COOL_TARGET_C=65    # cool back down to this between combinations
COOL_MAX_S=240      # give up waiting for COOL_TARGET_C after this long

while [ $# -gt 0 ]; do
    case "$1" in
        --load)   LOAD_THREADS="$2"; shift 2 ;;
        --settle) SETTLE_S="$2";     shift 2 ;;
        --sample) SAMPLE_S="$2";     shift 2 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

[ "$(id -u)" -eq 0 ] || { echo "must run as root: sudo $0" >&2; exit 1; }

# hwmon numbering is not stable across boots - resolve every sensor by name.
hwmon_by_name() {
    local want="$1" d
    for d in /sys/class/hwmon/hwmon*; do
        [ "$(cat "$d/name" 2>/dev/null)" = "$want" ] && { echo "$d"; return 0; }
    done
    return 1
}

CORETEMP=$(hwmon_by_name coretemp) || { echo "coretemp hwmon not found" >&2; exit 1; }
ASUS=$(hwmon_by_name asus)         || { echo "asus hwmon not found" >&2; exit 1; }
PSTATE=/sys/devices/system/cpu/intel_pstate
RAPL=/sys/class/powercap/intel-rapl:0
PROFILE=/sys/firmware/acpi/platform_profile

read_temp() { echo $(( $(cat "$CORETEMP/temp1_input") / 1000 )); }
read_fan()  { cat "$ASUS/fan1_input"; }

# --- save original state ------------------------------------------------------
ORIG_PROFILE=$(cat "$PROFILE")
ORIG_PL1=$(cat "$RAPL/constraint_0_power_limit_uw")
ORIG_PL2=$(cat "$RAPL/constraint_1_power_limit_uw")
ORIG_NOTURBO=$(cat "$PSTATE/no_turbo")
ORIG_MAXPCT=$(cat "$PSTATE/max_perf_pct")
ORIG_HWPBOOST=$(cat "$PSTATE/hwp_dynamic_boost")
ORIG_EPP=$(cat /sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference)

LOAD_PIDS=()
stop_load() {
    [ ${#LOAD_PIDS[@]} -eq 0 ] && return 0
    kill "${LOAD_PIDS[@]}" 2>/dev/null || true
    wait "${LOAD_PIDS[@]}" 2>/dev/null || true
    LOAD_PIDS=()
}

restore() {
    stop_load
    echo "$ORIG_PROFILE" > "$PROFILE" 2>/dev/null || true
    # Firmware rewrites the limits when the profile changes, so restore them after.
    echo "$ORIG_PL1"      > "$RAPL/constraint_0_power_limit_uw" 2>/dev/null || true
    echo "$ORIG_PL2"      > "$RAPL/constraint_1_power_limit_uw" 2>/dev/null || true
    echo "$ORIG_NOTURBO"  > "$PSTATE/no_turbo"                  2>/dev/null || true
    echo "$ORIG_MAXPCT"   > "$PSTATE/max_perf_pct"              2>/dev/null || true
    echo "$ORIG_HWPBOOST" > "$PSTATE/hwp_dynamic_boost"         2>/dev/null || true
    for f in /sys/devices/system/cpu/cpu*/cpufreq/energy_performance_preference; do
        echo "$ORIG_EPP" > "$f" 2>/dev/null || true
    done
    echo "Restored: profile=$ORIG_PROFILE PL=$((ORIG_PL1/1000000))/$((ORIG_PL2/1000000))W max_perf=$ORIG_MAXPCT% no_turbo=$ORIG_NOTURBO epp=$ORIG_EPP"
}
trap restore EXIT INT TERM

start_load() {
    local i
    [ "$LOAD_THREADS" -eq 0 ] && return 0   # --load 0 measures the idle case
    for ((i = 0; i < LOAD_THREADS; i++)); do
        # Pure integer busy loop: no package deps, no disk, no memory pressure.
        bash -c 'while :; do :; done' &
        LOAD_PIDS+=($!)
    done
}

# apply <profile> <pl1_w> <pl2_w> <max_perf_pct> <no_turbo> <epp> <hwp_boost>
apply() {
    # Profile first: writing it makes the firmware reassert its own power limits,
    # so every limit below must be written after, not before.
    echo "$1" > "$PROFILE"
    sleep 1
    echo $(( $2 * 1000000 )) > "$RAPL/constraint_0_power_limit_uw"
    echo $(( $3 * 1000000 )) > "$RAPL/constraint_1_power_limit_uw"
    echo "$4" > "$PSTATE/max_perf_pct"
    echo "$5" > "$PSTATE/no_turbo"
    echo "$7" > "$PSTATE/hwp_dynamic_boost"
    local f
    for f in /sys/devices/system/cpu/cpu*/cpufreq/energy_performance_preference; do
        echo "$6" > "$f"
    done
}

# Combinations are only comparable if each starts from the same thermal state.
# A fixed pause is not enough: a combination that ran hot leaves the heatsink
# loaded, which then shows up as the *next* combination's result.
cooldown() {
    stop_load
    apply balanced 20 30 50 1 power 0
    local waited=0
    while [ "$(read_temp)" -gt "$COOL_TARGET_C" ] && [ "$waited" -lt "$COOL_MAX_S" ]; do
        sleep 5
        waited=$((waited + 5))
    done
    printf "  (cooled to %s C in %ss)\n" "$(read_temp)" "$waited"
}

OUT="${OUT:-./tmp/combos_$(date +%Y-%m-%d_%H-%M-%S).csv}"
mkdir -p "$(dirname "$OUT")"
echo "combo,t_s,pkg_c,fan_rpm,pl1_read_w" > "$OUT"

# label|profile|pl1|pl2|max_perf_pct|no_turbo|epp|hwp_dynamic_boost
#
# The sweep separates two effects that the vendor profiles always change
# together, so that each can be judged on its own:
#   * the EC fan curve, selected by platform_profile;
#   * burst suppression (turbo, frequency ceiling, EPP, HWP dynamic boost),
#     which is what heats the package when the machine is nominally idle.
# Combos 2 and 3 vary exactly one of the two against the reference; combo 4 is
# the combination the vendor tooling cannot express, and combo 5 keeps the lazy
# curve for contrast.
COMBOS=(
    "1 reference: balanced 45/65 100% turbo|balanced|45|65|100|0|balance_performance|1"
    "2 aggressive curve only: performance 45/65 100%|performance|45|65|100|0|balance_performance|1"
    "3 burst suppression only: balanced 20/30 50%|balanced|20|30|50|1|power|0"
    "4 aggressive curve + suppression: performance 20/30 50%|performance|20|30|50|1|power|0"
    "5 lazy curve + suppression: quiet 20/30 50%|quiet|20|30|50|1|power|0"
)

echo "Load: $LOAD_THREADS threads. Settle ${SETTLE_S}s, record ${SAMPLE_S}s."
echo "About $(( ${#COMBOS[@]} * (SETTLE_S + SAMPLE_S) / 60 )) minutes total. Ctrl-C is safe."
echo

cooldown   # start from a known thermal state, not from whatever preceded the run

for entry in "${COMBOS[@]}"; do
    IFS='|' read -r label prof pl1 pl2 pct turbo epp hwp <<< "$entry"
    apply "$prof" "$pl1" "$pl2" "$pct" "$turbo" "$epp" "$hwp"
    start_load

    aborted=0
    for ((t = 0; t < SETTLE_S + SAMPLE_S; t += 2)); do
        temp=$(read_temp)
        if [ "$temp" -ge "$ABORT_TEMP_C" ]; then
            # Abandon this combination but keep going: a combination that cooks
            # the machine is itself a result, and the remaining ones still need
            # measuring.
            echo "  ${label}: THERMAL ABORT at ${temp} C, skipped" >&2
            aborted=1
            break
        fi
        if [ "$t" -ge "$SETTLE_S" ]; then
            pl1r=$(( $(cat "$RAPL/constraint_0_power_limit_uw") / 1000000 ))
            echo "$label,$((t - SETTLE_S)),$temp,$(read_fan),$pl1r" >> "$OUT"
        fi
        sleep 2
    done

    if [ "$aborted" -eq 0 ]; then
        # Steady-state means for the console summary.
        LC_ALL=C awk -F, -v L="$label" '$1==L {n++; ts+=$3; fs+=$4}
            END {printf "  %-48s %3.0f C   %4.0f RPM\n", L, ts/n, fs/n}' "$OUT"
    fi

    cooldown
done

echo
echo "CSV: $OUT"

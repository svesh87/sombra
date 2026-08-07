#!/bin/bash
# Observe and record. Changes nothing, so it is safe to start at any time.
#
# Synthetic load is a poor model of a real workload: a busy loop heats the cores
# only, while a video call also loads the iGPU, and integer spinning draws less
# power per core than mixed real work. This logger exists to capture what the
# machine actually does during the workload that matters, rather than during an
# approximation of it.
#
# Run: sudo ./scripts/log-session.sh [-o FILE] [-i SECONDS]
# Stop with Ctrl-C; a summary is printed on exit.
#
# Root is needed only to read the RAPL energy counters, which give the package
# power and its split between core and uncore - the split that shows whether the
# iGPU is contributing. Without root everything else is still recorded.
set -euo pipefail

OUT="./tmp/session_$(date +%Y-%m-%d_%H-%M-%S).csv"
INTERVAL=1

while [ $# -gt 0 ]; do
    case "$1" in
        -o) OUT="$2"; shift 2 ;;
        -i) INTERVAL="$2"; shift 2 ;;
        *) echo "usage: $0 [-o FILE] [-i SECONDS]" >&2; exit 2 ;;
    esac
done

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
ACPITZ=$(hwmon_by_name acpitz || true)
NVME=$(hwmon_by_name nvme || true)

RAPL=/sys/class/powercap/intel-rapl:0
THROTTLE=/sys/devices/system/cpu/cpu0/thermal_throttle
# iGPU clock. Recorded because a video call loads the graphics engine as much as
# the cores - decoding every participant's stream, encoding your own, and any
# continuous inference such as auto-framing - and CPU-side limits do nothing
# about that.
GPUFREQ=$(ls /sys/class/drm/card*/gt_act_freq_mhz 2>/dev/null | head -1)

# Missing or unreadable nodes degrade to an empty field rather than aborting the
# recording: a partial log of a real session is worth more than no log.
read_or_blank() { cat "$1" 2>/dev/null || true; }
milli_to_c() { local v; v=$(read_or_blank "$1"); [ -n "$v" ] && echo $((v / 1000)) || echo ""; }

# RAPL exposes a free-running energy counter, so power is a delta over time.
# The counter wraps at max_energy_range_uj and the wrap must not appear as a
# huge negative sample.
ENERGY_MAX=$(read_or_blank "$RAPL/max_energy_range_uj")
energy_delta() { # <path> <previous>  -> microjoules consumed since previous
    local cur prev d
    cur=$(read_or_blank "$1"); prev="$2"
    [ -z "$cur" ] || [ -z "$prev" ] && { echo ""; return; }
    d=$((cur - prev))
    [ "$d" -lt 0 ] && [ -n "$ENERGY_MAX" ] && d=$((d + ENERGY_MAX))
    echo "$d"
}

mkdir -p "$(dirname "$OUT")"
echo "ts,pkg_c,acpitz_c,nvme_c,fan_rpm,pkg_w,core_w,uncore_w,freq_avg_mhz,gpu_mhz,load1,pl1_w,pkg_throttle,core_throttle" > "$OUT"

prev_pkg=$(read_or_blank "$RAPL/energy_uj")
prev_core=$(read_or_blank "$RAPL/intel-rapl:0:0/energy_uj")
prev_uncore=$(read_or_blank "$RAPL/intel-rapl:0:1/energy_uj")
base_pkg_thr=$(read_or_blank "$THROTTLE/package_throttle_count")
base_core_thr=$(read_or_blank "$THROTTLE/core_throttle_count")

summarise() {
    echo
    LC_ALL=C awk -F, 'NR>1 {
        n++; t+=$2; f+=$5; p+=$6; u+=$8
        if($2>tmax) tmax=$2
        if($5>fmax) fmax=$5
        if($6>pmax) pmax=$6
    } END {
        if(n==0) { print "no samples"; exit }
        printf "samples      %d (%.1f min)\n", n, n*'"$INTERVAL"'/60
        printf "package      mean %.1f C   peak %d C\n", t/n, tmax
        printf "fan          mean %.0f RPM  peak %d RPM\n", f/n, fmax
        printf "package pwr  mean %.1f W   peak %.1f W\n", p/n, pmax
        printf "uncore pwr   mean %.1f W   (%.0f%% of package - the iGPU share)\n", u/n, (p>0 ? 100*u/p : 0)
    }' "$OUT"
    echo "throttle events during the session: package $(( $(read_or_blank "$THROTTLE/package_throttle_count") - base_pkg_thr )), core $(( $(read_or_blank "$THROTTLE/core_throttle_count") - base_core_thr ))"
    echo "CSV: $OUT"
}
trap summarise EXIT INT TERM

echo "Recording every ${INTERVAL}s to $OUT. Ctrl-C to stop."

while :; do
    sleep "$INTERVAL"

    cur_pkg=$(read_or_blank "$RAPL/energy_uj")
    cur_core=$(read_or_blank "$RAPL/intel-rapl:0:0/energy_uj")
    cur_uncore=$(read_or_blank "$RAPL/intel-rapl:0:1/energy_uj")

    # microjoules over INTERVAL seconds -> watts
    w() { local d="$1"; [ -n "$d" ] && LC_ALL=C awk -v d="$d" -v i="$INTERVAL" 'BEGIN{printf "%.2f", d/1000000/i}' || echo ""; }

    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "$(date +%s)" \
        "$(milli_to_c "$CORETEMP/temp1_input")" \
        "$([ -n "$ACPITZ" ] && milli_to_c "$ACPITZ/temp1_input")" \
        "$([ -n "$NVME" ] && milli_to_c "$NVME/temp1_input")" \
        "$(read_or_blank "$ASUS/fan1_input")" \
        "$(w "$(energy_delta "$RAPL/energy_uj" "$prev_pkg")")" \
        "$(w "$(energy_delta "$RAPL/intel-rapl:0:0/energy_uj" "$prev_core")")" \
        "$(w "$(energy_delta "$RAPL/intel-rapl:0:1/energy_uj" "$prev_uncore")")" \
        "$(LC_ALL=C awk '/cpu MHz/{s+=$4;n++} END{printf "%.0f", s/n}' /proc/cpuinfo)" \
        "$([ -n "$GPUFREQ" ] && read_or_blank "$GPUFREQ")" \
        "$(LC_ALL=C awk '{print $1}' /proc/loadavg)" \
        "$(( $(read_or_blank "$RAPL/constraint_0_power_limit_uw") / 1000000 ))" \
        "$(read_or_blank "$THROTTLE/package_throttle_count")" \
        "$(read_or_blank "$THROTTLE/core_throttle_count")" \
        >> "$OUT"

    prev_pkg=$cur_pkg; prev_core=$cur_core; prev_uncore=$cur_uncore
done

#!/bin/bash
# Manual stopgap: cap the package hard so a sustained workload cannot overheat it.
#
#   sudo ./cool-mode.sh        enable, fan forced to full speed
#   sudo ./cool-mode.sh auto   enable, fan left under EC control (candidate mode)
#   sudo ./cool-mode.sh off    return to a sane baseline
#
# "on" is the configuration that has been shown to hold, and is deliberately
# left unchanged.
#
# "auto" exists to decompose it. The proven configuration does two things at
# once - it caps power *and* forces the fan to maximum - so a session that
# survives under it does not say which of the two was responsible. Maximum fan
# speed is around 6100 RPM, far above what is tolerable, so it is worth knowing
# whether the power cap alone is sufficient. "auto" applies the same limits with
# the fan under EC control, plus the settings that measurement showed actually
# reduce temperature: EPP, HWP dynamic boost and a fan curve that is not lazy.
#
# Everything written here is lost on reboot. "off" deliberately does not restore
# the firmware defaults: the firmware programs PL1 to 200 W, which is more than
# the chassis can dissipate.
set -eu

FAN=$(ls -d /sys/devices/platform/asus-nb-wmi/hwmon/hwmon*/pwm1_enable | head -1)
PSTATE=/sys/devices/system/cpu/intel_pstate
RAPL=/sys/class/powercap/intel-rapl:0
PROFILE=/sys/firmware/acpi/platform_profile

set_epp() {
    for f in /sys/devices/system/cpu/cpu*/cpufreq/energy_performance_preference; do
        echo "$1" > "$f"
    done
}

if [ "${1:-on}" = "auto" ]; then
    # Profile first: writing it makes the firmware reassert its own power limits.
    echo performance > "$PROFILE"   # not lazy: with little heat produced this costs no RPM
    sleep 1
    echo 2 > "$FAN"                                      # fan stays under EC control
    echo 1 > "$PSTATE/no_turbo"
    echo 60 > "$PSTATE/max_perf_pct"
    echo 0 > "$PSTATE/hwp_dynamic_boost"                 # suppress micro-bursts
    set_epp power
    echo 15000000 > "$RAPL/constraint_0_power_limit_uw"  # PL1 15 W
    echo 25000000 > "$RAPL/constraint_1_power_limit_uw"  # PL2 25 W
    echo "Candidate mode on: 15/25 W, bursts suppressed, fan on auto. Watch the RPM."
    exit 0
fi

if [ "${1:-on}" = "off" ]; then
    echo balanced > "$PROFILE"
    sleep 1
    echo 1 > "$PSTATE/hwp_dynamic_boost"
    set_epp balance_performance
    echo 2 > "$FAN"                                      # fan back to EC control
    echo 0 > "$PSTATE/no_turbo"
    echo 100 > "$PSTATE/max_perf_pct"
    echo 45000000 > "$RAPL/constraint_0_power_limit_uw"  # PL1 45 W, not the firmware's 200 W
    echo 65000000 > "$RAPL/constraint_1_power_limit_uw"  # PL2 65 W
    echo "Baseline restored; power limits left at a sane value."
else
    echo 0 > "$FAN"                                      # 0 means full speed on asus-wmi
    echo 1 > "$PSTATE/no_turbo"
    echo 60 > "$PSTATE/max_perf_pct"
    echo 15000000 > "$RAPL/constraint_0_power_limit_uw"  # PL1 15 W
    echo 25000000 > "$RAPL/constraint_1_power_limit_uw"  # PL2 25 W
    echo "Cooling mode on: fan at full speed, package limited to 15/25 W, no turbo."
fi

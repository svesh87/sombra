// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#pragma once

#include "sample.h"

#include <QString>

// Everything the daemon writes to the hardware in one tick.
//
// These are static per mode. There is no feedback loop that walks them towards
// a target: measurement showed static values handle both idle and sustained
// load, and a controller would add coefficients to tune and failure modes —
// hunting and sticking — that static values cannot have.
struct Setpoints {
    Profile profile = Profile::Balanced;
    int pl1W = 20;  // RAPL long-term limit, watts
    int pl2W = 30;  // RAPL short-term limit, watts
    int maxPerfPct = 50;
    bool noTurbo = true;
    Epp epp = Epp::Power;
    bool hwpBoost = false;
    int gtMaxMhz = 2350;
    int fanMode = 2;  // pwm1_enable: 2 automatic, 0 full speed

    bool operator==(const Setpoints &other) const;
    bool operator!=(const Setpoints &other) const { return !(*this == other); }
};

// The highest limits the maintainer has confirmed on this machine. Nothing —
// not a configuration file, not a live slider, not a return to a "normal"
// state — raises a limit past these. The firmware's own 200 W PL1 is a fault
// condition, not a ceiling to aim for.
inline constexpr int kMaxConfirmedPl1W = 45;
inline constexpr int kMaxConfirmedPl2W = 65;

// intel_pstate reports min_perf_pct = 8 on this platform, which is the floor
// the frequency ceiling can be pushed to.
inline constexpr int kMinPerfPct = 8;
inline constexpr int kMaxGtMhz = 2350;  // gt_RP0_freq_mhz, the hardware maximum
// gt_RPn_freq_mhz, which is the floor the driver accepts for gt_max_freq_mhz.
// Not gt_min_freq_mhz, which reads 100 on this machine and is a different node:
// a gt_max below RPn is refused outright. Measured — with 100 here the write
// never took, and every one of the 3826 samples recorded with it flagged the
// control as disagreeing. The emergency guard clamps the iGPU ceiling to this
// value, so the wrong number meant the guard's iGPU clamp did nothing either.
inline constexpr int kMinGtMhz = 800;

// The state the machine is left in when the daemon exits. Not the firmware
// default, which would restore the 200 W PL1, and not the current setpoints,
// which may be an emergency clamp that would then outlive the daemon.
//
// This does raise the limits on the way out, including out of a clamp, so a
// service restart under sustained load briefly un-throttles a hot package.
// That is the price of the rule that the fan must always go back to EC
// control: leaving a clamp behind without a daemon to release it is the worse
// of the two.
Setpoints safeExitSetpoints();

// Emergency thermal guard.
//
// This is a safety interlock, not regulation: it is always active regardless
// of mode, it only ever lowers, and it exists because this platform can reach
// its hardware cutout before a throttling loop responds. Thresholds sit well
// below the coretemp critical point, which is never moved.
inline constexpr int16_t kGuardClampTempDeci = 950;    // 95.0 °C
inline constexpr int16_t kGuardFanMaxTempDeci = 1000;  // 100.0 °C
inline constexpr int16_t kGuardReleaseTempDeci = 850;  // 85.0 °C
// Release is delayed so the clamp cannot chatter around its own threshold.
inline constexpr int kGuardReleaseTicks = 10;
// Watts, like every other PL1/PL2 value here; the temperature constants above
// are decidegrees Celsius.
inline constexpr int kGuardPl1W = 10;
inline constexpr int kGuardPl2W = 15;

struct GuardState {
    bool engaged = false;
    // The one place the daemon drives the fan by itself. Automatic control
    // never does this otherwise: full speed is roughly 6100 RPM.
    bool fanForced = false;
    int coolTicks = 0;
};

// Pure function of (sample, requested setpoints, guard state). The only
// mutation is of the state passed in, which makes every escalation, release
// and hysteresis path testable without hardware and without a clock.
Setpoints applyGuard(const Sample &sample, const Setpoints &requested, GuardState &state);

QString eppToString(Epp epp);
Epp eppFromString(const QString &text);
QString profileToString(Profile profile);
Profile profileFromString(const QString &text);
QString markToString(Mark mark);
Mark markFromString(const QString &text);

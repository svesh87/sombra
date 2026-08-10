// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#pragma once

#include <cstdint>

// One telemetry record, appended once per second.
//
// The layout is fixed because the record size is what turns a retention period
// into a file size: at 1 Hz, seven days is kRingCapacity records, and 64 bytes
// each keeps the ring under 40 MB. Fields are integers in units chosen so that
// nothing needs floating point and nothing is averaged — averaging would hide
// exactly the peaks the log exists to show.
//
//   temperatures  decidegrees Celsius  (805 = 80.5 °C)
//   power         centiwatts           (2350 = 23.50 W)
//   frequency     megahertz
//   load          hundredths           (61 = 0.61)
//   brightness    percent of the panel's own scale
//   light         millilux
//
// A missing reading is stored as kAbsentTemp, kAbsentValue or kAbsentPercent,
// never as zero: zero is a legal fan speed, a legal power draw and a legal
// backlight level.

inline constexpr int16_t kAbsentTemp = INT16_MIN;
inline constexpr uint16_t kAbsentValue = UINT16_MAX;
inline constexpr uint8_t kAbsentPercent = UINT8_MAX;
inline constexpr uint32_t kAbsentLight = UINT32_MAX;

// Values of energy_performance_preference. Stored as a small enum rather than
// a string so that the record stays fixed-size.
enum class Epp : uint8_t {
    Unknown = 0,
    // Listed by the kernel but never written by this daemon, and not accepted
    // from the configuration: the node takes "default" and then reports the
    // preference it actually chose, so a written "default" reads back as
    // something else and the daemon would flag a disagreement that is really
    // its own. It also is not a setpoint — it means "whatever the driver
    // picks", which contradicts static setpoints per mode. The value stays in
    // the enum because samples store these as numbers.
    Default,
    Performance,
    BalancePerformance,
    BalancePower,
    Power,
};

// Values of /sys/firmware/acpi/platform_profile. This is the EC fan curve
// selector, which is the noise-versus-temperature dial on this platform.
enum class Profile : uint8_t {
    Unknown = 0,
    Quiet,
    Balanced,
    Performance,
};

// A judgement made by ear, stamped onto the current sample from the tray.
// Without it, live tuning produces an opinion that cannot be traced back to
// the numbers that caused it.
enum class Mark : uint8_t {
    None = 0,
    Good,
    TooLoud,
    TooSlow,
};

enum SampleFlag : uint8_t {
    FlagOnAc = 1u << 0,
    // The emergency thermal guard is clamping the setpoints this tick.
    FlagGuardEngaged = 1u << 1,
    // The control surface disagreed with the daemon during this tick: a node
    // held a value the daemon did not put there, a node that had been resolved
    // disappeared, or a write did not take. The first case is the one that
    // matters — it means something else is writing controls this daemon is
    // supposed to own alone.
    FlagWriteMismatch = 1u << 2,
    // At least one sensor could not be read this tick.
    FlagSensorsDegraded = 1u << 3,
    // Live tuning is in force, so the setpoints below are not the mode's.
    FlagLiveTuning = 1u << 4,
    // The backlight is following the ambient light sensor. Switched off, the
    // daemon does not write the backlight at all, which is why this has to be
    // recorded: without it a flat brightness trace is ambiguous.
    FlagAutoBrightness = 1u << 5,
};

#pragma pack(push, 1)
struct Sample {
    uint32_t ts;          // unix seconds; 0 marks a slot that was never written
    int16_t pkgTemp;      // coretemp "Package id 0"
    int16_t boardTemp;    // acpitz
    int16_t nvmeTemp;     // nvme Composite
    uint16_t fanRpm;
    uint16_t pkgPower;    // RAPL package-0
    uint16_t corePower;   // RAPL core sub-domain
    uint16_t uncorePower; // RAPL uncore sub-domain: separates iGPU heat from core heat
    uint16_t cpuMhz;      // mean of the online cores
    uint16_t gpuMhz;      // i915 gt_act_freq_mhz
    uint16_t load1;
    // Hardware throttle events since the previous sensor read, not since a
    // fixed interval: the counters are cumulative for the life of the boot, so
    // only their rate of change means anything, and the first read after a
    // restart has no rate yet and records zero.
    uint16_t pkgThrottle;
    uint16_t coreThrottle; // same, summed over the cores

    // Connected display connectors, counted from /sys/class/drm/*/status. The
    // internal panel is normally one of them, so this is not "external
    // monitors"; the difference is taken when the data is read, because with a
    // closed lid the panel may be absent from the count as well.
    //
    // Recorded because driving an external monitor is work that cannot be
    // deferred, and its cost is invisible in a log that does not say whether
    // one was attached. Nothing controls anything by this number.
    //
    // What it does not know: a connector reads "connected" while a cable is in
    // it, whether or not the output is lit, and a DP-MST hub appears
    // differently than expected. Resolution and scaling are deliberately not
    // recorded: sysfs does not have them, and the graphics work they would
    // stand for is already measured directly by uncorePower and gpuMhz.
    uint8_t displays; // kAbsentPercent when the DRM tree could not be read

    // The setpoints in force at the moment of the sample. A log that does not
    // say what was applied cannot be analysed, and mode and setpoint
    // transitions are derived by diffing consecutive samples rather than from
    // a separate event log.
    uint8_t modeId;      // index into the configured mode table, 0xFF if unknown
    uint8_t pl1W;        // RAPL long-term limit, watts
    uint8_t pl2W;        // RAPL short-term limit, watts
    uint8_t maxPerfPct;  // intel_pstate frequency ceiling
    uint8_t noTurbo;
    uint8_t epp;      // Epp
    uint8_t hwpBoost; // intel_pstate hwp_dynamic_boost
    uint8_t profile;  // Profile
    uint16_t gtMaxMhz;
    uint8_t fanMode;     // pwm1_enable: 2 automatic, 0 full speed
    uint8_t chargeLimit; // charge_control_end_threshold, percent
    uint8_t flags;       // SampleFlag
    uint8_t mark;        // Mark

    // The backlight side. Recorded in every sample for the same reason the
    // setpoints are: the four anti-hunting constants are only observable in
    // the data, so a trace that does not carry the light reading, the level
    // and the baseline it was measured against cannot be used to judge them.
    //
    // Millilux, in four bytes because the sensor reaches roughly 20 855 lux and
    // a room with the ceiling light on already reads 103. Sixteen bits would
    // stop at 65 lux and report every brighter room as the same number.
    uint32_t light;
    uint8_t brightness;     // panel level, percent
    uint8_t brightnessBase; // the level with the light's contribution removed
    // What the automation was allowed to do at the moment of the sample.
    // Recorded rather than looked up from the settings file, because a record
    // that needs another file to be read is not self-contained.
    uint8_t brightnessResponse;
    uint8_t brightnessFloor;

    uint8_t reserved[13];
};
#pragma pack(pop)

static_assert(sizeof(Sample) == 64, "the ring geometry depends on a 64-byte record");

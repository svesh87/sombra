// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#pragma once

#include "sample.h"
#include "sysfs.h"

#include <QString>
#include <QStringList>

#include <cstdint>

// Reads one telemetry record per tick.
//
// Node discovery happens once, in resolve(): hwmon indices move between boots
// and a sensor may be absent on another kernel, so nothing is addressed by
// number and every reading degrades to "absent" rather than to a crash.
//
// Power is derived from the RAPL energy counters rather than read directly.
// Those counters are cumulative and wrap at max_energy_range_uj, which is why
// the reader keeps the previous value and the elapsed time itself.
class SensorReader {
public:
    explicit SensorReader(Sysfs &sysfs);

    // Locates every node once. Safe to call again after a resume or a kernel
    // module reload; it simply re-resolves.
    void resolve();

    // One record. Setpoint fields are left at zero: the daemon fills them in,
    // because only it knows what was actually applied this tick.
    Sample read(uint32_t timestamp, qint64 monotonicMs);

    // True when at least one node could not be read on the last call. The
    // daemon uses this to fall back to the most conservative mode.
    bool degraded() const { return m_degraded; }

    // Paths resolved by resolve(), exposed so that a diagnostic can print what
    // the daemon actually found.
    QString packageTempPath() const { return m_pkgTempPath; }
    QString fanPath() const { return m_fanPath; }

private:
    std::optional<qint64> readEnergyUj(const QString &domainDir, const QString &counterName,
                                       uint64_t &previous, qint64 elapsedMs);

    Sysfs &m_sysfs;
    bool m_degraded = false;

    QString m_pkgTempPath;
    QString m_boardTempPath;
    QString m_nvmeTempPath;
    QString m_fanPath;
    QString m_gpuFreqPath;
    QString m_raplPackage;
    QString m_raplCore;
    QString m_raplUncore;
    QStringList m_cpuFreqPaths;
    QStringList m_coreThrottlePaths;
    QString m_packageThrottlePath;

    uint64_t m_prevPackageUj = 0;
    uint64_t m_prevCoreUj = 0;
    uint64_t m_prevUncoreUj = 0;
    uint64_t m_packageRangeUj = 0;
    uint64_t m_coreRangeUj = 0;
    uint64_t m_uncoreRangeUj = 0;
    bool m_haveEnergyBaseline = false;
    qint64 m_prevMonotonicMs = 0;
    bool m_haveMonotonic = false;

    uint64_t m_prevPackageThrottle = 0;
    uint64_t m_prevCoreThrottle = 0;
    bool m_haveThrottleBaseline = false;
};

// Difference between two readings of a counter that wraps at `range`.
// Exposed because wraparound is the one thing in this file worth testing on
// its own: it happens rarely enough in service to never be seen in a log.
uint64_t energyDelta(uint64_t current, uint64_t previous, uint64_t range);

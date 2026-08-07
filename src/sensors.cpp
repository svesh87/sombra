// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "sensors.h"

#include <algorithm>

namespace {

// Finds the tempN_input node whose tempN_label matches, which is how the
// package sensor is told apart from the per-core ones.
QString labelledTempInput(const Sysfs &sysfs, const QString &hwmonDir, const QString &label)
{
    const QStringList labels = sysfs.glob(hwmonDir, QStringLiteral("temp*_label"));
    for (const QString &labelPath : labels) {
        const std::optional<QString> text = sysfs.readText(labelPath);
        if (!text || *text != label)
            continue;
        QString input = labelPath;
        input.replace(QLatin1String("_label"), QLatin1String("_input"));
        if (sysfs.exists(input))
            return input;
    }
    return {};
}

QString firstExisting(const Sysfs &sysfs, const QStringList &candidates)
{
    for (const QString &candidate : candidates) {
        if (sysfs.exists(candidate))
            return candidate;
    }
    return {};
}

uint16_t clampToU16(qint64 value)
{
    if (value < 0)
        return 0;
    if (value >= kAbsentValue)
        return kAbsentValue - 1;
    return static_cast<uint16_t>(value);
}

} // namespace

uint64_t energyDelta(uint64_t current, uint64_t previous, uint64_t range)
{
    if (current >= previous)
        return current - previous;
    // The counter wrapped. Without a known range there is no way to tell how
    // much was missed, so the tick contributes nothing rather than a spike.
    if (range == 0 || range < previous)
        return 0;
    return (range - previous) + current;
}

SensorReader::SensorReader(Sysfs &sysfs)
    : m_sysfs(sysfs)
{
}

void SensorReader::resolve()
{
    m_pkgTempPath.clear();
    m_boardTempPath.clear();
    m_nvmeTempPath.clear();
    m_fanPath.clear();
    m_gpuFreqPath.clear();
    m_raplPackage.clear();
    m_raplCore.clear();
    m_raplUncore.clear();
    m_cpuFreqPaths.clear();
    m_coreThrottlePaths.clear();
    m_packageThrottlePath.clear();

    if (const auto coretemp = m_sysfs.hwmonByName(QStringLiteral("coretemp"))) {
        m_pkgTempPath = labelledTempInput(m_sysfs, *coretemp, QStringLiteral("Package id 0"));
        if (m_pkgTempPath.isEmpty())
            m_pkgTempPath = firstExisting(m_sysfs, { *coretemp + QStringLiteral("/temp1_input") });
    }
    if (const auto acpitz = m_sysfs.hwmonByName(QStringLiteral("acpitz")))
        m_boardTempPath = firstExisting(m_sysfs, { *acpitz + QStringLiteral("/temp1_input") });
    if (const auto nvme = m_sysfs.hwmonByName(QStringLiteral("nvme"))) {
        m_nvmeTempPath = labelledTempInput(m_sysfs, *nvme, QStringLiteral("Composite"));
        if (m_nvmeTempPath.isEmpty())
            m_nvmeTempPath = firstExisting(m_sysfs, { *nvme + QStringLiteral("/temp1_input") });
    }
    // The asus hwmon is the primary fan reading; acpi_fan is the cross-check
    // and stands in when the vendor node is missing.
    if (const auto asus = m_sysfs.hwmonByName(QStringLiteral("asus")))
        m_fanPath = firstExisting(m_sysfs, { *asus + QStringLiteral("/fan1_input") });
    if (m_fanPath.isEmpty()) {
        if (const auto acpiFan = m_sysfs.hwmonByName(QStringLiteral("acpi_fan")))
            m_fanPath = firstExisting(m_sysfs, { *acpiFan + QStringLiteral("/fan1_input") });
    }

    const QStringList cards =
            m_sysfs.glob(QStringLiteral("sys/class/drm"), QStringLiteral("card*"));
    for (const QString &card : cards) {
        const QString candidate = card + QStringLiteral("/gt_act_freq_mhz");
        if (m_sysfs.exists(candidate)) {
            m_gpuFreqPath = candidate;
            break;
        }
    }

    if (const auto package = m_sysfs.raplDomainByName(QStringLiteral("package-0")))
        m_raplPackage = *package;
    if (const auto core = m_sysfs.raplDomainByName(QStringLiteral("core")))
        m_raplCore = *core;
    if (const auto uncore = m_sysfs.raplDomainByName(QStringLiteral("uncore")))
        m_raplUncore = *uncore;

    const QStringList cpus =
            m_sysfs.glob(QStringLiteral("sys/devices/system/cpu"), QStringLiteral("cpu[0-9]*"));
    for (const QString &cpu : cpus) {
        const QString freq = cpu + QStringLiteral("/cpufreq/scaling_cur_freq");
        if (m_sysfs.exists(freq))
            m_cpuFreqPaths.append(freq);
        const QString coreThrottle = cpu + QStringLiteral("/thermal_throttle/core_throttle_count");
        if (m_sysfs.exists(coreThrottle))
            m_coreThrottlePaths.append(coreThrottle);
        const QString pkgThrottle =
                cpu + QStringLiteral("/thermal_throttle/package_throttle_count");
        if (m_packageThrottlePath.isEmpty() && m_sysfs.exists(pkgThrottle))
            m_packageThrottlePath = pkgThrottle;
    }

    m_haveEnergyBaseline = false;
    m_haveThrottleBaseline = false;

    const auto readRange = [this](const QString &domain, uint64_t &range) {
        range = 0;
        if (domain.isEmpty())
            return;
        if (const auto value = m_sysfs.readNumber(domain + QStringLiteral("/max_energy_range_uj")))
            range = static_cast<uint64_t>(std::max<qint64>(0, *value));
    };
    readRange(m_raplPackage, m_packageRangeUj);
    readRange(m_raplCore, m_coreRangeUj);
    readRange(m_raplUncore, m_uncoreRangeUj);
}

std::optional<qint64> SensorReader::readEnergyUj(const QString &domainDir,
                                                 const QString &counterName, uint64_t &previous,
                                                 qint64 elapsedMs)
{
    if (domainDir.isEmpty())
        return std::nullopt;
    const auto raw = m_sysfs.readNumber(domainDir + QStringLiteral("/energy_uj"));
    if (!raw)
        return std::nullopt;
    const uint64_t current = static_cast<uint64_t>(std::max<qint64>(0, *raw));

    uint64_t range = 0;
    if (counterName == QLatin1String("package"))
        range = m_packageRangeUj;
    else if (counterName == QLatin1String("core"))
        range = m_coreRangeUj;
    else
        range = m_uncoreRangeUj;

    const uint64_t delta = energyDelta(current, previous, range);
    previous = current;
    if (!m_haveEnergyBaseline || elapsedMs <= 0)
        return std::nullopt;
    // Microjoules over milliseconds is milliwatts; the record holds
    // centiwatts.
    return static_cast<qint64>(delta) / elapsedMs / 10;
}

Sample SensorReader::read(uint32_t timestamp, qint64 monotonicMs)
{
    Sample sample {};
    sample.ts = timestamp;
    sample.pkgTemp = kAbsentTemp;
    sample.boardTemp = kAbsentTemp;
    sample.nvmeTemp = kAbsentTemp;
    sample.fanRpm = kAbsentValue;
    sample.pkgPower = kAbsentValue;
    sample.corePower = kAbsentValue;
    sample.uncorePower = kAbsentValue;
    sample.cpuMhz = kAbsentValue;
    sample.gpuMhz = kAbsentValue;
    sample.load1 = kAbsentValue;
    sample.modeId = 0xFF;

    m_degraded = false;
    const auto note = [this](bool ok) {
        if (!ok)
            m_degraded = true;
    };

    // Millidegrees to decidegrees.
    const auto readTemp = [this, &note](const QString &path) -> int16_t {
        if (path.isEmpty()) {
            note(false);
            return kAbsentTemp;
        }
        const auto milli = m_sysfs.readNumber(path);
        if (!milli) {
            note(false);
            return kAbsentTemp;
        }
        return static_cast<int16_t>(std::clamp<qint64>(*milli / 100, -3000, 3000));
    };

    sample.pkgTemp = readTemp(m_pkgTempPath);
    sample.boardTemp = readTemp(m_boardTempPath);
    sample.nvmeTemp = readTemp(m_nvmeTempPath);

    if (!m_fanPath.isEmpty()) {
        if (const auto rpm = m_sysfs.readNumber(m_fanPath))
            sample.fanRpm = clampToU16(*rpm);
        else
            note(false);
    } else {
        note(false);
    }

    // Zero is a legitimate first timestamp, so the absence of a previous
    // reading is tracked with a flag rather than inferred from the value.
    const qint64 elapsedMs = m_haveMonotonic ? monotonicMs - m_prevMonotonicMs : 0;
    const bool hadBaseline = m_haveEnergyBaseline;
    if (const auto watts =
                readEnergyUj(m_raplPackage, QStringLiteral("package"), m_prevPackageUj, elapsedMs))
        sample.pkgPower = clampToU16(*watts);
    else if (hadBaseline && !m_raplPackage.isEmpty())
        note(false);
    if (const auto watts =
                readEnergyUj(m_raplCore, QStringLiteral("core"), m_prevCoreUj, elapsedMs))
        sample.corePower = clampToU16(*watts);
    if (const auto watts =
                readEnergyUj(m_raplUncore, QStringLiteral("uncore"), m_prevUncoreUj, elapsedMs))
        sample.uncorePower = clampToU16(*watts);
    if (m_raplPackage.isEmpty())
        note(false);
    m_haveEnergyBaseline = true;
    m_prevMonotonicMs = monotonicMs;
    m_haveMonotonic = true;

    if (!m_cpuFreqPaths.isEmpty()) {
        qint64 total = 0;
        int counted = 0;
        for (const QString &path : m_cpuFreqPaths) {
            if (const auto khz = m_sysfs.readNumber(path)) {
                total += *khz;
                ++counted;
            }
        }
        if (counted > 0)
            sample.cpuMhz = clampToU16(total / counted / 1000);
        else
            note(false);
    } else {
        note(false);
    }

    if (!m_gpuFreqPath.isEmpty()) {
        if (const auto mhz = m_sysfs.readNumber(m_gpuFreqPath))
            sample.gpuMhz = clampToU16(*mhz);
    }

    if (const auto text = m_sysfs.readText(QStringLiteral("proc/loadavg"))) {
        const QStringList parts = text->split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (!parts.isEmpty()) {
            bool ok = false;
            const double load = parts.first().toDouble(&ok);
            if (ok)
                sample.load1 = clampToU16(static_cast<qint64>(load * 100.0 + 0.5));
        }
    }

    uint64_t packageThrottle = 0;
    if (!m_packageThrottlePath.isEmpty()) {
        if (const auto value = m_sysfs.readNumber(m_packageThrottlePath))
            packageThrottle = static_cast<uint64_t>(std::max<qint64>(0, *value));
    }
    uint64_t coreThrottle = 0;
    for (const QString &path : m_coreThrottlePaths) {
        if (const auto value = m_sysfs.readNumber(path))
            coreThrottle += static_cast<uint64_t>(std::max<qint64>(0, *value));
    }
    // The counters are cumulative for the life of the boot, so only their rate
    // of change says anything. The first tick has no rate yet.
    if (m_haveThrottleBaseline) {
        sample.pkgThrottle = clampToU16(static_cast<qint64>(
                packageThrottle > m_prevPackageThrottle
                        ? packageThrottle - m_prevPackageThrottle
                        : 0));
        sample.coreThrottle = clampToU16(static_cast<qint64>(
                coreThrottle > m_prevCoreThrottle ? coreThrottle - m_prevCoreThrottle : 0));
    }
    m_prevPackageThrottle = packageThrottle;
    m_prevCoreThrottle = coreThrottle;
    m_haveThrottleBaseline = true;

    if (m_degraded)
        sample.flags |= FlagSensorsDegraded;

    return sample;
}

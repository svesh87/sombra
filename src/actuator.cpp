// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "actuator.h"

#include <QDebug>

namespace {

const QString kNoTurboPath = QStringLiteral("sys/devices/system/cpu/intel_pstate/no_turbo");
const QString kMaxPerfPath = QStringLiteral("sys/devices/system/cpu/intel_pstate/max_perf_pct");
const QString kHwpBoostPath =
        QStringLiteral("sys/devices/system/cpu/intel_pstate/hwp_dynamic_boost");
const QString kPlatformProfilePath = QStringLiteral("sys/firmware/acpi/platform_profile");

// RAPL constraints are expressed in microwatts.
qint64 wattsToMicrowatts(int watts)
{
    return static_cast<qint64>(watts) * 1000000;
}

} // namespace

Actuator::Actuator(Sysfs &sysfs)
    : m_sysfs(sysfs)
{
}

void Actuator::resolve()
{
    m_missing.clear();
    m_written.clear();
    m_vanished = false;

    m_platformProfilePath = m_sysfs.exists(kPlatformProfilePath) ? kPlatformProfilePath : QString();
    if (m_platformProfilePath.isEmpty())
        m_missing.append(kPlatformProfilePath);

    m_raplPackage.clear();
    if (const auto package = m_sysfs.raplDomainByName(QStringLiteral("package-0")))
        m_raplPackage = *package;
    else
        m_missing.append(QStringLiteral("powercap package-0"));

    m_pwmPath.clear();
    if (const auto asus = m_sysfs.hwmonByName(QStringLiteral("asus"))) {
        const QString candidate = *asus + QStringLiteral("/pwm1_enable");
        if (m_sysfs.exists(candidate))
            m_pwmPath = candidate;
    }
    if (m_pwmPath.isEmpty())
        m_missing.append(QStringLiteral("hwmon asus pwm1_enable"));

    m_gtMaxPath.clear();
    const QStringList cards =
            m_sysfs.glob(QStringLiteral("sys/class/drm"), QStringLiteral("card*"));
    for (const QString &card : cards) {
        const QString candidate = card + QStringLiteral("/gt_max_freq_mhz");
        if (m_sysfs.exists(candidate)) {
            m_gtMaxPath = candidate;
            break;
        }
    }

    m_eppPaths.clear();
    const QStringList cpus =
            m_sysfs.glob(QStringLiteral("sys/devices/system/cpu"), QStringLiteral("cpu[0-9]*"));
    for (const QString &cpu : cpus) {
        const QString candidate = cpu + QStringLiteral("/cpufreq/energy_performance_preference");
        if (m_sysfs.exists(candidate))
            m_eppPaths.append(candidate);
    }
    if (m_eppPaths.isEmpty())
        m_missing.append(QStringLiteral("cpufreq energy_performance_preference"));
}

bool Actuator::hasAnyControl() const
{
    return !m_platformProfilePath.isEmpty() || !m_raplPackage.isEmpty() || !m_eppPaths.isEmpty()
            || m_sysfs.exists(kMaxPerfPath);
}

bool Actuator::lastApplyMismatched() const
{
    return m_foreign || m_vanished || m_writeFailed;
}

void Actuator::writeIfDifferent(const QString &path, const QString &value)
{
    // A node this kernel never had is not a failure; it was reported once, at
    // resolve time.
    if (path.isEmpty())
        return;
    if (!m_sysfs.exists(path)) {
        // Resolved and gone since, which happens when a hwmon device
        // re-registers within a boot. Writing into a stale path would fail
        // silently for the rest of the boot, so the daemon is told to
        // re-resolve instead.
        m_vanished = true;
        return;
    }

    const std::optional<QString> current = m_sysfs.readText(path);
    if (current && *current == value) {
        m_written.insert(path, value);
        return;
    }

    // A node holding something other than what this daemon last put there was
    // written by another process. That is the ownership violation the whole
    // architecture exists to avoid, so it is recorded rather than quietly
    // undone. On the first apply there is no memory yet and nothing to detect.
    const auto previous = m_written.constFind(path);
    if (current && previous != m_written.constEnd() && *current != *previous)
        m_foreign = true;

    m_sysfs.writeText(path, value);
    const std::optional<QString> after = m_sysfs.readText(path);
    if (!after || *after != value) {
        m_writeFailed = true;
        return;
    }
    m_written.insert(path, value);
}

void Actuator::writeIfDifferent(const QString &path, qint64 value)
{
    writeIfDifferent(path, QString::number(value));
}

void Actuator::apply(const Setpoints &wanted)
{
    m_foreign = false;
    m_vanished = false;
    m_writeFailed = false;

    Setpoints effective = wanted;
    if (m_raplPackage.isEmpty() && effective.profile == Profile::Quiet) {
        // Without a power limit the quiet curve is the worst of both: the
        // firmware's own limit stands and the EC tolerates more heat before it
        // ramps. A missing power control makes the lazier curve the unsafe
        // choice rather than the conservative one.
        effective.profile = Profile::Balanced;
    }

    // Order is forced by the firmware: writing platform_profile makes it
    // reassert its own RAPL limits, so the profile goes first and the limits
    // after it. Everything downstream of the limits follows.
    writeIfDifferent(m_platformProfilePath, profileToString(effective.profile));

    if (!m_raplPackage.isEmpty()) {
        writeIfDifferent(m_raplPackage + QStringLiteral("/constraint_0_power_limit_uw"),
                         wattsToMicrowatts(effective.pl1W));
        writeIfDifferent(m_raplPackage + QStringLiteral("/constraint_1_power_limit_uw"),
                         wattsToMicrowatts(effective.pl2W));
    }

    writeIfDifferent(kMaxPerfPath, effective.maxPerfPct);
    writeIfDifferent(kNoTurboPath, effective.noTurbo ? 1 : 0);
    writeIfDifferent(kHwpBoostPath, effective.hwpBoost ? 1 : 0);

    const QString epp = eppToString(effective.epp);
    for (const QString &path : m_eppPaths)
        writeIfDifferent(path, epp);

    writeIfDifferent(m_gtMaxPath, effective.gtMaxMhz);

    // The fan is last and is only ever moved by the emergency guard or by an
    // explicit command. Automatic control leaves it on EC control.
    writeIfDifferent(m_pwmPath, effective.fanMode);

    if (m_vanished)
        qWarning() << "a control node disappeared after it was resolved";
    if (m_foreign)
        qWarning() << "a control node holds a value this daemon did not write";
}

void Actuator::restore()
{
    // The fan first and unconditionally: everything else can wait, a fan stuck
    // at full speed cannot.
    if (!m_pwmPath.isEmpty())
        m_sysfs.writeNumber(m_pwmPath, 2);
    apply(safeExitSetpoints());
}

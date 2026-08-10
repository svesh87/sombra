// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#pragma once

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QTemporaryDir>

// Builds a directory that looks like the machine's control surface.
//
// The hwmon indices here are deliberately not the ones on the real machine and
// deliberately not in a helpful order: anything that resolves a sensor by
// number rather than by its `name` file must fail against this tree.
class FakeSysfs {
public:
    FakeSysfs() { build(); }

    QString root() const { return m_dir.path(); }
    bool isValid() const { return m_dir.isValid(); }

    // Plugs a monitor into an external connector: the DRM status the daemon reads
    // to notice it, and the file the fake bus answers from.
    void plugMonitor(const QString &connector, const QString &vendor, const QString &model,
                     const QString &serial, int level) const
    {
        write(FakeSysfs::connector(connector), QStringLiteral("connected"));
        write(monitorFile(connector),
              QStringLiteral("vendor=%1\nmodel=%2\nserial=%3\nconnector=card1-%4\n"
                             "maximum=100\nlevel=%5\n")
                      .arg(vendor, model, serial, connector)
                      .arg(level));
    }

    void write(const QString &relative, const QString &contents) const
    {
        const QString path = m_dir.path() + QLatin1Char('/') + relative;
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            file.write(contents.toUtf8() + '\n');
    }

    QString read(const QString &relative) const
    {
        QFile file(m_dir.path() + QLatin1Char('/') + relative);
        if (!file.open(QIODevice::ReadOnly))
            return {};
        return QString::fromUtf8(file.readAll()).trimmed();
    }

    void remove(const QString &relative) const
    {
        QFile::remove(m_dir.path() + QLatin1Char('/') + relative);
    }

    void makeUnwritable(const QString &relative) const
    {
        QFile::setPermissions(m_dir.path() + QLatin1Char('/') + relative, QFile::ReadOwner);
    }

    // Well-known locations inside the fake tree, so tests do not repeat them.
    static QString coretemp() { return QStringLiteral("sys/class/hwmon/hwmon7"); }
    static QString acpitz() { return QStringLiteral("sys/class/hwmon/hwmon2"); }
    static QString nvme() { return QStringLiteral("sys/class/hwmon/hwmon11"); }
    static QString asus() { return QStringLiteral("sys/class/hwmon/hwmon4"); }
    static QString acpiFan() { return QStringLiteral("sys/class/hwmon/hwmon9"); }
    static QString raplPackage()
    {
        return QStringLiteral("sys/class/powercap/intel-rapl:0");
    }
    static QString raplCore() { return QStringLiteral("sys/class/powercap/intel-rapl:0:0"); }
    static QString raplUncore() { return QStringLiteral("sys/class/powercap/intel-rapl:0:1"); }
    static QString card() { return QStringLiteral("sys/class/drm/card1"); }
    // The `status` node of one display connector, named as the kernel names
    // them: "card<N>-<connector>".
    static QString connector(const QString &name)
    {
        return QStringLiteral("sys/class/drm/card1-%1/status").arg(name);
    }
    // What the fake DDC bus answers from: one file per monitor, named after its
    // connector. Nothing writes it by default — a machine with nothing plugged in
    // is the state the pictures and most of the suite want.
    static QString monitorFile(const QString &connector)
    {
        return QStringLiteral("fake-ddc/card1-%1").arg(connector);
    }
    static QString pstate() { return QStringLiteral("sys/devices/system/cpu/intel_pstate"); }
    static QString platformProfile()
    {
        return QStringLiteral("sys/firmware/acpi/platform_profile");
    }
    static QString cpu(int index)
    {
        return QStringLiteral("sys/devices/system/cpu/cpu%1").arg(index);
    }
    // Deliberately not iio:device0, for the same reason the hwmon indices are
    // not the real ones: nothing may address these by number.
    static QString als() { return QStringLiteral("sys/bus/iio/devices/iio:device3"); }
    static QString backlight() { return QStringLiteral("sys/class/backlight/intel_backlight"); }

    static constexpr int kCpuCount = 4;
    // The panel scale on this machine. Percentages are converted against it,
    // so a test that expects 25 % expects 100 here.
    static constexpr int kBacklightMax = 400;

private:
    void build()
    {
        write(coretemp() + QStringLiteral("/name"), QStringLiteral("coretemp"));
        write(coretemp() + QStringLiteral("/temp1_label"), QStringLiteral("Package id 0"));
        write(coretemp() + QStringLiteral("/temp1_input"), QStringLiteral("61000"));
        write(coretemp() + QStringLiteral("/temp2_label"), QStringLiteral("Core 0"));
        write(coretemp() + QStringLiteral("/temp2_input"), QStringLiteral("59000"));

        write(acpitz() + QStringLiteral("/name"), QStringLiteral("acpitz"));
        write(acpitz() + QStringLiteral("/temp1_input"), QStringLiteral("45000"));

        write(nvme() + QStringLiteral("/name"), QStringLiteral("nvme"));
        write(nvme() + QStringLiteral("/temp1_label"), QStringLiteral("Composite"));
        write(nvme() + QStringLiteral("/temp1_input"), QStringLiteral("38850"));

        write(asus() + QStringLiteral("/name"), QStringLiteral("asus"));
        write(asus() + QStringLiteral("/fan1_input"), QStringLiteral("1900"));
        write(asus() + QStringLiteral("/pwm1_enable"), QStringLiteral("2"));

        write(acpiFan() + QStringLiteral("/name"), QStringLiteral("acpi_fan"));
        write(acpiFan() + QStringLiteral("/fan1_input"), QStringLiteral("1900"));

        write(raplPackage() + QStringLiteral("/name"), QStringLiteral("package-0"));
        write(raplPackage() + QStringLiteral("/energy_uj"), QStringLiteral("1000000"));
        write(raplPackage() + QStringLiteral("/max_energy_range_uj"),
              QStringLiteral("262143328850"));
        write(raplPackage() + QStringLiteral("/constraint_0_power_limit_uw"),
              QStringLiteral("200000000"));
        write(raplPackage() + QStringLiteral("/constraint_1_power_limit_uw"),
              QStringLiteral("115000000"));

        write(raplCore() + QStringLiteral("/name"), QStringLiteral("core"));
        write(raplCore() + QStringLiteral("/energy_uj"), QStringLiteral("400000"));
        write(raplCore() + QStringLiteral("/max_energy_range_uj"), QStringLiteral("262143328850"));

        write(raplUncore() + QStringLiteral("/name"), QStringLiteral("uncore"));
        write(raplUncore() + QStringLiteral("/energy_uj"), QStringLiteral("100000"));
        write(raplUncore() + QStringLiteral("/max_energy_range_uj"),
              QStringLiteral("262143328850"));

        write(card() + QStringLiteral("/gt_act_freq_mhz"), QStringLiteral("300"));
        write(card() + QStringLiteral("/gt_max_freq_mhz"), QStringLiteral("2350"));
        write(card() + QStringLiteral("/gt_min_freq_mhz"), QStringLiteral("100"));

        // Display connectors: the internal panel plus two empty sockets, which
        // is the machine with nothing plugged in.
        write(connector(QStringLiteral("eDP-1")), QStringLiteral("connected"));
        write(connector(QStringLiteral("DP-1")), QStringLiteral("disconnected"));
        write(connector(QStringLiteral("HDMI-A-1")), QStringLiteral("disconnected"));

        write(pstate() + QStringLiteral("/no_turbo"), QStringLiteral("0"));
        write(pstate() + QStringLiteral("/max_perf_pct"), QStringLiteral("100"));
        write(pstate() + QStringLiteral("/min_perf_pct"), QStringLiteral("8"));
        write(pstate() + QStringLiteral("/hwp_dynamic_boost"), QStringLiteral("1"));

        write(platformProfile(), QStringLiteral("performance"));
        write(QStringLiteral("sys/firmware/acpi/platform_profile_choices"),
              QStringLiteral("quiet balanced performance"));

        for (int i = 0; i < kCpuCount; ++i) {
            write(cpu(i) + QStringLiteral("/cpufreq/scaling_cur_freq"), QStringLiteral("2400000"));
            write(cpu(i) + QStringLiteral("/cpufreq/energy_performance_preference"),
                  QStringLiteral("balance_performance"));
            write(cpu(i) + QStringLiteral("/thermal_throttle/core_throttle_count"),
                  QStringLiteral("0"));
            write(cpu(i) + QStringLiteral("/thermal_throttle/package_throttle_count"),
                  QStringLiteral("0"));
        }

        write(QStringLiteral("sys/class/power_supply/AC0/type"), QStringLiteral("Mains"));
        write(QStringLiteral("sys/class/power_supply/AC0/online"), QStringLiteral("1"));
        write(QStringLiteral("sys/class/power_supply/BAT0/type"), QStringLiteral("Battery"));
        write(QStringLiteral("sys/class/power_supply/BAT0/charge_control_end_threshold"),
              QStringLiteral("100"));

        // A second iio device without an illuminance channel, so that a
        // resolver picking the first directory it sees fails here.
        write(QStringLiteral("sys/bus/iio/devices/iio:device1/name"),
              QStringLiteral("accel_3d"));
        write(als() + QStringLiteral("/name"), QStringLiteral("als"));
        write(als() + QStringLiteral("/in_illuminance_raw"), QStringLiteral("1000"));
        write(als() + QStringLiteral("/in_illuminance_scale"), QStringLiteral("0.001000000"));

        write(backlight() + QStringLiteral("/max_brightness"),
              QString::number(kBacklightMax));
        write(backlight() + QStringLiteral("/brightness"),
              QString::number(kBacklightMax / 2));
        write(backlight() + QStringLiteral("/actual_brightness"),
              QString::number(kBacklightMax / 2));

        write(QStringLiteral("proc/loadavg"), QStringLiteral("0.61 0.55 0.48 1/1234 5678"));
    }

    QTemporaryDir m_dir;
};

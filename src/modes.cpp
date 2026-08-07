// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "modes.h"

#include <QFileInfo>
#include <QSettings>

#include <algorithm>

namespace {

// Clamps to a permitted range and records what was changed. Silent clamping
// would leave the maintainer editing a number that has no effect.
int clampReported(int value, int low, int high, const QString &what, QStringList *problems)
{
    const int clamped = std::clamp(value, low, high);
    if (clamped != value && problems) {
        problems->append(QStringLiteral("%1: %2 is outside %3..%4, using %5")
                                 .arg(what)
                                 .arg(value)
                                 .arg(low)
                                 .arg(high)
                                 .arg(clamped));
    }
    return clamped;
}

bool readBool(const QSettings &settings, const QString &key, bool fallback)
{
    const QVariant value = settings.value(key);
    if (!value.isValid())
        return fallback;
    const QString text = value.toString().trimmed().toLower();
    if (text == QLatin1String("on") || text == QLatin1String("true") || text == QLatin1String("1")
        || text == QLatin1String("yes"))
        return true;
    if (text == QLatin1String("off") || text == QLatin1String("false")
        || text == QLatin1String("0") || text == QLatin1String("no"))
        return false;
    return fallback;
}

} // namespace

ModeTable ModeTable::builtinDefaults()
{
    ModeTable table;

    Mode quiet;
    quiet.id = QStringLiteral("quiet");
    quiet.label = QStringLiteral("Quiet");
    quiet.setpoints.profile = Profile::Quiet;
    quiet.setpoints.pl1W = 12;
    quiet.setpoints.pl2W = 20;
    quiet.setpoints.maxPerfPct = 40;
    quiet.setpoints.noTurbo = true;
    quiet.setpoints.epp = Epp::Power;
    quiet.setpoints.hwpBoost = false;
    quiet.setpoints.gtMaxMhz = 1000;

    // The working mode. The machine spends the overwhelming majority of its
    // time here, it is applied at boot, and it is what telemetry tuning
    // targets. Measured under sustained load: 80 °C at 3700 RPM.
    Mode work;
    work.id = QStringLiteral("work");
    work.label = QStringLiteral("Work");
    work.setpoints.profile = Profile::Balanced;
    work.setpoints.pl1W = 20;
    work.setpoints.pl2W = 30;
    work.setpoints.maxPerfPct = 50;
    work.setpoints.noTurbo = true;
    work.setpoints.epp = Epp::Power;
    work.setpoints.hwpBoost = false;
    work.setpoints.gtMaxMhz = kMaxGtMhz;

    Mode full;
    full.id = QStringLiteral("full");
    full.label = QStringLiteral("Full");
    full.setpoints.profile = Profile::Balanced;
    full.setpoints.pl1W = kMaxConfirmedPl1W;
    full.setpoints.pl2W = kMaxConfirmedPl2W;
    full.setpoints.maxPerfPct = 100;
    full.setpoints.noTurbo = false;
    full.setpoints.epp = Epp::BalancePerformance;
    full.setpoints.hwpBoost = true;
    full.setpoints.gtMaxMhz = kMaxGtMhz;

    table.m_modes = { quiet, work, full };
    table.m_defaultModeId = work.id;
    return table;
}

bool ModeTable::load(const QString &path, QStringList *problems)
{
    if (!QFileInfo::exists(path)) {
        if (problems)
            problems->append(QStringLiteral("%1 does not exist, using built-in modes").arg(path));
        return false;
    }

    QSettings settings(path, QSettings::IniFormat);
    if (settings.status() != QSettings::NoError) {
        if (problems)
            problems->append(
                    QStringLiteral("%1 could not be parsed, using built-in modes").arg(path));
        return false;
    }

    const ModeTable defaults = builtinDefaults();
    QList<Mode> parsed;

    const QStringList groups = settings.childGroups();
    for (const QString &group : groups) {
        // Sections are "[mode <id>]". A slash in a section name would be read
        // by QSettings as a group separator, so the separator here is a space.
        if (!group.startsWith(QLatin1String("mode ")))
            continue;
        const QString id = group.mid(5).trimmed();
        if (id.isEmpty()) {
            if (problems)
                problems->append(QStringLiteral("a mode section has an empty name, skipped"));
            continue;
        }

        settings.beginGroup(group);

        Setpoints points;
        const QString profileText =
                settings.value(QStringLiteral("platform_profile"), QStringLiteral("balanced"))
                        .toString();
        points.profile = profileFromString(profileText);
        if (points.profile == Profile::Unknown) {
            if (problems) {
                problems->append(
                        QStringLiteral("mode %1: unknown platform_profile '%2', using balanced")
                                .arg(id, profileText));
            }
            points.profile = Profile::Balanced;
        }

        points.pl1W = clampReported(settings.value(QStringLiteral("pl1_w"), 20).toInt(), 1,
                                    kMaxConfirmedPl1W,
                                    QStringLiteral("mode %1: pl1_w").arg(id), problems);
        points.pl2W = clampReported(settings.value(QStringLiteral("pl2_w"), 30).toInt(),
                                    points.pl1W, kMaxConfirmedPl2W,
                                    QStringLiteral("mode %1: pl2_w").arg(id), problems);
        points.maxPerfPct =
                clampReported(settings.value(QStringLiteral("max_perf_pct"), 50).toInt(),
                              kMinPerfPct, 100,
                              QStringLiteral("mode %1: max_perf_pct").arg(id), problems);
        points.noTurbo = !readBool(settings, QStringLiteral("turbo"), false);

        const QString eppText =
                settings.value(QStringLiteral("epp"), QStringLiteral("power")).toString();
        points.epp = eppFromString(eppText);
        if (points.epp == Epp::Unknown) {
            if (problems) {
                problems->append(
                        QStringLiteral("mode %1: unknown epp '%2', using power").arg(id, eppText));
            }
            points.epp = Epp::Power;
        }

        points.hwpBoost = readBool(settings, QStringLiteral("hwp_dynamic_boost"), false);
        points.gtMaxMhz =
                clampReported(settings.value(QStringLiteral("gt_max_mhz"), kMaxGtMhz).toInt(),
                              kMinGtMhz, kMaxGtMhz,
                              QStringLiteral("mode %1: gt_max_mhz").arg(id), problems);
        // Automatic control never forces the fan. Full speed is reachable only
        // by explicit command, so it is not something a mode can define.
        points.fanMode = 2;

        Mode mode;
        mode.id = id;
        mode.label = settings.value(QStringLiteral("label"), id).toString();
        mode.setpoints = points;
        parsed.append(mode);

        settings.endGroup();
    }

    if (parsed.isEmpty()) {
        if (problems)
            problems->append(QStringLiteral("%1 defines no modes, using built-in modes").arg(path));
        return false;
    }

    // Ordered by long-term power limit, so the table is a scale from the
    // coolest envelope to the widest one. The file's own order cannot be used:
    // QSettings does not preserve it. The order matters because the desktop
    // panel has three slots that map onto this scale, and because a sample
    // records the mode by index.
    std::sort(parsed.begin(), parsed.end(), [](const Mode &a, const Mode &b) {
        if (a.setpoints.pl1W != b.setpoints.pl1W)
            return a.setpoints.pl1W < b.setpoints.pl1W;
        return a.id < b.id;
    });
    m_modes = parsed;

    // QSettings folds a "[general]" section into the root of the file, so the
    // key is read without its section name.
    const QString wanted = settings.value(QStringLiteral("default_mode")).toString().trimmed();
    if (!wanted.isEmpty() && find(wanted)) {
        m_defaultModeId = wanted;
    } else {
        if (!wanted.isEmpty() && problems) {
            problems->append(QStringLiteral("default_mode '%1' is not a defined mode, using %2")
                                     .arg(wanted, m_modes.first().id));
        }
        // The middle slot is the working mode, so a three-mode table without an
        // explicit default lands on its middle entry rather than on the first.
        m_defaultModeId = m_modes.at(m_modes.size() / 2).id;
    }

    return true;
}

const Mode *ModeTable::find(const QString &id) const
{
    for (const Mode &mode : m_modes) {
        if (mode.id == id)
            return &mode;
    }
    return nullptr;
}

int ModeTable::indexOf(const QString &id) const
{
    for (int i = 0; i < m_modes.size(); ++i) {
        if (m_modes.at(i).id == id)
            return i;
    }
    return -1;
}

const Mode *ModeTable::mostConservative() const
{
    const Mode *best = nullptr;
    for (const Mode &mode : m_modes) {
        if (!best || mode.setpoints.pl1W < best->setpoints.pl1W)
            best = &mode;
    }
    return best;
}

QString ModeTable::toConfigSnippet(const QString &id, const Setpoints &setpoints)
{
    return QStringLiteral("[mode %1]\n"
                          "label=%1\n"
                          "platform_profile=%2\n"
                          "pl1_w=%3\n"
                          "pl2_w=%4\n"
                          "max_perf_pct=%5\n"
                          "turbo=%6\n"
                          "epp=%7\n"
                          "hwp_dynamic_boost=%8\n"
                          "gt_max_mhz=%9\n")
            .arg(id, profileToString(setpoints.profile))
            .arg(setpoints.pl1W)
            .arg(setpoints.pl2W)
            .arg(setpoints.maxPerfPct)
            .arg(setpoints.noTurbo ? QStringLiteral("off") : QStringLiteral("on"),
                 eppToString(setpoints.epp),
                 setpoints.hwpBoost ? QStringLiteral("1") : QStringLiteral("0"))
            .arg(setpoints.gtMaxMhz);
}

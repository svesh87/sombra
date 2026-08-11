// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "modes.h"

#include <QFileInfo>
#include <QSettings>

#include <algorithm>
#include <optional>

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

std::optional<bool> parseBool(const QString &value)
{
    const QString text = value.trimmed().toLower();
    if (text == QLatin1String("on") || text == QLatin1String("true") || text == QLatin1String("1")
        || text == QLatin1String("yes"))
        return true;
    if (text == QLatin1String("off") || text == QLatin1String("false")
        || text == QLatin1String("0") || text == QLatin1String("no"))
        return false;
    return std::nullopt;
}

} // namespace

bool applyModeSetting(Mode &mode, const QString &key, const QString &value, QString *error,
                      QStringList *clamped)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    const auto number = [&](int *out) {
        bool ok = false;
        const int parsed = value.trimmed().toInt(&ok);
        if (ok)
            *out = parsed;
        return ok;
    };
    const QString what = QStringLiteral("mode %1: %2").arg(mode.id, key);
    Setpoints &points = mode.setpoints;

    if (key == QLatin1String("label")) {
        mode.label = value.trimmed();
        return true;
    }
    if (key == QLatin1String("platform_profile")) {
        const Profile profile = profileFromString(value.trimmed());
        if (profile == Profile::Unknown)
            return fail(QStringLiteral("%1: unknown platform_profile '%2'").arg(what, value));
        points.profile = profile;
        return true;
    }
    if (key == QLatin1String("epp")) {
        const Epp epp = eppFromString(value.trimmed());
        if (epp == Epp::Unknown)
            return fail(QStringLiteral("%1: unknown epp '%2'").arg(what, value));
        points.epp = epp;
        return true;
    }
    if (key == QLatin1String("turbo")) {
        const auto parsed = parseBool(value);
        if (!parsed)
            return fail(QStringLiteral("%1: '%2' is not on or off").arg(what, value));
        points.noTurbo = !*parsed;
        return true;
    }
    if (key == QLatin1String("hwp_dynamic_boost")) {
        const auto parsed = parseBool(value);
        if (!parsed)
            return fail(QStringLiteral("%1: '%2' is not on or off").arg(what, value));
        points.hwpBoost = *parsed;
        return true;
    }
    if (key == QLatin1String("fan")) {
        // A mode may drive the fan. It could not once, on the grounds that
        // automatic control should never be loud — but the person choosing a
        // mode is not automatic control, and making them reach for a second
        // control to do the obvious thing served the rule rather than them.
        const QString text = value.trimmed().toLower();
        if (text == QLatin1String("max")) {
            points.fanMode = 0;
            return true;
        }
        if (text == QLatin1String("auto")) {
            points.fanMode = 2;
            return true;
        }
        return fail(QStringLiteral("%1: '%2' is not max or auto").arg(what, value));
    }

    int parsed = 0;
    if (key == QLatin1String("pl1_w")) {
        if (!number(&parsed))
            return fail(QStringLiteral("%1: '%2' is not a number").arg(what, value));
        points.pl1W = clampReported(parsed, 1, kMaxConfirmedPl1W, what, clamped);
        return true;
    }
    if (key == QLatin1String("pl2_w")) {
        if (!number(&parsed))
            return fail(QStringLiteral("%1: '%2' is not a number").arg(what, value));
        points.pl2W = clampReported(parsed, 1, kMaxConfirmedPl2W, what, clamped);
        return true;
    }
    if (key == QLatin1String("max_perf_pct")) {
        if (!number(&parsed))
            return fail(QStringLiteral("%1: '%2' is not a number").arg(what, value));
        points.maxPerfPct = clampReported(parsed, kMinPerfPct, 100, what, clamped);
        return true;
    }
    if (key == QLatin1String("gt_max_mhz")) {
        if (!number(&parsed))
            return fail(QStringLiteral("%1: '%2' is not a number").arg(what, value));
        points.gtMaxMhz = clampReported(parsed, kMinGtMhz, kMaxGtMhz, what, clamped);
        return true;
    }

    return fail(QStringLiteral("mode %1: unknown setting '%2'").arg(mode.id, key));
}

void normalizeMode(Mode &mode, QStringList *clamped)
{
    mode.setpoints.pl2W = clampReported(mode.setpoints.pl2W, mode.setpoints.pl1W,
                                        kMaxConfirmedPl2W,
                                        QStringLiteral("mode %1: pl2_w").arg(mode.id), clamped);
}

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

        // Every key goes through the same function the bus uses, so the two
        // ways of setting a value cannot drift apart. What is not in the file
        // keeps the built-in starting value rather than being an error.
        Mode mode;
        mode.id = id;
        mode.label = id;
        for (const QString &key : settings.childKeys()) {
            QString error;
            const QString value = settings.value(key).toStringList().join(QLatin1Char(','));
            if (!applyModeSetting(mode, key.toLower(), value, &error, problems) && problems)
                problems->append(error + QStringLiteral(", ignored"));
        }
        normalizeMode(mode, problems);
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

bool ModeTable::replace(const Mode &mode)
{
    for (int i = 0; i < m_modes.size(); ++i) {
        if (m_modes.at(i).id != mode.id)
            continue;
        m_modes[i] = mode;
        // The table is a scale from the coolest envelope to the widest one, and
        // an edit can move a mode along it. Sorting here keeps the panel's
        // three slots in that order without the caller having to know.
        std::sort(m_modes.begin(), m_modes.end(), [](const Mode &a, const Mode &b) {
            if (a.setpoints.pl1W != b.setpoints.pl1W)
                return a.setpoints.pl1W < b.setpoints.pl1W;
            return a.id < b.id;
        });
        return true;
    }
    return false;
}

bool ModeTable::setDefaultModeId(const QString &id)
{
    if (!find(id))
        return false;
    m_defaultModeId = id;
    return true;
}

QString ModeTable::toConfigSection(const QString &id, const QString &label,
                                   const Setpoints &setpoints)
{
    QString text;
    text += QStringLiteral("[mode %1]\n").arg(id);
    // Quoted, because a label is free text and the parser splits an unquoted
    // value on commas — a label with one in it would come back changed.
    text += QStringLiteral("label=\"%1\"\n").arg(label.isEmpty() ? id : label);
    text += QStringLiteral("platform_profile=%1\n").arg(profileToString(setpoints.profile));
    text += QStringLiteral("pl1_w=%1\n").arg(setpoints.pl1W);
    text += QStringLiteral("pl2_w=%1\n").arg(setpoints.pl2W);
    text += QStringLiteral("max_perf_pct=%1\n").arg(setpoints.maxPerfPct);
    text += QStringLiteral("turbo=%1\n")
                    .arg(setpoints.noTurbo ? QStringLiteral("off") : QStringLiteral("on"));
    text += QStringLiteral("epp=%1\n").arg(eppToString(setpoints.epp));
    text += QStringLiteral("hwp_dynamic_boost=%1\n").arg(setpoints.hwpBoost ? 1 : 0);
    text += QStringLiteral("gt_max_mhz=%1\n").arg(setpoints.gtMaxMhz);
    text += QStringLiteral("fan=%1\n")
                    .arg(setpoints.fanMode == 0 ? QStringLiteral("max") : QStringLiteral("auto"));
    return text;
}

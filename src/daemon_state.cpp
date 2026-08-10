// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "daemon_state.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <algorithm>

namespace {

// Accepts what a person would plausibly type by hand, since the file is plain
// text and nothing stops them editing it.
bool parseBool(const QString &text, bool *ok)
{
    const QString value = text.trimmed().toLower();
    *ok = true;
    if (value == QLatin1String("on") || value == QLatin1String("true")
        || value == QLatin1String("1") || value == QLatin1String("yes"))
        return true;
    if (value == QLatin1String("off") || value == QLatin1String("false")
        || value == QLatin1String("0") || value == QLatin1String("no"))
        return false;
    *ok = false;
    return false;
}

} // namespace

DaemonState DaemonState::load(const QString &path, QStringList *problems)
{
    DaemonState state;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        // A first boot has no state file. That is the normal case, not a
        // problem, so it is reported only when the file exists and cannot be
        // read.
        if (QFileInfo::exists(path) && problems)
            problems->append(QStringLiteral("%1 could not be read").arg(path));
        return state;
    }

    const QStringList lines = QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'));
    for (const QString &raw : lines) {
        const QString line = raw.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;
        const int eq = line.indexOf(QLatin1Char('='));
        if (eq <= 0) {
            if (problems)
                problems->append(QStringLiteral("state: ignoring '%1'").arg(line));
            continue;
        }
        const QString key = line.left(eq).trimmed();
        const QString value = line.mid(eq + 1).trimmed();

        if (key == QLatin1String("mode")) {
            state.mode = value;
        } else if (key == QLatin1String("brightness_profile")) {
            // Read and dropped. The backlight has one set of settings, so
            // there is nothing for a profile name to select; a state file
            // written before that is not a file with a typo in it.
        } else if (key == QLatin1String("auto_brightness")) {
            bool ok = false;
            const bool parsed = parseBool(value, &ok);
            if (!ok) {
                if (problems) {
                    problems->append(QStringLiteral("state: auto_brightness '%1' is not a "
                                                    "boolean, leaving it off")
                                             .arg(value));
                }
                continue;
            }
            state.autoBrightness = parsed;
        } else if (key == QLatin1String("brightness_base")) {
            bool ok = false;
            const int parsed = value.toInt(&ok);
            if (!ok || parsed < 0 || parsed > 100) {
                if (problems) {
                    problems->append(
                            QStringLiteral("state: brightness_base '%1' is not a percentage, "
                                           "taking the panel's current level")
                                    .arg(value));
                }
                continue;
            }
            state.brightnessBase = parsed;
        } else if (key == QLatin1String("charge_limit")) {
            bool ok = false;
            const int parsed = value.toInt(&ok);
            if (!ok || parsed < 0 || parsed > 100) {
                if (problems) {
                    problems->append(QStringLiteral("state: charge_limit '%1' is not a "
                                                    "percentage, leaving the threshold alone")
                                             .arg(value));
                }
                continue;
            }
            state.chargeLimit = parsed;
        } else if (problems) {
            problems->append(QStringLiteral("state: unknown key '%1'").arg(key));
        }
    }

    return state;
}

bool DaemonState::save(const QString &path) const
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        return false;

    QString text;
    text += QStringLiteral("# Written by ombrad. Durable mode definitions live in the\n"
                           "# configuration file; this holds what a person chose at runtime.\n");
    text += QStringLiteral("mode=%1\n").arg(mode);
    text += QStringLiteral("auto_brightness=%1\n")
                    .arg(autoBrightness ? QStringLiteral("on") : QStringLiteral("off"));
    if (brightnessBase != kBrightnessBaseUnset)
        text += QStringLiteral("brightness_base=%1\n").arg(std::clamp(brightnessBase, 0, 100));
    if (chargeLimit != kChargeLimitUnset)
        text += QStringLiteral("charge_limit=%1\n").arg(std::clamp(chargeLimit, 0, 100));

    return file.write(text.toUtf8()) >= 0;
}

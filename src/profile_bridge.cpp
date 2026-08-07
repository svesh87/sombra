// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "profile_bridge.h"

namespace profile_bridge {

QStringList standardProfiles()
{
    return { QStringLiteral("power-saver"), QStringLiteral("balanced"),
             QStringLiteral("performance") };
}

QString modeForProfile(const ModeTable &modes, const QString &profile)
{
    const QList<Mode> &table = modes.modes();
    if (table.isEmpty())
        return {};

    if (profile == QLatin1String("power-saver"))
        return table.first().id;
    if (profile == QLatin1String("performance"))
        return table.last().id;
    if (profile == QLatin1String("balanced")) {
        // The working mode, which is where the machine spends its time. It is
        // the configured default when that is not one of the two extremes.
        const QString wanted = modes.defaultModeId();
        if (wanted != table.first().id && wanted != table.last().id && modes.find(wanted))
            return wanted;
        return table.at(table.size() / 2).id;
    }
    return {};
}

QString profileForMode(const ModeTable &modes, const QString &modeId)
{
    const QList<Mode> &table = modes.modes();
    const int index = modes.indexOf(modeId);
    if (index < 0)
        return {};
    if (table.size() == 1)
        return QStringLiteral("balanced");
    if (index == 0)
        return QStringLiteral("power-saver");
    if (index == table.size() - 1)
        return QStringLiteral("performance");
    return QStringLiteral("balanced");
}

} // namespace profile_bridge

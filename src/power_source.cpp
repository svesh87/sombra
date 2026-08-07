// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "power_source.h"

#include <algorithm>

PowerSource::PowerSource(Sysfs &sysfs)
    : m_sysfs(sysfs)
{
}

void PowerSource::resolve()
{
    m_onlinePath.clear();
    m_chargeLimitPath.clear();

    const QStringList supplies =
            m_sysfs.glob(QStringLiteral("sys/class/power_supply"), QStringLiteral("*"));
    for (const QString &supply : supplies) {
        const auto type = m_sysfs.readText(supply + QStringLiteral("/type"));
        const QString online = supply + QStringLiteral("/online");
        if (m_onlinePath.isEmpty() && type && *type == QLatin1String("Mains")
            && m_sysfs.exists(online)) {
            m_onlinePath = online;
        }
        const QString threshold = supply + QStringLiteral("/charge_control_end_threshold");
        if (m_chargeLimitPath.isEmpty() && m_sysfs.exists(threshold))
            m_chargeLimitPath = threshold;
    }
}

bool PowerSource::onAc() const
{
    if (m_onlinePath.isEmpty())
        return false;
    const auto value = m_sysfs.readNumber(m_onlinePath);
    return value && *value != 0;
}

std::optional<int> PowerSource::chargeLimit() const
{
    if (m_chargeLimitPath.isEmpty())
        return std::nullopt;
    const auto value = m_sysfs.readNumber(m_chargeLimitPath);
    if (!value)
        return std::nullopt;
    return static_cast<int>(*value);
}

bool PowerSource::setChargeLimit(int percent)
{
    if (m_chargeLimitPath.isEmpty())
        return false;
    return m_sysfs.writeNumber(m_chargeLimitPath, std::clamp(percent, 20, 100));
}

// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "backlight.h"

#include <QDebug>

#include <algorithm>

Backlight::Backlight(Sysfs &sysfs)
    : m_sysfs(sysfs)
{
}

void Backlight::resolve()
{
    m_devicePath.clear();
    m_brightnessPath.clear();
    m_maxRaw = 0;
    m_lastKnownRaw.reset();

    const QStringList devices =
            m_sysfs.glob(QStringLiteral("sys/class/backlight"), QStringLiteral("*"));
    for (const QString &device : devices) {
        const QString brightness = device + QStringLiteral("/brightness");
        const auto max = m_sysfs.readNumber(device + QStringLiteral("/max_brightness"));
        // A panel whose scale has no top has no percentages either, so it is
        // no use here even if the node exists.
        if (!m_sysfs.exists(brightness) || !max || *max <= 0)
            continue;
        m_devicePath = device;
        m_brightnessPath = brightness;
        m_maxRaw = static_cast<int>(std::min<qint64>(*max, 1000000));
        qInfo().noquote() << "backlight:" << m_sysfs.absolute(device) << "max" << m_maxRaw;
        return;
    }
}

std::optional<int> Backlight::readRaw() const
{
    if (m_brightnessPath.isEmpty())
        return std::nullopt;
    const auto value = m_sysfs.readNumber(m_brightnessPath);
    if (!value)
        return std::nullopt;
    return static_cast<int>(std::clamp<qint64>(*value, 0, m_maxRaw));
}

std::optional<int> Backlight::readPercent() const
{
    const auto raw = readRaw();
    if (!raw)
        return std::nullopt;
    return toPercent(*raw);
}

bool Backlight::writeRaw(int raw)
{
    if (m_brightnessPath.isEmpty())
        return false;
    const int wanted = std::clamp(raw, 0, m_maxRaw);
    if (!m_sysfs.writeNumber(m_brightnessPath, wanted))
        return false;
    m_lastKnownRaw = wanted;
    return true;
}

int Backlight::toPercent(int raw) const
{
    if (m_maxRaw <= 0)
        return 0;
    return static_cast<int>((static_cast<qint64>(std::clamp(raw, 0, m_maxRaw)) * 100 + m_maxRaw / 2)
                            / m_maxRaw);
}

int Backlight::toRaw(int percent) const
{
    return static_cast<int>((static_cast<qint64>(std::clamp(percent, 0, 100)) * m_maxRaw + 50)
                            / 100);
}

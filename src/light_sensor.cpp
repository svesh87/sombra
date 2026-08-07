// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "light_sensor.h"

#include <QDebug>

#include <algorithm>

LightSensor::LightSensor(Sysfs &sysfs)
    : m_sysfs(sysfs)
{
}

void LightSensor::resolve()
{
    m_devicePath.clear();
    m_rawPath.clear();

    const QStringList devices =
            m_sysfs.glob(QStringLiteral("sys/bus/iio/devices"), QStringLiteral("iio:device*"));
    for (const QString &device : devices) {
        const QString raw = device + QStringLiteral("/in_illuminance_raw");
        if (!m_sysfs.exists(raw))
            continue;
        m_devicePath = device;
        m_rawPath = raw;
        const auto name = m_sysfs.readText(device + QStringLiteral("/name"));
        qInfo().noquote() << "ambient light sensor:" << m_sysfs.absolute(device)
                          << (name ? *name : QStringLiteral("unnamed"));
        return;
    }
}

std::optional<int> LightSensor::read() const
{
    if (m_rawPath.isEmpty())
        return std::nullopt;
    const auto value = m_sysfs.readNumber(m_rawPath);
    if (!value)
        return std::nullopt;
    // Negative is not a light level, and the telemetry field is 16 bits wide
    // with its top value reserved for "absent".
    return static_cast<int>(std::clamp<qint64>(*value, 0, 65534));
}

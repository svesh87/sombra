// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "light_sensor.h"

#include "sample.h"

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
        // Both are optional in the industrial-I/O interface; a channel without
        // them reports a raw count that is already its unit.
        m_scale = 1.0;
        m_offset = 0.0;
        if (const auto text = m_sysfs.readText(device + QStringLiteral("/in_illuminance_scale"))) {
            bool ok = false;
            const double value = text->toDouble(&ok);
            if (ok && value > 0)
                m_scale = value;
        }
        if (const auto text = m_sysfs.readText(device + QStringLiteral("/in_illuminance_offset"))) {
            bool ok = false;
            const double value = text->toDouble(&ok);
            if (ok)
                m_offset = value;
        }
        const auto name = m_sysfs.readText(device + QStringLiteral("/name"));
        qInfo().noquote() << "ambient light sensor:" << m_sysfs.absolute(device)
                          << (name ? *name : QStringLiteral("unnamed"));
        return;
    }
}

std::optional<quint32> LightSensor::read() const
{
    if (m_rawPath.isEmpty())
        return std::nullopt;
    const auto value = m_sysfs.readNumber(m_rawPath);
    if (!value)
        return std::nullopt;
    // (raw + offset) * scale is lux; a thousand of those is the millilux the
    // record holds. Negative is not a light level.
    const double millilux = (static_cast<double>(*value) + m_offset) * m_scale * 1000.0;
    return static_cast<quint32>(
            std::clamp<double>(millilux, 0.0, static_cast<double>(kAbsentLight - 1)));
}

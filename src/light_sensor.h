// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#pragma once

#include "sysfs.h"

#include <QString>

#include <optional>

// The ambient light sensor, reached through the industrial-I/O subsystem.
//
// What it gives: a single number that rises with the light falling on the
// panel bezel, updated far faster than anything here reads it.
//
// What it does not give: calibrated lux. The driver on this machine exposes
// `in_illuminance_raw` with a scale of 0.001, and the product of the two lands
// two orders of magnitude below what a light meter reports for the same room —
// the sensor sits behind dark glass and nothing corrects for that. Calling the
// result lux would be a claim of accuracy that is not there, so the reading is
// reported in the sensor's own units and the brightness curve in the
// configuration is calibrated against them, from recorded telemetry, on this
// machine.
//
// The device is located by the reading it offers rather than by its index,
// because iio device numbers move between boots the way hwmon indices do. Its
// `name` is read only for the log line: drivers disagree about what to put
// there, and a device exposing an illuminance channel is a light sensor
// whatever it calls itself.
class LightSensor {
public:
    explicit LightSensor(Sysfs &sysfs);

    // Safe to call again: it simply re-resolves. A sensor that is absent
    // leaves this object inert rather than failing.
    void resolve();

    bool available() const { return !m_rawPath.isEmpty(); }

    // Absent when there is no sensor or the node could not be read. The caller
    // must not treat that as darkness — with no reading the automation holds
    // the backlight where it is.
    std::optional<int> read() const;

    // The device directory, relative to the sysfs root, for diagnostics.
    const QString &devicePath() const { return m_devicePath; }

private:
    Sysfs &m_sysfs;
    QString m_devicePath;
    QString m_rawPath;
};

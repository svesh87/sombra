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
// The reading is in millilux, which is lux with the dark end kept: the sensor
// reports tenths of a lux in a dark room, and rounding those to whole lux
// throws away the range the eye is most sensitive in.
//
// The conversion is the industrial-I/O one — `(raw + offset) * scale` gives the
// channel's own unit, which for an illuminance channel is lux. On this machine
// the scale is 0.001, so the raw number happens to be millilux already; that
// coincidence is not relied on, the scale is read.
//
// This was got wrong once, and expensively: the scale attribute was not read,
// the raw number was compared against a light meter, and the conclusion written
// down was that the sensor's units are "two orders of magnitude below lux and
// nothing corrects for that". The units were lux all along, a thousand times
// over.
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

    // Millilux, or absent when there is no sensor or the node could not be
    // read. The caller must not treat absence as darkness — with no reading the
    // automation holds the backlight where it is.
    //
    // Nothing is clamped here beyond the storage width. The sensor on this
    // machine tops out around 20 855 lux, measured by pointing a bicycle lamp
    // at it, and an ordinary room with the ceiling light on already reads 103
    // lux — which is why the reading is carried in more than sixteen bits.
    std::optional<quint32> read() const;

    // The device directory, relative to the sysfs root, for diagnostics.
    const QString &devicePath() const { return m_devicePath; }

private:
    Sysfs &m_sysfs;
    QString m_devicePath;
    QString m_rawPath;
    // Read once at resolve(): the channel's scale and offset, as industrial-I/O
    // defines them. A driver that publishes neither means a raw count is
    // already the unit, which is what the defaults say.
    double m_scale = 1.0;
    double m_offset = 0.0;
};

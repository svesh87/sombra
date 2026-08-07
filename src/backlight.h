// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#pragma once

#include "sysfs.h"

#include <QString>

#include <optional>

// The panel backlight.
//
// Two things about the node decide the shape of this class.
//
// It is writable only by root, so the daemon owns it and the tray cannot touch
// it: everything the tray does to the backlight goes through the daemon.
//
// It is writable by anyone else who is root, and desktops do that. So this
// class remembers the last level it knows about — every value it wrote, plus
// whatever it was told to note. A reading that differs from that memory is
// somebody else's write, and in practice that somebody is the person on the
// brightness keys. That is the whole detection mechanism for "a hand
// correction happened", and it is the same one the thermal actuator uses on
// its own nodes.
//
// The memory is what was *known*, not what was *written*, because the two
// differ at the one moment that matters: before the first write there is
// nothing written, and treating that as an unknown level would make every
// tick look like a hand correction.
//
// The scale is the panel's own, 0..max_brightness with max_brightness 400 on
// this machine. Everything above this class works in percent, because that is
// what a person sets and what a configuration file can state; the conversion
// lives here and nowhere else.
class Backlight {
public:
    explicit Backlight(Sysfs &sysfs);

    void resolve();

    bool available() const { return !m_brightnessPath.isEmpty(); }
    int maxRaw() const { return m_maxRaw; }

    // The requested level, not `actual_brightness`: the latter reports what
    // the hardware has reached, which during a fade is not what was asked for.
    std::optional<int> readRaw() const;
    std::optional<int> readPercent() const;

    // Clamped to the panel's range. Returns false when the node is absent or
    // the write did not take.
    bool writeRaw(int raw);

    // The last level this object knows about, absent until it has written or
    // been told one.
    std::optional<int> lastKnownRaw() const { return m_lastKnownRaw; }

    // Adopts a level as known without writing it. Used for the level found on
    // the panel at startup and for one somebody else set.
    void noteObserved(int raw) { m_lastKnownRaw = raw; }

    int toPercent(int raw) const;
    int toRaw(int percent) const;

    const QString &devicePath() const { return m_devicePath; }

private:
    Sysfs &m_sysfs;
    QString m_devicePath;
    QString m_brightnessPath;
    int m_maxRaw = 0;
    std::optional<int> m_lastKnownRaw;
};

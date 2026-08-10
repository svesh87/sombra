// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#pragma once

#include <QString>
#include <QStringList>

// What the daemon has to remember across a reboot.
//
// This is deliberately not the configuration file. The configuration holds
// durable definitions written by the maintainer and is never written by the
// daemon, so the comments in it survive; this file holds the choices a person
// made at runtime and is rewritten whenever one of them changes.
//
// What is here is here because forgetting it would either fight the person or
// lose an obligation:
//
//   mode                selecting a mode is a deliberate act
//   auto_brightness     something switched off must not come back on its own
//   brightness_profile  likewise a deliberate choice
//   brightness_base     the level the person set; without it the automation
//                       would have to be told again after every boot
//   charge_limit        the threshold lives in sysfs and is lost on reboot,
//                       and holding it is an obligation inherited together
//                       with sole ownership of the power controls
//
// What is deliberately absent: live tuning, which applies immediately and is
// never written back anywhere, and the forced maximum fan speed, because 6100
// RPM surviving a reboot is a surprise rather than restored state.
struct DaemonState {
    // Percent, or kChargeLimitUnset when the daemon has never been told one.
    static constexpr int kChargeLimitUnset = 0;
    // Percent, or kBrightnessBaseUnset before the first reading of the panel.
    static constexpr int kBrightnessBaseUnset = -1;

    QString mode;
    bool autoBrightness = false;
    int brightnessBase = kBrightnessBaseUnset;
    int chargeLimit = kChargeLimitUnset;

    // Missing file, unknown key or unparsable value all yield the default for
    // that field and a line in `problems`. One typo must not leave the machine
    // without a mode.
    static DaemonState load(const QString &path, QStringList *problems = nullptr);

    // Creates the directory if needed. Returns false when the file could not
    // be written, which the caller logs: an unwritable state file costs the
    // next boot its settings, not this one its cooling.
    bool save(const QString &path) const;
};

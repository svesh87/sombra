// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#pragma once

#include "setpoints.h"

#include <QList>
#include <QString>
#include <QStringList>

struct Mode {
    QString id;
    QString label;
    Setpoints setpoints;
};

// Applies one "key=value" of the settings vocabulary to a mode.
//
// One function for both ways in, so that a value the file would refuse cannot
// be written through the bus either. The vocabulary is the file's: `pl1_w`,
// not the `pl1` of live tuning, because what is typed at the daemon and what
// ends up in the file are then one language.
//
// An unknown key or an unreadable value is refused through `error`. A number
// outside its permitted range is clamped instead, with a line in `clamped`:
// one typo must not leave the machine unmanaged, and silent clamping would
// leave someone editing a number that has no effect.
bool applyModeSetting(Mode &mode, const QString &key, const QString &value, QString *error,
                      QStringList *clamped);

// Enforces what one key cannot know on its own: PL2 is never below PL1. Called
// after a set of assignments, because the two arrive in no particular order.
void normalizeMode(Mode &mode, QStringList *clamped);

// The mode table, held in the daemon's settings file.
//
// The daemon owns that file: it reads it at startup and rewrites it whenever a
// definition changes. Editing goes through the daemon — `sombra config` and the
// tray both send it the same commands — because the numbers are tuned by
// analysing recorded telemetry and the tooling that does it must be able to
// apply what it found without root and without a text editor.
//
// The file is therefore machine-written and carries no hand comments; the
// built-in defaults below are what it is compared against and what a reset
// restores.
//
// There are exactly three modes and they are edited in place. The desktop
// panel has three slots, and a fourth definition would exist only where the
// panel cannot reach it.
class ModeTable {
public:
    // The starting table, used when no settings file exists and what a reset
    // returns to. The numbers come from measurement on this machine, not from
    // taste.
    static ModeTable builtinDefaults();

    // Replaces the table with the file's contents. Returns false and leaves
    // the table untouched if the file defines no usable mode. Values outside
    // the permitted range are clamped and reported through `problems` rather
    // than rejecting the whole file: a typo in one mode must not leave the
    // machine unmanaged.
    bool load(const QString &path, QStringList *problems = nullptr);

    const QList<Mode> &modes() const { return m_modes; }
    const Mode *find(const QString &id) const;
    int indexOf(const QString &id) const;

    // Replaces the definition of an existing mode. False when no mode carries
    // that id: modes are edited, never created, so an unknown id is a mistake
    // to report rather than a new entry to add.
    bool replace(const Mode &mode);

    // The mode applied at boot, and the one the panel's middle slot selects.
    const QString &defaultModeId() const { return m_defaultModeId; }
    bool setDefaultModeId(const QString &id);

    // The mode with the lowest long-term power limit. Used when sensors are
    // unavailable: the fallback is the most conservative mode, never the last
    // known one.
    const Mode *mostConservative() const;

    // One "[mode <id>]" section, as it appears in the settings file. This is
    // the writing half of load(): whatever it produces has to parse back into
    // the same values, which is what the round-trip test asserts.
    static QString toConfigSection(const QString &id, const QString &label,
                                   const Setpoints &setpoints);

private:
    QList<Mode> m_modes;
    QString m_defaultModeId;
};

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

// The mode table, read from the daemon's configuration file.
//
// The configuration file is the single source of truth for durable mode
// definitions. The reason is not convenience: these numbers are tuned by
// analysing recorded telemetry, and tooling can edit a text file but cannot
// operate a dialog. The daemon only ever reads the file, so the maintainer's
// comments survive.
class ModeTable {
public:
    // The starting table, used when no configuration file exists. The numbers
    // come from measurement on this machine, not from taste.
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

    // The mode applied at boot, and the one the panel's middle slot selects.
    const QString &defaultModeId() const { return m_defaultModeId; }

    // The mode with the lowest long-term power limit. Used when sensors are
    // unavailable: the fallback is the most conservative mode, never the last
    // known one.
    const Mode *mostConservative() const;

    // The configuration text that would reproduce a set of live values, for
    // the tray's "copy as a configuration snippet". Promoting a live setting
    // into the file stays a deliberate edit by the maintainer.
    static QString toConfigSnippet(const QString &id, const Setpoints &setpoints);

private:
    QList<Mode> m_modes;
    QString m_defaultModeId;
};

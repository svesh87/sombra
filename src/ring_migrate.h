// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#pragma once

#include "sample.h"

#include <QList>
#include <QString>

#include <cstdint>

// Carrying recorded samples across a change of record layout.
//
// A telemetry ring is not a database, but it is a week of a machine's life and
// several evenings of tuning, and the layout changes for reasons that have
// nothing to do with the samples already in it — a field widened, a field
// dropped. Discarding them costs the one thing the project cannot regenerate.
//
// The shape is a chain, not a table of pairs: each step converts one version
// into the next, is written once and is then frozen. Only the last step changes
// when the current layout changes, which is what keeps the cost of a new
// version at one struct and one function.
//
// A layout that was never released is not in the chain. Version 3 existed only
// in a working tree; a file claiming it is treated like any other unreadable
// one — set aside, not converted and not discarded.

// Records the file held, converted into the current layout, oldest first.
// Empty when the file is not a ring, or is one no chain of steps reaches. A
// file already in the current layout is read as it stands, which is what lets
// a ring change size without losing what is in it.
//
// Never-written slots are dropped rather than carried: they are an artefact of
// the geometry, and a converted ring starts from what was actually recorded.
QList<Sample> migratedSamples(const QString &path, QString *note);

// True for a version this can convert into the current layout.
bool canMigrateRing(uint16_t version);

// Moves a file out of the way and returns the name it was given. Used for a
// ring this build cannot read: a version from a newer build after a rollback, a
// layout that was never released, or a file whose header is damaged. The bytes
// stay on disk, so a converter written later still has something to read, and
// the daemon carries on with a fresh ring rather than stopping.
//
// One slot, replaced every time. Successive rollbacks would otherwise each
// leave a file behind, and the middle ones are of no use to anybody.
QString setRingAside(const QString &path);

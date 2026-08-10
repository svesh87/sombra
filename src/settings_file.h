// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#pragma once

#include "brightness_map.h"
#include "modes.h"

#include <QString>

// The daemon's settings file: mode definitions and brightness profiles, in one
// INI, written whole.
//
// Both tables load themselves from this path already. Writing is one operation
// rather than two because the file holds both lists, and a table that wrote
// only its own sections would erase the other's.
//
// The file is machine-written and says so in its first line. Comments do not
// survive a rewrite, which is the price of the daemon owning the file — and the
// daemon owns it so that a script analysing telemetry can apply what it found
// through `ombra config`, without root and without a text editor.
namespace settings_file {

// The text of a complete file. Separate from save() so that the round-trip is
// testable without touching a filesystem.
QString serialize(const ModeTable &modes, const BrightnessSettings &brightness);

// Whole or not at all: an interrupted write must not leave the daemon with a
// file it will refuse to parse at the next boot. The directory is created if
// needed. False means nothing was written, and the reason goes to `error`.
bool save(const QString &path, const ModeTable &modes, const BrightnessSettings &brightness,
          QString *error = nullptr);

} // namespace settings_file

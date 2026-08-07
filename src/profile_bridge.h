// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#pragma once

#include "modes.h"

#include <QString>
#include <QStringList>

// Translation between the desktop's three fixed profile names and this
// project's modes.
//
// The panel's three slots are redefined as this project's modes rather than
// reproduced: the mode table is an ordered scale of thermal envelope, so the
// coolest mode answers to "power-saver", the widest to "performance", and the
// working mode in between to "balanced". Nothing here reproduces vendor
// behaviour — that behaviour is the problem the project exists to fix.
namespace profile_bridge {

// The names the standard interface is allowed to use, in the order the
// specification defines them.
QStringList standardProfiles();

// Mode id for a standard profile name, or an empty string when the name is not
// one of the three. Degrades sensibly for tables that do not have exactly
// three modes: with one mode every profile maps to it, with two the middle
// slot follows the working mode.
QString modeForProfile(const ModeTable &modes, const QString &profile);

// The profile name that best describes a mode. The inverse of the above for
// three-mode tables; for larger tables the middle band all reads as
// "balanced".
QString profileForMode(const ModeTable &modes, const QString &modeId);

} // namespace profile_bridge

// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#pragma once

#include <QIcon>

// The panel icon, drawn here and borrowing nothing from its surroundings.
//
// Publishing a standard theme name would be the polite thing to do: the panel
// would draw the glyph of the icon theme in force and recolour it to match
// itself. It does not work for what this icon has to say. A StatusNotifierItem
// host prefers the name over any picture sent with it and draws a placeholder for
// a name it cannot resolve, and it resolves names in its own icon theme — the
// panel here reaches nothing that its theme does not carry, while Qt follows that
// theme's inheritance and finds `power-profile-*-symbolic` two themes up. So a
// check against Qt's answer is not a check against the panel's, and the panel
// drew an empty square.
//
// The icon themes on this machine carry no trio of glyphs about power at all.
// What they have is trios about volume, screen brightness, microphone
// sensitivity and security, and borrowing one of those would put a sun or a
// speaker in the panel to mean a thermal envelope. A picture that means what it
// shows is worth more than one the panel can recolour.
//
// The colour is fixed rather than taken from the palette, for a related reason:
// the palette belongs to the application, the panel is coloured by the desktop,
// and the two disagree exactly when the desktop is light with a dark bar.
//
// What a glance has to answer: which of the modes is running, and the two states
// that are not modes — no daemon, and the guard clamping.
//
// `modeIndex` is the mode's position in the table, from the coolest envelope to
// the widest, and `modeCount` the size of that table. The fill is a fraction of
// the two, so a table of a different size still reads as a level. A negative
// index means the mode is not known yet.
QIcon trayIcon(bool connected, bool guardEngaged, int modeIndex, int modeCount);

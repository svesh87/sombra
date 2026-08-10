// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#pragma once

#include <QIcon>

// The panel icon.
//
// A standard name from the icon theme where the theme has one, so the panel
// draws the glyph its own theme provides, recolours it to match itself, and
// changes it when the person changes their icon theme. The names are the ones
// desktop power-profile applets use, so a glyph already means what we mean by
// it.
//
// A picture of our own where the theme has nothing. This is not a nicety: a
// StatusNotifierItem host prefers the name over any picture sent with it, and a
// name its theme does not carry becomes a question mark in the panel.
// `power-profile-*-symbolic` ships in Adwaita and in neither the panel's theme
// nor Qt's on this machine, so the name has to be checked rather than assumed.
//
// The check is only worth anything if Qt looks names up in the theme the panel
// uses — see theme::applyDesktopIconTheme(), which is what makes that true.
//
// The drawn picture takes no colour from the palette either. The palette belongs
// to the application and the panel is coloured by the desktop; the two disagree
// exactly when the desktop is light with a dark bar. It is drawn in fixed
// colours that read on both.
//
// What a glance has to answer either way: which of the modes is running, and the
// two states that are not modes — no daemon, and the guard clamping.
//
// `modeIndex` is the mode's position in the table, from the coolest envelope to
// the widest, and `modeCount` the size of that table. The drawn fill is a
// fraction of the two, so a table of a different size still reads as a level. A
// negative index means the mode is not known yet.
QIcon trayIcon(bool connected, bool guardEngaged, int modeIndex, int modeCount);

// The theme name this state would use, or empty when there is none for it. Split
// out so the choice of name can be read and tested without a panel.
QString trayIconName(bool connected, bool guardEngaged, int modeIndex, int modeCount);

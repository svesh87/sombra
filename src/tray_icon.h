// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#pragma once

#include <QIcon>

// The panel icon: the application's own picture, a sun behind a persiana, drawn
// here and borrowing nothing from its surroundings.
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
// The colours are fixed rather than taken from the palette: the palette belongs
// to the application, the panel is coloured by the desktop, and the two disagree
// exactly when the desktop is light with a dark bar. The picture carries its own
// plate for the same reason. A flat glyph would have to be one colour that
// clears both a light and a dark panel, and a warm one does not — the sun this
// icon is built on measures 1.5:1 against a light panel where 3.5 is the floor.
// On a plate the sun is only ever seen against the plate, and the one contrast
// that involves the desktop is the plate's own.
//
// What a glance has to answer, on three independent channels:
//   * the plate says whether the emergency guard is clamping — blue, or red;
//   * the sun says whether the daemon is answering — warm, or grey;
//   * the persiana says which mode is running: shut for the coolest envelope,
//     open for the widest, and absent when there is no mode to show.
//
// The guard is the plate rather than the sun because a red sun and a blue plate
// measure within 1.2:1 of each other and would separate by hue alone, which is
// the one cue that survives neither a small size nor a colour-blind eye.
//
// `modeIndex` is the mode's position in the table, from the coolest envelope to
// the widest, and `modeCount` the size of that table. The opening is a fraction
// of the two, so a table of a different size still reads as a position in it. A
// negative index means the mode is not known yet.
QIcon trayIcon(bool connected, bool guardEngaged, int modeIndex, int modeCount);

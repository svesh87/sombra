// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#pragma once

#include <QIcon>

// The panel icon: the application's own picture, a cloud over a sun, drawn here
// and borrowing nothing from its surroundings.
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
//   * where the cloud sits says which mode is running: the whole of the picture
//     for the coolest envelope, half the sun in between, gone for the widest;
//   * warm or grey says whether the mode is known at all.
//
// The guard is the plate rather than the sun because a red sun and a blue plate
// measure within 1.2:1 of each other and would separate by hue alone, which is
// the one cue that survives neither a small size nor a colour-blind eye.
//
// In the coolest envelope the sun is not drawn — a cloud with rays coming out
// around it is a mode with the light still on — so there that channel is the
// cloud's own colour, near-white while the mode is known and grey when it is not.
// The sun carries it the same way wherever it is drawn, warm against grey.
//
// `modeIndex` is the mode's position in the table, from the coolest envelope to
// the widest, and `modeCount` the size of that table. The ends of the table are
// the ends of the scale, so a table of another size still reads as a position in
// it; there are three modes, so in practice each has a seat of its own.
//
// A negative index, an empty table and a daemon that is not answering are one
// picture: the icon speaks about the mode, and none of the three knows what the
// mode is. Which of them it is belongs to the tooltip and to the console's status
// line, where there are words for it.
QIcon trayIcon(bool connected, bool guardEngaged, int modeIndex, int modeCount);

// The window's icon, which is a different question from the panel's. The panel gets
// the picture above, drawn here and handed over; the title bar, the task switcher and
// the dock read the icon the application declares, and a application that declares
// none gets whatever the shell substitutes.
//
// This one is the installed file rather than a drawing, because a dock takes it from
// the desktop entry and the entry names an icon in the theme. `SOMBRA_ICONS_DIR`
// puts a directory ahead of the theme's own, which is what makes a run from a build
// tree show that tree's picture instead of the installed copy of an older one; with
// nothing set, the theme answers and the override changes nothing.
QIcon applicationIcon();

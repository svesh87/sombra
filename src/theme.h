// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#pragma once

#include <QObject>
#include <QPalette>
#include <QString>

#include <functional>
#include <optional>

// Following the desktop's light or dark preference.
//
// The source is the freedesktop appearance portal —
// `org.freedesktop.portal.Settings`, key `org.freedesktop.appearance` /
// `color-scheme` — and not Qt's own idea of the theme. Two reasons, and both
// are about this machine rather than about taste: QStyleHints::colorScheme()
// arrived in Qt 6.5 and the Qt here is 6.4, and the platform theme in use
// (`qt5ct`) takes its colours from its own configuration and never looks at the
// desktop at all. The portal answers the question that was actually asked.
//
// Following it while the console is open takes assembling the palette here, and
// two measurements on this machine say why. A platform theme does not update a
// running application's palette when the desktop's theme changes — the file
// watcher is there and the colours do not move — and the desktop switches its
// theme on its own, at sunrise and sunset. A console that only read the scheme at
// startup would sit in yesterday's colours until it was closed and opened.
//
// So the colours are read from where the desktop wrote them for Qt: the colour
// scheme named by `qt6ct`'s or `qt5ct`'s configuration, which is a file of
// twenty-one colours per group in QPalette's own role order. On this machine
// `cosmic-settings-daemon` generates it from the desktop's theme and rewrites it
// on every switch. Where there is no such file, the portal's scheme picks one of
// the two palettes below instead.
//
// An explicit choice of light or dark pins the palette: Qt stops taking one from
// the platform theme once an application has set its own, which is why `System`
// hands it back before deciding anything.
namespace theme {

// What the person asked for. `System` is the default and the only value that
// looks at the portal at all; the other two are a deliberate override, because a
// panel-and-window combination the desktop calls light is not always the one
// somebody wants to look at.
enum class Preference {
    System,
    Light,
    Dark,
};

// The stored choice, from the console's own settings file. Not the daemon's:
// this is about the window rather than about the machine, and the daemon has no
// business holding it.
Preference preference();
void setPreference(Preference wanted);

// Parsing and printing the choice, for the settings file and for tests.
QString nameOf(Preference wanted);
Preference preferenceFrom(const QString &name);

// Applies the stored choice: the desktop's own colours when it is `System`, and
// one of the two palettes below otherwise. Returns true when the palette changed.
bool applyPreference();

// The palette the desktop wrote for Qt applications, or nothing when it wrote
// none. Read fresh on every call, because the point of it is to be re-read when
// the desktop switches theme under a running console.
std::optional<QPalette> desktopPalette();

// Calls `onChange` on any setting the appearance portal announces. The context
// object owns the subscription; when it dies, so does the watch.
void watch(QObject *context, std::function<void()> onChange);

enum class Scheme {
    // The portal is absent or answered something unexpected. Nothing is
    // applied: a desktop that will not say what it wants is not a desktop
    // asking for our colours.
    Unknown,
    Light,
    Dark,
};

// Reads the portal. Blocking, with a short timeout: this runs once at startup
// and on a signal, and a portal that does not answer must not hold the window.
Scheme desktopScheme();

// Which of the two a palette is, judged by the luminance of its window colour.
// This is how "does the current palette already agree" is answered without
// knowing which platform theme produced it.
Scheme schemeOf(const QPalette &palette);

// A palette for a scheme, built on the neutral greys a Fusion-styled window
// expects. Used only when the platform theme's own palette is of the other
// scheme.
QPalette paletteFor(Scheme scheme);

// Applies the desktop's scheme to the application if the current palette is of
// the other one. Returns true when the palette was changed.
bool applyDesktopScheme();

// Applies a named scheme on the same terms: only when the current palette is of
// the other one, because replacing a palette that already agrees would make the
// window the odd one out on the machine.
bool applyScheme(Scheme scheme);

} // namespace theme

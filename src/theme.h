// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#pragma once

#include <QObject>
#include <QPalette>
#include <QString>

#include <functional>

// Following the desktop's appearance: the light or dark preference, and the icon
// theme.
//
// The source is the freedesktop appearance portal —
// `org.freedesktop.portal.Settings`, key `org.freedesktop.appearance` /
// `color-scheme` — and not Qt's own idea of the theme. Two reasons, and both
// are about this machine rather than about taste: QStyleHints::colorScheme()
// arrived in Qt 6.5 and the Qt here is 6.4, and the platform theme in use
// (`qt5ct`) takes its colours from its own configuration and never looks at the
// desktop at all. The portal answers the question that was actually asked.
//
// What is deliberately not done: replacing the platform theme's palette when it
// already agrees with the desktop. A palette of our own would look like nothing
// else on the machine, so it is applied only when the two disagree — which on a
// correctly configured desktop is never, and after a switch to the other scheme
// is immediately.
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

// Applies the stored choice: the desktop's scheme when it is `System`, and the
// named one otherwise. Returns true when the palette changed.
bool applyPreference();

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

// The desktop's icon theme, from the portal's `org.gnome.desktop.interface` /
// `icon-theme`. Empty when the portal is absent or says nothing.
//
// Asked for the same reason as the colour scheme: `qt5ct` names an icon theme of
// its own, and Qt then looks names up in a theme the panel knows nothing about.
// A name that resolves for us and not for the panel is worse than no name — a
// StatusNotifierItem host prefers the name and draws a placeholder when it
// cannot find it. Agreeing with the desktop is what makes "does this name exist"
// a question worth asking.
QString desktopIconTheme();

// Points Qt at the desktop's icon theme. Returns true when the theme changed,
// which is the caller's cue to redraw anything it has already drawn.
bool applyDesktopIconTheme();

// Calls `onChange` whenever the portal reports a different scheme or a different
// icon theme. The context object owns the subscription; when it dies, so does
// the watch.
void watch(QObject *context, std::function<void()> onChange);

} // namespace theme

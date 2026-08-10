// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#pragma once

#include <QLocale>
#include <QString>
#include <QUrl>

// Finding the document behind the console's "?" button.
//
// Every control in the window explains itself on hover, and every entry of every
// list does too. Two things that leaves out: the question somebody has before they
// know what to hover over — what is this program, and what is a mode — and the
// whole of it in one place, which is what somebody comparing two controls wants.
// So the page is a reference: every control of every tab with its units, its range
// and what it costs, plus the model the window is built on. That does not fit in a
// tooltip and it does not fit in a window whose height is already the height of its
// largest tab, so it is a file, opened in whatever the desktop opens documents
// with.
//
// One file per language, chosen the way a catalogue is: the system's language,
// then English. English is not a fallback that apologises — it is the source
// language of everything else here.
//
// The page carries a named section per tab, and the button opens the one for the
// tab in front of the person: asked from the backlight tab, "what does this do"
// means the backlight. The names are the tabs' own internal names — the same ones
// the documentation pictures are called — so they are identical in every language
// and can be used as anchors.
namespace help {

// The page for that language, or an empty string when there is none. Absence is
// not a failure of the program: the console works without it, and the button says
// so instead of doing nothing.
QString path(const QLocale &locale = QLocale::system());

// The page for that language with the section for `tab` selected, ready for
// QDesktopServices. Invalid when there is no page; an unknown section name still
// opens the page, at the top, because the reference is worth more than the jump.
QUrl url(const QString &tab, const QLocale &locale = QLocale::system());

// Where the pages are looked for: `<prefix>/share/sombra/help`, overridden by
// `SOMBRA_HELP_DIR`. The override is what lets the suite point at the source tree
// rather than at an installed copy, exactly as it does for the catalogues.
QString directory();

} // namespace help

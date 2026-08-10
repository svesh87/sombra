// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#pragma once

#include <QLocale>
#include <QString>

class QCoreApplication;

// Loading the console's translations for the language the system is set to.
//
// Only the console is translated. The command line, the daemon's replies and the
// manual pages stay in English: they are read in a terminal, quoted in
// documentation and searched for by their exact wording, and a translated `err`
// line is a line nobody can look up. That shows in one place — a reply shown on
// the status line is English inside a translated window — and it is the honest
// trade rather than an oversight.
//
// Two catalogues are loaded, and the second one matters more than it looks: Qt's
// own `qtbase_<lang>` carries the words in the standard dialogs, so without it a
// Russian window still asks "Yes / No" when the reset button wants confirming.
namespace translations {

// Installs what is available for a language onto the application, replacing
// whatever a previous call installed. Missing catalogues are not an error:
// English is the source language, so a machine with no translation files shows
// the strings as they are written in the code.
//
// The locale is an argument because `QLocale::system()` is read once by Qt and
// does not change with the environment afterwards — which makes a language
// impossible to test unless it can be named outright.
//
// The directory is `<prefix>/share/sombra/translations`, and
// `SOMBRA_TRANSLATIONS_DIR` overrides it — which is how the suite runs against
// freshly built catalogues and how the documentation pictures are taken in
// English on a machine whose language is not.
void install(QCoreApplication *application, const QLocale &locale = QLocale::system());

// Where the catalogues are looked for. Exposed for the tests and for a log line
// worth having when a translation does not appear.
QString directory();

} // namespace translations

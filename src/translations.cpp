// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "translations.h"

#include <QCoreApplication>
#include <QDir>
#include <QList>
#include <QLibraryInfo>
#include <QLocale>
#include <QTranslator>

#include <utility>

namespace {

// Installed beside the binary's data rather than compiled into it, so the
// catalogues are visible in the package and can be replaced without a rebuild.
constexpr const char *kDefaultDirectory = SOMBRA_TRANSLATIONS_PATH;
constexpr const char *kOverride = "SOMBRA_TRANSLATIONS_DIR";

// What this module installed last time, so that installing again replaces it
// rather than stacking another catalogue on top. Qt keeps every translator it is
// given and asks them newest first, so without this a second call in one process
// would leave the first language answering for whatever the second does not
// cover.
QList<QTranslator *> installed;

// Loads one catalogue and hands it to the application, which takes ownership.
// A catalogue that is not there is not a failure: the source language is
// English, and `tr()` returns what is written in the code.
bool load(QCoreApplication *application, const QString &prefix, const QString &directory,
          const QLocale &locale)
{
    auto *translator = new QTranslator(application);
    if (!translator->load(locale, prefix, QStringLiteral("_"), directory)) {
        delete translator;
        return false;
    }
    if (!QCoreApplication::installTranslator(translator)) {
        delete translator;
        return false;
    }
    installed.append(translator);
    return true;
}

} // namespace

namespace translations {

QString directory()
{
    const QByteArray override = qgetenv(kOverride);
    if (!override.isEmpty())
        return QString::fromLocal8Bit(override);
    return QString::fromLatin1(kDefaultDirectory);
}

void install(QCoreApplication *application, const QLocale &locale)
{
    for (QTranslator *translator : std::as_const(installed)) {
        QCoreApplication::removeTranslator(translator);
        delete translator;
    }
    installed.clear();
    // Qt's own words first, so that a window in another language does not ask
    // for confirmation with English buttons. Its catalogues live where Qt was
    // installed, not where ours are.
    load(application, QStringLiteral("qtbase"), QLibraryInfo::path(QLibraryInfo::TranslationsPath),
         locale);
    load(application, QStringLiteral("sombra"), directory(), locale);
}

} // namespace translations

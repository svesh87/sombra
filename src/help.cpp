// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "help.h"

#include <QDir>
#include <QFileInfo>
#include <QStringList>

namespace {

// Installed beside the catalogues rather than compiled into the binary: a page
// somebody wants to correct should be a file they can open.
constexpr const char *kDefaultDirectory = SOMBRA_HELP_PATH;
constexpr const char *kOverride = "SOMBRA_HELP_DIR";

QString fileFor(const QString &language)
{
    const QString candidate = help::directory() + QStringLiteral("/sombra-help.%1.html")
                                                          .arg(language);
    return QFileInfo::exists(candidate) ? candidate : QString();
}

} // namespace

namespace help {

QString directory()
{
    const QByteArray override = qgetenv(kOverride);
    if (!override.isEmpty())
        return QString::fromLocal8Bit(override);
    return QString::fromLatin1(kDefaultDirectory);
}

QString path(const QLocale &locale)
{
    // Both forms of the name, because a locale is `pt_BR` as often as it is `ru`,
    // and a page written for a language serves every region of it.
    for (const QString &name : locale.uiLanguages()) {
        const QString language = QString(name).replace(QLatin1Char('-'), QLatin1Char('_'));
        if (const QString file = fileFor(language); !file.isEmpty())
            return file;
        const QString bare = language.section(QLatin1Char('_'), 0, 0);
        if (const QString file = fileFor(bare); !file.isEmpty())
            return file;
    }
    return fileFor(QStringLiteral("en"));
}

QUrl url(const QString &tab, const QLocale &locale)
{
    const QString page = path(locale);
    if (page.isEmpty())
        return QUrl();
    QUrl target = QUrl::fromLocalFile(page);
    if (!tab.isEmpty())
        target.setFragment(tab);
    return target;
}

} // namespace help

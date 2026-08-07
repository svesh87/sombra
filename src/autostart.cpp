// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "autostart.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>

namespace autostart {

QString entryPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::ConfigLocation)
            + QStringLiteral("/autostart/ombra-tray.desktop");
}

bool enabled()
{
    return QFileInfo::exists(entryPath());
}

bool setEnabled(bool on, QString *error)
{
    const auto fail = [error](const QString &reason) {
        if (error)
            *error = reason;
        return false;
    };

    const QString path = entryPath();
    if (!on) {
        if (!QFileInfo::exists(path))
            return true;
        if (!QFile::remove(path))
            return fail(QStringLiteral("cannot remove %1").arg(path));
        return true;
    }

    const QFileInfo info(path);
    if (!QDir().mkpath(info.absolutePath()))
        return fail(QStringLiteral("cannot create %1").arg(info.absolutePath()));

    // Written whole or not at all: a half-written entry would be a desktop file
    // the session refuses, which reads as "the switch does nothing".
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return fail(QStringLiteral("cannot write %1: %2").arg(path, file.errorString()));

    const QString contents = QStringLiteral("[Desktop Entry]\n"
                                            "Type=Application\n"
                                            "Name=Ombra\n"
                                            "Comment=Thermal envelope console\n"
                                            "Exec=%1\n"
                                            "Icon=applications-system\n"
                                            "Terminal=false\n"
                                            "StartupNotify=false\n"
                                            "X-GNOME-Autostart-enabled=true\n")
                                    .arg(QCoreApplication::applicationFilePath());
    if (file.write(contents.toUtf8()) < 0 || !file.commit())
        return fail(QStringLiteral("cannot write %1: %2").arg(path, file.errorString()));
    return true;
}

} // namespace autostart

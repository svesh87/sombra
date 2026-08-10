// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "settings_file.h"

#include <QDir>
#include <QFileInfo>
#include <QSaveFile>

namespace settings_file {

QString serialize(const ModeTable &modes, const BrightnessSettings &brightness)
{
    QString text;
    text += QStringLiteral("# Written by ombrad. Edit through `ombra config` or the tray;\n"
                           "# anything added here by hand is lost at the next change.\n"
                           "#\n"
                           "# Units and ranges are documented in README.md. The values a reset\n"
                           "# returns to are compiled into the daemon, not kept here.\n\n");

    text += QStringLiteral("[general]\n");
    text += QStringLiteral("default_mode=%1\n\n").arg(modes.defaultModeId());

    for (const Mode &mode : modes.modes()) {
        text += ModeTable::toConfigSection(mode.id, mode.label, mode.setpoints);
        text += QLatin1Char('\n');
    }

    text += brightnessSettingsSection(brightness);

    return text;
}

bool save(const QString &path, const ModeTable &modes, const BrightnessSettings &brightness,
          QString *error)
{
    const QString directory = QFileInfo(path).absolutePath();
    if (!QDir().mkpath(directory)) {
        if (error)
            *error = QStringLiteral("%1 could not be created").arg(directory);
        return false;
    }

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error)
            *error = QStringLiteral("%1 could not be opened: %2").arg(path, file.errorString());
        return false;
    }
    const QByteArray payload = serialize(modes, brightness).toUtf8();
    if (file.write(payload) != payload.size()) {
        if (error)
            *error = QStringLiteral("%1 could not be written: %2").arg(path, file.errorString());
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        if (error)
            *error = QStringLiteral("%1 could not be committed: %2").arg(path, file.errorString());
        return false;
    }
    return true;
}

} // namespace settings_file

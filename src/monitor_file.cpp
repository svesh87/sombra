// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "monitor_file.h"

#include <QFile>
#include <QHash>

#include <algorithm>

namespace {

QHash<QString, QString> fields(const QString &absolute)
{
    QHash<QString, QString> out;
    QFile file(absolute);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return out;
    const QByteArray contents = file.readAll();
    for (const QByteArray &line : contents.split('\n')) {
        const int eq = line.indexOf('=');
        if (eq <= 0)
            continue;
        out.insert(QString::fromUtf8(line.left(eq)).trimmed(),
                   QString::fromUtf8(line.mid(eq + 1)).trimmed());
    }
    return out;
}

MonitorInfo describe(const QHash<QString, QString> &keys, const QString &fallbackConnector)
{
    MonitorInfo info;
    info.vendor = keys.value(QStringLiteral("vendor"));
    info.model = keys.value(QStringLiteral("model"));
    info.serial = keys.value(QStringLiteral("serial"));
    info.connector = keys.value(QStringLiteral("connector"), fallbackConnector);
    bool ok = false;
    const int maximum = keys.value(QStringLiteral("maximum")).toInt(&ok);
    info.maximum = ok && maximum > 0 ? maximum : 100;
    info.id = monitorId(info.vendor, info.model, info.serial, info.connector);
    return info;
}

} // namespace

MonitorFileBus::MonitorFileBus(Sysfs &sysfs) : m_sysfs(sysfs) { }

QList<MonitorInfo> MonitorFileBus::detect()
{
    QList<MonitorInfo> found;
    for (const QString &relative : m_sysfs.glob(directory(), QStringLiteral("*"))) {
        const QHash<QString, QString> keys = fields(m_sysfs.absolute(relative));
        if (keys.isEmpty())
            continue;
        found.append(describe(keys, relative.section(QLatin1Char('/'), -1)));
    }
    return found;
}

QString MonitorFileBus::fileFor(const QString &id)
{
    for (const QString &relative : m_sysfs.glob(directory(), QStringLiteral("*"))) {
        const QHash<QString, QString> keys = fields(m_sysfs.absolute(relative));
        if (keys.isEmpty())
            continue;
        if (describe(keys, relative.section(QLatin1Char('/'), -1)).id == id)
            return relative;
    }
    return QString();
}

std::optional<int> MonitorFileBus::read(const QString &id)
{
    const QString relative = fileFor(id);
    if (relative.isEmpty())
        return std::nullopt;
    const QHash<QString, QString> keys = fields(m_sysfs.absolute(relative));
    bool ok = false;
    const int level = keys.value(QStringLiteral("level")).toInt(&ok);
    if (!ok)
        return std::nullopt;
    return std::clamp(level, 0, 100);
}

bool MonitorFileBus::write(const QString &id, int percent)
{
    const QString relative = fileFor(id);
    if (relative.isEmpty())
        return false;

    QHash<QString, QString> keys = fields(m_sysfs.absolute(relative));
    keys.insert(QStringLiteral("level"), QString::number(std::clamp(percent, 0, 100)));

    QString text;
    // A fixed order, so that a file rewritten by a test is still readable by a
    // person looking at it afterwards.
    for (const QString &key : { QStringLiteral("vendor"), QStringLiteral("model"),
                                QStringLiteral("serial"), QStringLiteral("connector"),
                                QStringLiteral("maximum"), QStringLiteral("level") }) {
        if (keys.contains(key))
            text += key + QLatin1Char('=') + keys.value(key) + QLatin1Char('\n');
    }

    QFile file(m_sysfs.absolute(relative));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return file.write(text.toUtf8()) == text.toUtf8().size();
}

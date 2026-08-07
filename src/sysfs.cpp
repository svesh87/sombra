// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "sysfs.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace {

QString joinRoot(const QString &root, const QString &relative)
{
    QString trimmed = relative;
    while (trimmed.startsWith(QLatin1Char('/')))
        trimmed.remove(0, 1);
    if (root.endsWith(QLatin1Char('/')))
        return root + trimmed;
    return root + QLatin1Char('/') + trimmed;
}

// Searches directories under `classDir` for one whose `name` file matches.
std::optional<QString> findByNameFile(const Sysfs &sysfs, const QString &classDir,
                                      const QString &pattern, const QString &wanted)
{
    const QStringList candidates = sysfs.glob(classDir, pattern);
    for (const QString &candidate : candidates) {
        const std::optional<QString> name = sysfs.readText(candidate + QStringLiteral("/name"));
        if (name && *name == wanted)
            return candidate;
    }
    return std::nullopt;
}

} // namespace

Sysfs::Sysfs(QString root)
    : m_root(root.isEmpty() ? QStringLiteral("/") : std::move(root))
{
}

QString Sysfs::absolute(const QString &relative) const
{
    return joinRoot(m_root, relative);
}

bool Sysfs::exists(const QString &relative) const
{
    return QFileInfo::exists(absolute(relative));
}

std::optional<QString> Sysfs::readText(const QString &relative) const
{
    QFile file(absolute(relative));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return std::nullopt;
    const QByteArray raw = file.read(4096);
    return QString::fromUtf8(raw).trimmed();
}

std::optional<qint64> Sysfs::readNumber(const QString &relative) const
{
    const std::optional<QString> text = readText(relative);
    if (!text)
        return std::nullopt;
    bool ok = false;
    const qint64 value = text->toLongLong(&ok);
    if (!ok)
        return std::nullopt;
    return value;
}

bool Sysfs::writeText(const QString &relative, const QString &value)
{
    if (!exists(relative))
        return false;
    QFile file(absolute(relative));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    const QByteArray payload = value.toUtf8();
    if (file.write(payload) != payload.size())
        return false;
    return file.flush();
}

bool Sysfs::writeNumber(const QString &relative, qint64 value)
{
    return writeText(relative, QString::number(value));
}

std::optional<QString> Sysfs::hwmonByName(const QString &name) const
{
    return findByNameFile(*this, QStringLiteral("sys/class/hwmon"),
                          QStringLiteral("hwmon*"), name);
}

std::optional<QString> Sysfs::raplDomainByName(const QString &name) const
{
    return findByNameFile(*this, QStringLiteral("sys/class/powercap"),
                          QStringLiteral("intel-rapl*"), name);
}

QStringList Sysfs::glob(const QString &relativeDir, const QString &pattern) const
{
    QDir dir(absolute(relativeDir));
    if (!dir.exists())
        return {};
    const QStringList names = dir.entryList(QStringList{pattern},
                                            QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot,
                                            QDir::Name);
    QStringList result;
    result.reserve(names.size());
    for (const QString &name : names)
        result.append(relativeDir + QLatin1Char('/') + name);
    return result;
}

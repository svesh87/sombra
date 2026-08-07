// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#pragma once

#include <QString>
#include <QStringList>

#include <optional>

// Every read and write of the kernel control surface goes through this class.
// The tree root is injected: production passes "/", tests pass a directory in
// a temporary location, so nothing in the test suite can reach real hardware.
//
// Paths passed to these methods are relative to that root and therefore carry
// no leading slash: "sys/class/hwmon", not "/sys/class/hwmon".
//
// Units in sysfs are easy to confuse. Nodes under powercap deal in microwatts
// and microseconds, hwmon temperatures in millidegrees, cpufreq in kilohertz.
// This class does no conversion at all; it moves integers and strings. The
// callers that know the semantics do the conversion.
class Sysfs {
public:
    explicit Sysfs(QString root);

    const QString &root() const { return m_root; }

    // Absolute path of a node, for log messages and for direct system calls.
    QString absolute(const QString &relative) const;

    bool exists(const QString &relative) const;

    // Trailing whitespace is stripped: sysfs nodes end in a newline.
    std::optional<QString> readText(const QString &relative) const;
    std::optional<qint64> readNumber(const QString &relative) const;

    // A node that does not exist is never written. Missing nodes differ
    // between kernels, and a write to one is a bug, not an emergency.
    bool writeText(const QString &relative, const QString &value);
    bool writeNumber(const QString &relative, qint64 value);

    // Resolves a hwmon directory by the contents of its `name` file and
    // returns the directory path relative to the root. hwmon indices move
    // between boots, so nothing may address them by number.
    std::optional<QString> hwmonByName(const QString &name) const;

    // Entries of `relativeDir` matching a glob, as paths relative to the root,
    // sorted so that iteration order does not depend on the filesystem.
    QStringList glob(const QString &relativeDir, const QString &pattern) const;

    // Directory under /sys/class/powercap whose `name` file holds `name`
    // (for example "package-0", "core", "uncore"), relative to the root.
    std::optional<QString> raplDomainByName(const QString &name) const;

private:
    QString m_root;
};

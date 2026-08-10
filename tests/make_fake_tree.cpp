// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "fake_sysfs.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QTextStream>

// Builds the fake control surface at a given path and leaves it there.
//
// It exists so the documentation pictures can be taken against a daemon that
// owns nothing: the readings in them are then the same every time, which keeps
// a regeneration from being a diff of temperatures, and no reading from
// anybody's machine ends up in a public repository.
//
// The tree itself comes from the same header the test suite uses, so the two
// cannot drift apart. Not installed — this is test and documentation support.
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTextStream err(stderr);
    const QStringList arguments = QCoreApplication::arguments();
    if (arguments.size() != 2) {
        err << "usage: " << arguments.value(0) << " <directory>\n";
        return 2;
    }

    const QString target = arguments.at(1);
    // FakeSysfs builds its tree in a temporary directory of its own, so the
    // result is copied out. Copying rather than pointing at it: the temporary
    // one is removed when this process exits, and the daemon outlives it.
    FakeSysfs tree;
    QDir().mkpath(target);

    QDirIterator it(tree.root(), QDir::Files | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString source = it.next();
        const QString relative = QDir(tree.root()).relativeFilePath(source);
        const QString destination = target + QLatin1Char('/') + relative;
        QDir().mkpath(QFileInfo(destination).absolutePath());
        QFile::remove(destination);
        if (!QFile::copy(source, destination)) {
            err << "cannot write " << destination << '\n';
            return 1;
        }
        QFile::setPermissions(destination,
                              QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                      | QFileDevice::ReadGroup | QFileDevice::ReadOther);
    }

    QTextStream(stdout) << target << '\n';
    return 0;
}

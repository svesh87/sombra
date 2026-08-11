// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "fake_bus.h"

#include <QCoreApplication>
#include <QDebug>
#include <QTextStream>

// Something to answer for polkit on a bus that has no polkit.
//
// Not a test and not installed. It exists because the documentation pictures are
// taken against a throwaway daemon on a bus of its own, and every command that
// daemon accepts is checked against an action first: without an authority the
// console in the pictures would be a console whose every command was refused.
//
// It says yes to everything, which is exactly what it may do here — the bus it
// answers on is private to whoever started it, and the daemon behind it owns a
// directory of ordinary files rather than a machine.
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTextStream err(stderr);

    if (argc < 2) {
        err << "usage: sombra-fake-authority <bus address>\n";
        return 2;
    }

    FakeAuthority authority;
    QString error;
    if (!authority.start(QString::fromLocal8Bit(argv[1]), &error)) {
        err << error << '\n';
        return 1;
    }
    // Printed so a script can wait for this line rather than for a process, the same
    // way it waits for the daemon's name rather than for the daemon.
    QTextStream(stdout) << "ready\n" << Qt::flush;
    return app.exec();
}

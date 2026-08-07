// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QLocalSocket>
#include <QTextStream>

// The scriptable face of the control socket. It is the only way to drive the
// daemon before the tray exists, and it stays afterwards because a text
// command is what a measurement script can issue.
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("cpu-throttlerctl"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
            "Commands:\n"
            "  status                     current readings and setpoints as JSON\n"
            "  mode <id>                  select a mode; cancels live tuning\n"
            "  set <key>=<value> ...      live tuning: pl1, pl2, max_perf_pct, turbo,\n"
            "                             epp, hwp_boost, gt_max_mhz, profile\n"
            "  reset                      drop live tuning, return to the mode\n"
            "  mark good|too-loud|too-slow  stamp a verdict onto the current sample\n"
            "  fan max|auto               full speed is explicit only\n"
            "  charge <percent>           battery charge threshold\n"
            "  snippet [id]               live setpoints as a base64 config snippet\n"
            "  reload                     re-read the configuration file"));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption socketOption(
            QStringLiteral("socket"), QStringLiteral("Control socket."), QStringLiteral("path"),
            QStringLiteral("/run/cpu_throttler/control.sock"));
    parser.addOption(socketOption);
    parser.addPositionalArgument(QStringLiteral("command"),
                                 QStringLiteral("Command and its arguments."));
    parser.process(app);

    QTextStream out(stdout);
    QTextStream err(stderr);

    const QStringList words = parser.positionalArguments();
    if (words.isEmpty()) {
        err << "no command given; try --help\n";
        return 2;
    }

    QLocalSocket socket;
    socket.connectToServer(parser.value(socketOption));
    if (!socket.waitForConnected(2000)) {
        err << "cannot reach the daemon on " << parser.value(socketOption) << ": "
            << socket.errorString() << '\n';
        return 1;
    }

    socket.write(words.join(QLatin1Char(' ')).toUtf8());
    socket.write("\n");
    socket.flush();

    if (!socket.waitForReadyRead(2000)) {
        err << "no reply from the daemon\n";
        return 1;
    }
    const QString reply = QString::fromUtf8(socket.readLine()).trimmed();
    const bool accepted = reply.startsWith(QLatin1String("ok"));

    // The protocol is one line per exchange, so a multi-line configuration
    // snippet travels base64-encoded. Unpack it here rather than making every
    // caller do it.
    if (accepted && words.first() == QLatin1String("snippet") && reply.size() > 3) {
        out << QString::fromUtf8(QByteArray::fromBase64(reply.mid(3).toUtf8()));
        return 0;
    }

    out << reply << '\n';
    return accepted ? 0 : 1;
}

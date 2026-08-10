// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "exporter.h"
#include "metrics_ring.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QLocalSocket>
#include <QTextStream>

// The scriptable face of the daemon. It is the only way to drive it before the
// tray exists, and it stays afterwards because a text command is what a
// measurement script can issue.
//
// Every command travels over the control socket except one. `export` reads the
// telemetry ring straight off disk: the daemon keeps no history, the ring is a
// file, and a week of samples has no business going through a line protocol.
// That is why it is dispatched before the control parser runs — it carries
// options of its own that the control side knows nothing about.

namespace {

int runExport(const QStringList &arguments)
{
    QCommandLineParser parser;
    parser.setApplicationDescription(
            QStringLiteral("Reads the telemetry ring. Nothing is averaged: the peaks are what the "
                           "mode parameters are tuned against."));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption ringOption(QStringLiteral("ring"), QStringLiteral("Telemetry ring."),
                                        QStringLiteral("file"),
                                        QStringLiteral("/var/lib/sombra/metrics.ring"));
    const QCommandLineOption formatOption(QStringLiteral("format"), QStringLiteral("csv or json."),
                                          QStringLiteral("name"), QStringLiteral("csv"));
    const QCommandLineOption fromOption(QStringLiteral("from"),
                                        QStringLiteral("Earliest unix timestamp to include."),
                                        QStringLiteral("ts"), QStringLiteral("0"));
    const QCommandLineOption toOption(
            QStringLiteral("to"),
            QStringLiteral("Latest unix timestamp to include; 0 means no upper bound."),
            QStringLiteral("ts"), QStringLiteral("0"));
    parser.addOptions({ ringOption, formatOption, fromOption, toOption });
    parser.process(arguments);

    QTextStream out(stdout);
    QTextStream err(stderr);

    const QString formatName = parser.value(formatOption).toLower();
    if (formatName != QLatin1String("csv") && formatName != QLatin1String("json")) {
        err << "unknown format " << formatName << "; expected csv or json\n";
        return 2;
    }

    MetricsRing ring;
    QString error;
    if (!ring.openExisting(parser.value(ringOption), &error)) {
        err << error << '\n';
        return 1;
    }

    exporter::write(ring, out,
                    formatName == QLatin1String("csv") ? exporter::Format::Csv
                                                       : exporter::Format::Json,
                    parser.value(fromOption).toUInt(), parser.value(toOption).toUInt());
    return 0;
}

int runControl(const QStringList &arguments)
{
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
            "Commands:\n"
            "  status                     current readings and setpoints as JSON\n"
            "  mode <id>                  select a mode; cancels live tuning\n"
            "  set <key>=<value> ...      live tuning: pl1, pl2, max_perf_pct, turbo,\n"
            "                             epp, hwp_boost, gt_max_mhz, profile, fan\n"
            "  reset                      drop live tuning, return to the mode\n"
            "  mark good|too-loud|too-slow  stamp a verdict onto the current sample\n"
            "  fan max|auto               shorthand for set fan=<value>\n"
            "  brightness on|off          follow the ambient light sensor, or do not\n"
            "  brightness level <percent>  set the backlight, as the person would\n"
            "  brightness base <percent>   the level the automation works around\n"
            "  brightness set <key>=<value> ...  response_pct, floor_pct, curve\n"
            "  charge <percent>           battery charge threshold\n"
            "  config show                what the daemon has saved, as JSON\n"
            "  config set mode <id> <key>=<value> ...   durable mode definition:\n"
            "                             label, platform_profile, pl1_w, pl2_w,\n"
            "                             max_perf_pct, turbo, epp, hwp_dynamic_boost,\n"
            "                             gt_max_mhz, fan\n"
            "  config set brightness <key>=<value> ...  write the backlight settings:\n"
            "                             response_pct, floor_pct, curve\n"
            "  config save live to <id>   keep the current live tuning as a mode\n"
            "  config default_mode <id>   the mode applied at boot\n"
            "  config reset modes|brightness|all        back to the built-in values\n"
            "  reload                     re-read the settings file\n"
            "  export [--format csv|json] [--from ts] [--to ts] [--ring file]\n"
            "                             read the telemetry ring; see `export --help`"));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption socketOption(
            QStringLiteral("socket"), QStringLiteral("Control socket."), QStringLiteral("path"),
            QStringLiteral("/run/sombra/control.sock"));
    parser.addOption(socketOption);
    parser.addPositionalArgument(QStringLiteral("command"),
                                 QStringLiteral("Command and its arguments."));
    parser.process(arguments);

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

    // The "ok" is protocol framing, and the two reading commands answer with a
    // payload behind it. Printing the payload alone is what makes
    // `sombra status | jq` and `sombra config show | jq` work without a caller
    // stripping a word off the front of the answer.
    if (accepted && reply.size() > 3) {
        if (words.first() == QLatin1String("status")
            || (words.first() == QLatin1String("config") && words.size() > 1
                && words.at(1) == QLatin1String("show"))) {
            out << reply.mid(3) << '\n';
            return 0;
        }
    }

    out << reply << '\n';
    return accepted ? 0 : 1;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("sombra"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));

    QStringList arguments = QCoreApplication::arguments();
    if (arguments.size() > 1 && arguments.at(1) == QLatin1String("export")) {
        // Dropped so that `export --help` reaches the export parser rather
        // than being read as a positional command.
        arguments.removeAt(1);
        return runExport(arguments);
    }
    return runControl(arguments);
}

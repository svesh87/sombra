// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "bus_client.h"
#include "control_wire.h"
#include "exporter.h"
#include "metrics_ring.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>

// The scriptable face of the daemon. It is the only way to drive it before the
// console exists, and it stays afterwards because a text command is what a
// measurement script can issue.
//
// Every command travels over the bus except one. `export` reads the telemetry ring
// straight off disk: the daemon keeps no history, the ring is a file, and a week of
// samples has no business going through a message bus. That is why it is dispatched
// before the control parser runs — it carries options of its own that the control
// side knows nothing about.
//
// The words are the interface. What they mean on the bus is decided in one place,
// `wire::callFor`, which the console uses as well: a person reading
// "set pl1=30 → ok" on the console's status line has to be able to type the same
// thing here.

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
            "  brightness set <key>=<value> ...  response_pct, floor_pct, ceiling_pct, curve\n"
            "  monitor list               the external monitors that answer DDC/CI\n"
            "  monitor level <percent> [<id>]   set one, or every one that answers\n"
            "  charge <percent>           battery charge threshold\n"
            "  config show                what the daemon has saved, as JSON\n"
            "  config set mode <id> <key>=<value> ...   durable mode definition:\n"
            "                             label, platform_profile, pl1_w, pl2_w,\n"
            "                             max_perf_pct, turbo, epp, hwp_dynamic_boost,\n"
            "                             gt_max_mhz, fan\n"
            "  config set brightness <key>=<value> ...  write the backlight settings:\n"
            "                             response_pct, floor_pct, ceiling_pct, curve\n"
            "  config set metrics <key>=<value> ...     how the telemetry is kept:\n"
            "                             retention_days (1..30), enabled\n"
            "  config save live to <id>   keep the current live tuning as a mode\n"
            "  config default_mode <id>   the mode applied at boot\n"
            "  config reset modes|brightness|metrics|all  back to the built-in values\n"
            "  reload                     re-read the settings file\n"
            "  export [--format csv|json] [--from ts] [--to ts] [--ring file]\n"
            "                             read the telemetry ring; see `export --help`"));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption busOption(
            QStringLiteral("bus-address"),
            QStringLiteral("Bus the daemon answers on. Empty means the system bus."),
            QStringLiteral("address"));
    parser.addOption(busOption);
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

    // What the words mean, worked out before anything is opened: a typo is not worth
    // a connection.
    const wire::Call call = wire::callFor(words);
    if (!call.valid()) {
        err << call.error << '\n';
        return 2;
    }

    BusClient client(parser.value(busOption));
    const QVariantMap properties = client.readAll();
    if (properties.isEmpty()) {
        err << "the daemon is not answering on " << wire::kService << '\n';
        return 1;
    }

    // The reading commands are properties, so they are already in hand. Printed as
    // the payload alone, without a word of framing in front of it, because the answer
    // has to go straight into a JSON parser.
    const auto printJson = [&out](const QJsonDocument &document) {
        out << QString::fromUtf8(document.toJson(QJsonDocument::Compact)) << '\n';
        return 0;
    };
    switch (call.read) {
    case wire::Call::Read::Status:
        return printJson(QJsonDocument(wire::statusJsonFrom(properties)));
    case wire::Call::Read::Configuration:
        return printJson(QJsonDocument(QJsonObject::fromVariantMap(
                properties.value(QStringLiteral("Configuration")).toMap())));
    case wire::Call::Read::Monitors:
        return printJson(QJsonDocument(
                wire::statusJsonFrom(properties).value(QStringLiteral("monitors")).toArray()));
    case wire::Call::Read::None:
        break;
    }

    const QString reply = client.call(call.method, call.arguments);
    const bool accepted = reply.startsWith(QLatin1String("ok"));
    out << reply << '\n';
    return accepted ? 0 : 1;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("sombra"));
    QCoreApplication::setApplicationVersion(QString::fromLatin1(SOMBRA_VERSION));

    QStringList arguments = QCoreApplication::arguments();
    if (arguments.size() > 1 && arguments.at(1) == QLatin1String("export")) {
        // Dropped so that `export --help` reaches the export parser rather
        // than being read as a positional command.
        arguments.removeAt(1);
        return runExport(arguments);
    }
    return runControl(arguments);
}

// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "exporter.h"
#include "metrics_ring.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QTextStream>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("cpu-throttler-export"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
            QStringLiteral("Reads the telemetry ring. Nothing is averaged: the peaks are what the "
                           "mode parameters are tuned against."));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption ringOption(QStringLiteral("ring"), QStringLiteral("Telemetry ring."),
                                        QStringLiteral("file"),
                                        QStringLiteral("/var/lib/cpu_throttler/metrics.ring"));
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
    parser.process(app);

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

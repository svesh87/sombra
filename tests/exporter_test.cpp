// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "exporter.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

namespace {

Sample populated(uint32_t ts)
{
    Sample s {};
    s.ts = ts;
    s.pkgTemp = 803;
    s.boardTemp = 451;
    s.nvmeTemp = 388;
    s.fanRpm = 3700;
    s.pkgPower = 2050;
    s.corePower = 1200;
    s.uncorePower = 700;
    s.cpuMhz = 2100;
    s.gpuMhz = 900;
    s.load1 = 812;
    s.pkgThrottle = 2;
    s.coreThrottle = 5;
    s.modeId = 1;
    s.pl1W = 20;
    s.pl2W = 30;
    s.maxPerfPct = 50;
    s.noTurbo = 1;
    s.epp = static_cast<uint8_t>(Epp::Power);
    s.hwpBoost = 0;
    s.profile = static_cast<uint8_t>(Profile::Balanced);
    s.gtMaxMhz = 2350;
    s.fanMode = 2;
    s.chargeLimit = 80;
    s.flags = FlagOnAc | FlagLiveTuning;
    s.mark = static_cast<uint8_t>(Mark::TooLoud);
    s.light = 1500;
    s.brightness = 60;
    s.brightnessBase = 55;
    s.brightnessResponse = 30;
    s.brightnessFloor = 35;
    return s;
}

Sample blind(uint32_t ts)
{
    Sample s {};
    s.ts = ts;
    s.pkgTemp = kAbsentTemp;
    s.boardTemp = kAbsentTemp;
    s.nvmeTemp = kAbsentTemp;
    s.fanRpm = kAbsentValue;
    s.pkgPower = kAbsentValue;
    s.corePower = kAbsentValue;
    s.uncorePower = kAbsentValue;
    s.cpuMhz = kAbsentValue;
    s.gpuMhz = kAbsentValue;
    s.load1 = kAbsentValue;
    s.modeId = 0xFF;
    s.flags = FlagSensorsDegraded | FlagGuardEngaged | FlagWriteMismatch;
    s.light = kAbsentLight;
    s.brightness = kAbsentPercent;
    s.brightnessBase = kAbsentPercent;
    s.brightnessResponse = kAbsentPercent;
    s.brightnessFloor = kAbsentPercent;
    return s;
}

} // namespace

class ExporterTest : public QObject {
    Q_OBJECT

private slots:
    void csvHeaderMatchesTheRowWidth();
    void csvRendersEveryValueAndEverySetpoint();
    void absentReadingsAreEmptyNotZero();
    void jsonRowIsValidAndNullsAbsentReadings();
    void writeFiltersByTimestampAndSkipsEmptySlots();
    void jsonOutputParsesAsAnArray();

private:
    QTemporaryDir m_dir;
};

void ExporterTest::csvHeaderMatchesTheRowWidth()
{
    const int columns = exporter::csvHeader().count(QLatin1Char(',')) + 1;
    const int fields = exporter::csvRow(populated(1)).count(QLatin1Char(',')) + 1;
    QCOMPARE(fields, columns);
    QCOMPARE(exporter::csvRow(blind(1)).count(QLatin1Char(',')) + 1, columns);
}

void ExporterTest::csvRendersEveryValueAndEverySetpoint()
{
    const QStringList fields = exporter::csvRow(populated(1700000000)).split(QLatin1Char(','));
    const QStringList names = exporter::csvHeader().split(QLatin1Char(','));
    const auto field = [&](const QString &name) {
        return fields.at(names.indexOf(name));
    };

    QCOMPARE(field(QStringLiteral("ts")), QStringLiteral("1700000000"));
    QCOMPARE(field(QStringLiteral("pkg_c")), QStringLiteral("80.3"));
    QCOMPARE(field(QStringLiteral("fan_rpm")), QStringLiteral("3700"));
    QCOMPARE(field(QStringLiteral("pkg_w")), QStringLiteral("20.50"));
    QCOMPARE(field(QStringLiteral("core_w")), QStringLiteral("12.00"));
    QCOMPARE(field(QStringLiteral("uncore_w")), QStringLiteral("7.00"));
    QCOMPARE(field(QStringLiteral("load1")), QStringLiteral("8.12"));
    // The setpoints travel with the readings; transitions are derived by
    // diffing consecutive rows rather than from a separate event log.
    QCOMPARE(field(QStringLiteral("pl1_w")), QStringLiteral("20"));
    QCOMPARE(field(QStringLiteral("turbo")), QStringLiteral("0"));
    QCOMPARE(field(QStringLiteral("epp")), QStringLiteral("power"));
    QCOMPARE(field(QStringLiteral("profile")), QStringLiteral("balanced"));
    QCOMPARE(field(QStringLiteral("on_ac")), QStringLiteral("1"));
    QCOMPARE(field(QStringLiteral("live")), QStringLiteral("1"));
    QCOMPARE(field(QStringLiteral("guard")), QStringLiteral("0"));
    QCOMPARE(field(QStringLiteral("mark")), QStringLiteral("too-loud"));
    // The window the backlight automation was allowed to work in. Recorded per
    // sample because live tuning can override the profile, and a row that
    // needs the configuration file read alongside it is not self-contained.
    QCOMPARE(field(QStringLiteral("brightness_response")), QStringLiteral("30"));
    QCOMPARE(field(QStringLiteral("brightness_floor")), QStringLiteral("35"));
    // Lux with the dark end kept: the record holds millilux, and 1500 of them
    // is one and a half lux — a room with the lights off.
    QCOMPARE(field(QStringLiteral("light_lux")), QStringLiteral("1.500"));
}

void ExporterTest::absentReadingsAreEmptyNotZero()
{
    const QStringList fields = exporter::csvRow(blind(5)).split(QLatin1Char(','));
    const QStringList names = exporter::csvHeader().split(QLatin1Char(','));
    for (const QString &name : { QStringLiteral("pkg_c"), QStringLiteral("fan_rpm"),
                                 QStringLiteral("pkg_w"), QStringLiteral("load1"),
                                 QStringLiteral("mode_id"), QStringLiteral("brightness_response"),
                                 QStringLiteral("brightness_floor"),
                                 QStringLiteral("light_lux") }) {
        QVERIFY2(fields.at(names.indexOf(name)).isEmpty(), qPrintable(name));
    }
    QCOMPARE(fields.at(names.indexOf(QStringLiteral("degraded"))), QStringLiteral("1"));
    QCOMPARE(fields.at(names.indexOf(QStringLiteral("mismatch"))), QStringLiteral("1"));
}

void ExporterTest::jsonRowIsValidAndNullsAbsentReadings()
{
    QJsonParseError error {};
    const QJsonDocument full =
            QJsonDocument::fromJson(exporter::jsonRow(populated(42)).toUtf8(), &error);
    QCOMPARE(error.error, QJsonParseError::NoError);
    QCOMPARE(full.object().value(QStringLiteral("pkg_c")).toDouble(), 80.3);
    QCOMPARE(full.object().value(QStringLiteral("epp")).toString(), QStringLiteral("power"));
    QCOMPARE(full.object().value(QStringLiteral("turbo")).toBool(), false);

    const QJsonDocument sparse = QJsonDocument::fromJson(exporter::jsonRow(blind(43)).toUtf8());
    QVERIFY(sparse.object().value(QStringLiteral("pkg_c")).isNull());
    QVERIFY(sparse.object().value(QStringLiteral("mode_id")).isNull());
    QCOMPARE(sparse.object().value(QStringLiteral("degraded")).toBool(), true);
}

void ExporterTest::writeFiltersByTimestampAndSkipsEmptySlots()
{
    MetricsRing ring;
    QVERIFY(ring.open(m_dir.path() + QStringLiteral("/filter.ring"), 32));
    for (uint32_t ts = 100; ts < 110; ++ts)
        ring.append(populated(ts));

    QString text;
    QTextStream stream(&text);
    QCOMPARE(exporter::write(ring, stream, exporter::Format::Csv, 103, 105), quint64(3));
    stream.flush();
    const QStringList lines = text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    QCOMPARE(lines.size(), 4);  // header plus three rows
    QVERIFY(lines.at(1).startsWith(QStringLiteral("103,")));
    QVERIFY(lines.at(3).startsWith(QStringLiteral("105,")));

    QString unbounded;
    QTextStream all(&unbounded);
    QCOMPARE(exporter::write(ring, all, exporter::Format::Csv, 0, 0), quint64(10));
}

void ExporterTest::jsonOutputParsesAsAnArray()
{
    MetricsRing ring;
    QVERIFY(ring.open(m_dir.path() + QStringLiteral("/json.ring"), 8));
    ring.append(populated(200));
    ring.append(blind(201));

    QString text;
    QTextStream stream(&text);
    QCOMPARE(exporter::write(ring, stream, exporter::Format::Json, 0, 0), quint64(2));
    stream.flush();

    QJsonParseError error {};
    const QJsonDocument document = QJsonDocument::fromJson(text.toUtf8(), &error);
    QCOMPARE(error.error, QJsonParseError::NoError);
    QVERIFY(document.isArray());
    QCOMPARE(document.array().size(), 2);

    // An empty selection is still valid JSON.
    QString empty;
    QTextStream none(&empty);
    QCOMPARE(exporter::write(ring, none, exporter::Format::Json, 9000, 0), quint64(0));
    none.flush();
    QCOMPARE(QJsonDocument::fromJson(empty.toUtf8()).array().size(), 0);
}

QTEST_MAIN(ExporterTest)
#include "exporter_test.moc"

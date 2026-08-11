// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "control_wire.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

// Both halves of the wire have an inverse, and that is what makes them checkable
// without a bus: the state goes out as properties and comes back as the same state,
// and a command becomes a call and the call becomes the same command. A mapping
// written twice can only be trusted if the two directions are compared with each
// other rather than each with its author's intention.

namespace {

// A status object with something awkward in every corner: absent readings, a monitor
// that stopped answering, no charge threshold, and a baseline that is not set — which
// is what takes the brightness band away.
QJsonObject fullStatus()
{
    return QJsonDocument::fromJson(R"({
        "ts": 1770000000,
        "ticks": 42,
        "mode": "work",
        "modes": [{"id": "quiet", "label": "Quiet"}, {"id": "work", "label": "Work"}],
        "live_tuning": true,
        "guard_engaged": false,
        "fan_forced": true,
        "write_mismatch": false,
        "sensors_degraded": true,
        "on_ac": true,
        "charge_limit": 80,
        "applied": {"profile": "quiet", "pl1_w": 20, "pl2_w": 30, "max_perf_pct": 50,
                    "turbo": false, "epp": "power", "hwp_boost": false, "gt_max_mhz": 900,
                    "fan_mode": 2},
        "requested": {"profile": "balanced", "pl1_w": 25, "pl2_w": 35, "max_perf_pct": 60,
                      "turbo": true, "epp": "balance_power", "hwp_boost": true,
                      "gt_max_mhz": 1200, "fan_mode": 0},
        "sensors": {"pkg_c": 61.5, "board_c": null, "nvme_c": 40.0, "fan_rpm": 1900,
                    "pkg_w": 6.25, "core_w": null, "uncore_w": 1.5, "cpu_mhz": 1800,
                    "gpu_mhz": null, "load1": 0.42, "displays": 3},
        "brightness": {"available": true, "auto": true, "level": 55, "base": null,
                       "light_lux": 120.5, "response_pct": 30, "floor_pct": 20,
                       "ceiling_pct": 90},
        "monitors": [
            {"id": "AQ14T-1", "model": "AQ14T", "connector": "DP-1", "level": 42,
             "answering": true},
            {"id": "DELL-2", "model": "DELL", "connector": "DP-2", "level": null,
             "answering": false}
        ],
        "metrics": {"retention_days": 7, "enabled": true, "samples": 12345}
    })").object();
}

QJsonObject fullConfiguration()
{
    return QJsonDocument::fromJson(R"({
        "path": "/var/lib/sombra/settings.conf",
        "stored": true,
        "default_mode": "work",
        "modes": [{"id": "quiet", "label": "Quiet", "platform_profile": "quiet",
                   "pl1_w": 12, "pl2_w": 20, "max_perf_pct": 40, "turbo": false,
                   "epp": "power", "hwp_dynamic_boost": false, "gt_max_mhz": 800}],
        "brightness": {"response_pct": 30, "floor_pct": 20, "ceiling_pct": 90,
                       "curve": "10:20,100:50"},
        "metrics": {"retention_days": 7, "enabled": true}
    })").object();
}

} // namespace

class ControlWireTest : public QObject {
    Q_OBJECT

private slots:
    void theStateSurvivesTheSplitIntoProperties();
    void anAbsentReadingIsNotAZero();
    void aBaselineThatIsNotSetTakesTheBandWithIt();
    void everyCommandBecomesACallAndTheCallBecomesTheCommand();
    void aReadingCommandIsNotACall();
    void wordsThatMeanNothingSayWhy();
    void aValueWithASpaceIsRefusedRatherThanSplit();
};

void ControlWireTest::theStateSurvivesTheSplitIntoProperties()
{
    const QJsonObject status = fullStatus();
    const QVariantMap properties = wire::propertiesFrom(status, fullConfiguration());
    // Every key, every null, every nested object: the console and the command line
    // both build their picture from this, and a field lost here is a field nobody
    // notices until somebody looks for it.
    QCOMPARE(wire::statusJsonFrom(properties), status);

    // And what the daemon has saved travels whole, because it is one property.
    QCOMPARE(QJsonObject::fromVariantMap(properties.value(QStringLiteral("Configuration")).toMap()),
             fullConfiguration());
}

void ControlWireTest::anAbsentReadingIsNotAZero()
{
    const QVariantMap properties = wire::propertiesFrom(fullStatus(), fullConfiguration());
    const QVariantMap readings = properties.value(QStringLiteral("Readings")).toMap();
    // A reading the machine could not take is a key that is not there. Nought is a
    // legal fan speed and a legal power draw, so it cannot stand for "no answer".
    QVERIFY(readings.contains(QStringLiteral("pkg_c")));
    QVERIFY(!readings.contains(QStringLiteral("board_c")));
    QVERIFY(!readings.contains(QStringLiteral("core_w")));
    QVERIFY(!readings.contains(QStringLiteral("gpu_mhz")));
    // And it comes back as null rather than as nothing.
    const QJsonObject sensors =
            wire::statusJsonFrom(properties).value(QStringLiteral("sensors")).toObject();
    QVERIFY(sensors.value(QStringLiteral("board_c")).isNull());
    QCOMPARE(sensors.value(QStringLiteral("pkg_c")).toDouble(), 61.5);

    // The monitor that stopped answering keeps its place and loses its level: a
    // screen that is switched off is not a screen at nought per cent.
    const QJsonArray monitors =
            wire::statusJsonFrom(properties).value(QStringLiteral("monitors")).toArray();
    QCOMPARE(monitors.size(), 2);
    QCOMPARE(monitors.at(0).toObject().value(QStringLiteral("level")).toInt(), 42);
    QVERIFY(monitors.at(1).toObject().value(QStringLiteral("level")).isNull());
}

void ControlWireTest::aBaselineThatIsNotSetTakesTheBandWithIt()
{
    // No baseline, so there is no band to speak of, and the fields for it are absent
    // rather than nought — which the round trip has to reproduce exactly.
    QJsonObject status = fullStatus();
    QVERIFY(!status.value(QStringLiteral("brightness")).toObject().contains(
            QStringLiteral("window_low")));
    QCOMPARE(wire::statusJsonFrom(wire::propertiesFrom(status, {})), status);

    // With one, the band is there and travels.
    QJsonObject brightness = status.value(QStringLiteral("brightness")).toObject();
    brightness.insert(QStringLiteral("base"), 60);
    brightness.insert(QStringLiteral("window_low"), 42);
    brightness.insert(QStringLiteral("window_high"), 78);
    status.insert(QStringLiteral("brightness"), brightness);
    QCOMPARE(wire::statusJsonFrom(wire::propertiesFrom(status, {})), status);
}

void ControlWireTest::everyCommandBecomesACallAndTheCallBecomesTheCommand()
{
    // The whole vocabulary, and both directions of it. The daemon's own parser is
    // what validates a value, so a call that cannot be spelled as one of these lines
    // would be a way into the daemon that nothing checks.
    const QStringList commands {
        QStringLiteral("mode quiet"),
        QStringLiteral("set pl1=30"),
        QStringLiteral("set gt_max_mhz=900 pl1=20 pl2=30"),
        QStringLiteral("reset"),
        QStringLiteral("mark good"),
        QStringLiteral("charge 80"),
        QStringLiteral("reload"),
        QStringLiteral("brightness on"),
        QStringLiteral("brightness off"),
        QStringLiteral("brightness level 55"),
        QStringLiteral("brightness base 60"),
        QStringLiteral("brightness set ceiling_pct=90 floor_pct=20"),
        QStringLiteral("monitor level 42"),
        QStringLiteral("monitor level 42 AQ14T-1"),
        QStringLiteral("config set mode quiet pl1_w=12"),
        QStringLiteral("config set brightness curve=10:20,100:50"),
        QStringLiteral("config set metrics retention_days=9"),
        QStringLiteral("config save live to work"),
        QStringLiteral("config default_mode work"),
        QStringLiteral("config reset all"),
    };

    for (const QString &command : commands) {
        const wire::Call call = wire::callFor(command.split(QLatin1Char(' ')));
        QVERIFY2(call.valid(), qPrintable(command + QStringLiteral(": ") + call.error));
        QCOMPARE(call.read, wire::Call::Read::None);
        QString error;
        const QString back = wire::commandFor(call.method, call.arguments, &error);
        QVERIFY2(!back.isEmpty(), qPrintable(command + QStringLiteral(": ") + error));
        QCOMPARE(back, command);
    }

    // One word is not its own command in the daemon's vocabulary: the fan is a
    // setpoint like the power limits, and "full speed, right now" is short for
    // changing it. So this one comes back as what it always meant.
    const wire::Call fan = wire::callFor({ QStringLiteral("fan"), QStringLiteral("max") });
    QCOMPARE(fan.method, QStringLiteral("SetLive"));
    QCOMPARE(wire::commandFor(fan.method, fan.arguments, nullptr),
             QStringLiteral("set fan=max"));
}

void ControlWireTest::aReadingCommandIsNotACall()
{
    // Nothing on the bus needs asking for what it is already announcing.
    QCOMPARE(wire::callFor({ QStringLiteral("status") }).read, wire::Call::Read::Status);
    QCOMPARE(wire::callFor({ QStringLiteral("config"), QStringLiteral("show") }).read,
             wire::Call::Read::Configuration);
    QCOMPARE(wire::callFor({ QStringLiteral("monitor"), QStringLiteral("list") }).read,
             wire::Call::Read::Monitors);
    QVERIFY(wire::callFor({ QStringLiteral("status") }).method.isEmpty());
}

void ControlWireTest::wordsThatMeanNothingSayWhy()
{
    const QStringList refusals {
        QString(),
        QStringLiteral("teapot"),
        QStringLiteral("mode"),
        QStringLiteral("set"),
        QStringLiteral("set pl1"),
        QStringLiteral("brightness"),
        QStringLiteral("brightness sideways"),
        QStringLiteral("brightness level"),
        QStringLiteral("monitor"),
        QStringLiteral("monitor sideways"),
        QStringLiteral("monitor level"),
        QStringLiteral("config"),
        QStringLiteral("config sideways"),
        QStringLiteral("config set"),
        QStringLiteral("config set mode"),
        QStringLiteral("config reset"),
        QStringLiteral("config save"),
        QStringLiteral("config save live"),
        QStringLiteral("config default_mode"),
        QStringLiteral("mark"),
        QStringLiteral("charge"),
        QStringLiteral("fan"),
    };
    for (const QString &words : refusals) {
        const wire::Call call =
                wire::callFor(words.split(QLatin1Char(' '), Qt::SkipEmptyParts));
        QVERIFY2(!call.valid(), qPrintable(words));
        QVERIFY2(!call.error.isEmpty(), qPrintable(words));
    }

    // And a method the interface does not have is not spelled as a command either.
    QString error;
    QVERIFY(wire::commandFor(QStringLiteral("Explode"), {}, &error).isEmpty());
    QVERIFY(!error.isEmpty());
}

void ControlWireTest::aValueWithASpaceIsRefusedRatherThanSplit()
{
    // The daemon's parser splits a line on spaces, so a value carrying one would
    // arrive as two assignments. Refused with the key named, rather than quietly
    // becoming something else.
    QString error;
    const QVariantMap values { { QStringLiteral("curve"), QStringLiteral("10:20, 100:50") } };
    QVERIFY(wire::commandFor(QStringLiteral("SetLive"), { values }, &error).isEmpty());
    QVERIFY(error.contains(QStringLiteral("curve")));

    // A number from a caller on the bus is as good as the text of one: the parser
    // takes the text of either.
    QCOMPARE(wire::commandFor(QStringLiteral("SetLive"),
                              { QVariantMap { { QStringLiteral("pl1"), 30 } } }, nullptr),
             QStringLiteral("set pl1=30"));
}

QTEST_MAIN(ControlWireTest)
#include "control_wire_test.moc"

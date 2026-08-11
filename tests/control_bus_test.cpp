// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "control_bus.h"
#include "control_wire.h"
#include "daemon.h"
#include "fake_bus.h"
#include "fake_sysfs.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

// The daemon's own interface, end to end.
//
// Everything here happens on a bus the case started, with something of the suite's
// own standing in for polkit. The daemon is an object in this process rather than a
// child of it, because a delayed reply and an asynchronous authorisation are exactly
// what has to be exercised, and both need this event loop to keep turning.

namespace {

constexpr const char *kPropertiesInterface = "org.freedesktop.DBus.Properties";
constexpr const char *kIntrospectable = "org.freedesktop.DBus.Introspectable";

// `GetAll` hands a dictionary back with the values still in their wire form, so a
// dictionary inside it is a `QDBusArgument` rather than a map. That is what the
// client's own unpacking is for; here it is done in one line, out loud, because a
// case that forgot it would read every nested property as empty and say the daemon
// published nothing.
QVariantMap nested(const QVariant &value)
{
    return qdbus_cast<QVariantMap>(value);
}

// Records what the daemon announces. Connected by name rather than through a
// generated proxy, so nothing depends on introspection succeeding first.
class ChangeCatcher : public QObject {
    Q_OBJECT

public:
    QList<QVariantMap> changes;
    QStringList interfaces;

    // The keys named across every announcement so far.
    QStringList keys() const
    {
        QStringList list;
        for (const QVariantMap &change : changes) {
            for (auto entry = change.constBegin(); entry != change.constEnd(); ++entry) {
                if (!list.contains(entry.key()))
                    list.append(entry.key());
            }
        }
        return list;
    }

public slots:
    void onPropertiesChanged(const QString &interfaceName, const QVariantMap &changed,
                             const QStringList &)
    {
        interfaces.append(interfaceName);
        changes.append(changed);
    }
};

} // namespace

class ControlBusTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void everythingIsReadInOneExchange();
    void onlyWhatChangedIsAnnounced();
    void methodsApplyAndAnswerWithTheDaemonsOwnSentence();
    void aRefusalIsAnErrorAndChangesNothing();
    void theSubjectIsTheCallerAndNoDialogIsEverAsked();
    void aRefusedCommandIsAnErrorRatherThanASentence();
    void aSecondDaemonOnTheSameNameIsRefused();
    void theDaemonRunsWithoutABusAndClaimsTheNameWhenItAppears();
    void theIntrospectionMatchesTheDefinition();

private:
    QString service() const { return QString::fromLatin1(wire::kService) + m_suffix; }
    QDBusMessage call(const QString &method, const QVariantList &arguments = {});
    QVariantMap readAll();
    QString introspect();

    PrivateBus m_bus;
    QString m_suffix;
    int m_run = 0;
    std::unique_ptr<FakeAuthority> m_authority;
    QDBusConnection m_verify { QString() };
    QString m_verifyName;
    std::unique_ptr<QTemporaryDir> m_work;
    std::unique_ptr<FakeSysfs> m_tree;
    std::unique_ptr<Daemon> m_daemon;
    std::unique_ptr<ControlBus> m_control;
};

void ControlBusTest::initTestCase()
{
    QString error;
    QVERIFY2(m_bus.start(&error), qPrintable(error));
    qDBusRegisterMetaType<QList<QVariantMap>>();
}

void ControlBusTest::init()
{
    ++m_run;
    // A suffix per case, so the real name is never even spelled and nothing outlives
    // the case that claimed it.
    m_suffix = QStringLiteral(".t%1").arg(m_run);

    QString error;
    m_authority = std::make_unique<FakeAuthority>();
    QVERIFY2(m_authority->start(m_bus.address(), &error), qPrintable(error));

    m_tree = std::make_unique<FakeSysfs>();
    m_work = std::make_unique<QTemporaryDir>();
    QVERIFY(m_work->isValid());

    DaemonOptions options;
    options.sysfsRoot = m_tree->root();
    options.configPath = m_work->path() + QStringLiteral("/settings.conf");
    options.statePath = m_work->path() + QStringLiteral("/state");
    options.ringPath = m_work->path() + QStringLiteral("/metrics.ring");
    options.ringCapacity = 64;
    options.threadedMonitors = false;
    m_daemon = std::make_unique<Daemon>(options);
    QVERIFY2(m_daemon->start(&error), qPrintable(error));

    ControlBus::Options busOptions;
    busOptions.busAddress = m_bus.address();
    busOptions.nameSuffix = m_suffix;
    busOptions.retryMs = 20;
    m_control = std::make_unique<ControlBus>(*m_daemon, busOptions);
    QVERIFY2(m_control->start(&error), qPrintable(error));
    QVERIFY(m_control->owned());

    m_verifyName = QStringLiteral("verify%1").arg(m_run);
    m_verify = QDBusConnection::connectToBus(m_bus.address(), m_verifyName);
    QVERIFY(m_verify.isConnected());
}

void ControlBusTest::cleanup()
{
    m_control.reset();
    m_daemon.reset();
    m_authority.reset();
    m_verify = QDBusConnection(QString());
    QDBusConnection::disconnectFromBus(m_verifyName);
    m_work.reset();
    m_tree.reset();
}

QDBusMessage ControlBusTest::call(const QString &method, const QVariantList &arguments)
{
    QDBusMessage message = QDBusMessage::createMethodCall(
            service(), QString::fromLatin1(wire::kPath), QString::fromLatin1(wire::kInterface),
            method);
    message.setArguments(arguments);
    // The daemon and the authority are both in this process, so the reply only
    // arrives if the event loop keeps running while the call is outstanding.
    return m_verify.call(message, QDBus::BlockWithGui, 5000);
}

QVariantMap ControlBusTest::readAll()
{
    QDBusMessage message = QDBusMessage::createMethodCall(
            service(), QString::fromLatin1(wire::kPath),
            QString::fromLatin1(kPropertiesInterface), QStringLiteral("GetAll"));
    message.setArguments({ QString::fromLatin1(wire::kInterface) });
    const QDBusMessage reply = m_verify.call(message, QDBus::BlockWithGui, 5000);
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty())
        return {};
    // Deliberately not unpacked here: this is what the wire hands back, and the
    // client's own unpacking is what the console's tests exercise.
    return qdbus_cast<QVariantMap>(reply.arguments().constFirst());
}

QString ControlBusTest::introspect()
{
    QDBusMessage message = QDBusMessage::createMethodCall(
            service(), QString::fromLatin1(wire::kPath), QString::fromLatin1(kIntrospectable),
            QStringLiteral("Introspect"));
    const QDBusMessage reply = m_verify.call(message, QDBus::BlockWithGui, 5000);
    return reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty()
            ? reply.arguments().constFirst().toString()
            : QString();
}

void ControlBusTest::everythingIsReadInOneExchange()
{
    const QVariantMap properties = readAll();
    // One exchange, and everything in it: a console that had to ask per property
    // would be back to polling by another name.
    QVERIFY(!properties.isEmpty());
    QCOMPARE(properties.value(QStringLiteral("Mode")).toString(), m_daemon->activeModeId());
    QVERIFY(!nested(properties.value(QStringLiteral("Readings"))).isEmpty());
    QVERIFY(properties.contains(QStringLiteral("Configuration")));
    QVERIFY(properties.contains(QStringLiteral("Monitors")));
    QCOMPARE(properties.value(QStringLiteral("RetentionDays")).toInt(), kRetentionDefaultDays);
    QCOMPARE(properties.value(QStringLiteral("LiveTuning")).toBool(), false);

    // And the same picture the command line prints comes out of it, which is the
    // whole reason the split has an inverse.
    const QJsonObject status = wire::statusJsonFrom(
            wire::propertiesFrom(m_daemon->statusObject(), m_daemon->configurationObject()));
    QCOMPARE(status.value(QStringLiteral("mode")).toString(), m_daemon->activeModeId());
}

void ControlBusTest::onlyWhatChangedIsAnnounced()
{
    ChangeCatcher catcher;
    QVERIFY(m_verify.connect(service(), QString::fromLatin1(wire::kPath),
                             QString::fromLatin1(kPropertiesInterface),
                             QStringLiteral("PropertiesChanged"), &catcher,
                             SLOT(onPropertiesChanged(QString, QVariantMap, QStringList))));

    // A tick moves the readings and nothing else, so that is all it announces. The
    // depth of the ring has no business arriving as part of a fan speed.
    m_daemon->tick();
    QTRY_VERIFY(!catcher.changes.isEmpty());
    QCOMPARE(catcher.interfaces.constFirst(), QString::fromLatin1(wire::kInterface));
    QVERIFY(catcher.keys().contains(QString::fromLatin1(wire::kReadings)));
    QVERIFY(!catcher.keys().contains(QStringLiteral("RetentionDays")));

    // Changing something else names it, and the announcement carries its new value.
    catcher.changes.clear();
    const QDBusMessage reply = call(QStringLiteral("Configure"),
                                    { QStringLiteral("metrics"),
                                      QVariantMap { { QStringLiteral("retention_days"), 9 } } });
    QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
    QTRY_VERIFY(catcher.keys().contains(QStringLiteral("RetentionDays")));
    for (const QVariantMap &change : catcher.changes) {
        if (change.contains(QStringLiteral("RetentionDays")))
            QCOMPARE(change.value(QStringLiteral("RetentionDays")).toInt(), 9);
    }
    QCOMPARE(readAll().value(QStringLiteral("RetentionDays")).toInt(), 9);
}

void ControlBusTest::methodsApplyAndAnswerWithTheDaemonsOwnSentence()
{
    // A mode, and the answer is the mode that took effect.
    const QDBusMessage mode = call(QStringLiteral("SetMode"), { QStringLiteral("quiet") });
    QCOMPARE(mode.type(), QDBusMessage::ReplyMessage);
    QCOMPARE(mode.arguments().constFirst().toString(), QStringLiteral("quiet"));
    QCOMPARE(m_daemon->activeModeId(), QStringLiteral("quiet"));

    // Live tuning, as a dictionary rather than as a line of text. A number and the
    // text of a number are both accepted, because the daemon's parser reads the text
    // of either.
    QCOMPARE(call(QStringLiteral("SetLive"),
                  { QVariantMap { { QStringLiteral("pl1"), 14 } } })
                     .type(),
             QDBusMessage::ReplyMessage);
    QVERIFY(m_daemon->liveTuning());
    QCOMPARE(m_daemon->requestedSetpoints().pl1W, 14);
    QCOMPARE(call(QStringLiteral("SetLive"),
                  { QVariantMap { { QStringLiteral("pl1"), QStringLiteral("16") } } })
                     .type(),
             QDBusMessage::ReplyMessage);
    QCOMPARE(m_daemon->requestedSetpoints().pl1W, 16);

    QCOMPARE(call(QStringLiteral("ResetLive")).type(), QDBusMessage::ReplyMessage);
    QVERIFY(!m_daemon->liveTuning());

    // A verdict, which is a change to what is recorded rather than to the machine,
    // and is checked like every other change.
    QCOMPARE(call(QStringLiteral("Mark"), { QStringLiteral("good") }).type(),
             QDBusMessage::ReplyMessage);

    // And a value past a bound is applied at the bound rather than refused: an
    // experiment that lands somewhere legal is not an error, and live tuning clamps
    // without a word about it — the number the console reads back is the answer.
    const QDBusMessage clamped =
            call(QStringLiteral("SetLive"),
                 { QVariantMap { { QStringLiteral("pl1"), 500 } } });
    QCOMPARE(clamped.type(), QDBusMessage::ReplyMessage);
    QVERIFY(m_daemon->requestedSetpoints().pl1W < 500);
    QCOMPARE(nested(readAll().value(QStringLiteral("Requested")))
                     .value(QStringLiteral("pl1_w")).toInt(),
             m_daemon->requestedSetpoints().pl1W);

    // Writing the definition down is the other kind of change, and there a clamp is
    // said out loud: a number in a file that is not the number somebody typed has to
    // be reported.
    const QDBusMessage saved =
            call(QStringLiteral("Configure"),
                 { QStringLiteral("mode"),
                   QVariantMap { { QStringLiteral("id"), QStringLiteral("quiet") },
                                 { QStringLiteral("pl1_w"), 500 } } });
    QCOMPARE(saved.type(), QDBusMessage::ReplyMessage);
    QVERIFY(!saved.arguments().constFirst().toString().isEmpty());
}

void ControlBusTest::aRefusalIsAnErrorAndChangesNothing()
{
    const QString before = m_daemon->activeModeId();
    m_authority->setAllowed(false);

    const QDBusMessage reply = call(QStringLiteral("SetMode"), { QStringLiteral("quiet") });
    QCOMPARE(reply.type(), QDBusMessage::ErrorMessage);
    // Its own error name, so a client can tell "you may not" from "that made no
    // sense".
    QCOMPARE(reply.errorName(), QStringLiteral("io.github.svesh87.Sombra.Error.Refused"));
    QCOMPARE(m_daemon->activeModeId(), before);

    // Reading is not guarded: what the machine is doing is not a secret, and a
    // console that could not show it would have nothing to refuse from.
    QVERIFY(!readAll().isEmpty());

    // Allowed again, and the same call goes through — the refusal was the authority's
    // answer rather than something the daemon remembered.
    m_authority->setAllowed(true);
    QCOMPARE(call(QStringLiteral("SetMode"), { QStringLiteral("quiet") }).type(),
             QDBusMessage::ReplyMessage);
    QCOMPARE(m_daemon->activeModeId(), QStringLiteral("quiet"));
}

void ControlBusTest::theSubjectIsTheCallerAndNoDialogIsEverAsked()
{
    QCOMPARE(call(QStringLiteral("Reload")).type(), QDBusMessage::ReplyMessage);
    QCOMPARE(m_authority->actions(), QStringList { QStringLiteral(
                                             "io.github.svesh87.Sombra.manage") });
    // The unique name of whoever called, not the well-known name the daemon answers
    // under: the question is who is asking, and polkit resolves that name to a
    // session itself.
    QCOMPARE(m_authority->subjects().constFirst(), m_verify.baseService());
    QVERIFY(m_authority->subjects().constFirst() != service());
    // And never a dialog. The action is answered by `allow_active` or not at all.
    QVERIFY(!m_authority->interactionAsked());
}

void ControlBusTest::aRefusedCommandIsAnErrorRatherThanASentence()
{
    // The daemon's own refusal, carried as an error with its own name: a mode that
    // does not exist is not a mode selected.
    const QDBusMessage reply = call(QStringLiteral("SetMode"), { QStringLiteral("teapot") });
    QCOMPARE(reply.type(), QDBusMessage::ErrorMessage);
    QCOMPARE(reply.errorName(), QStringLiteral("io.github.svesh87.Sombra.Error.Failed"));
    QVERIFY(!reply.errorMessage().isEmpty());
    // Framing does not travel: the client shows the sentence, not the word in front
    // of it.
    QVERIFY(!reply.errorMessage().startsWith(QStringLiteral("err")));

    // And a call the interface has but the arguments do not fit is the same kind of
    // answer rather than a crash.
    QCOMPARE(call(QStringLiteral("Configure"), { QStringLiteral("teapot"), QVariantMap {} })
                     .type(),
             QDBusMessage::ErrorMessage);
}

void ControlBusTest::aSecondDaemonOnTheSameNameIsRefused()
{
    // Two owners of one control surface is the single startup condition worth
    // refusing over, and on the bus that is the name already having an owner.
    DaemonOptions options;
    options.sysfsRoot = m_tree->root();
    options.configPath = m_work->path() + QStringLiteral("/second.conf");
    options.statePath = m_work->path() + QStringLiteral("/second.state");
    options.ringPath = m_work->path() + QStringLiteral("/second.ring");
    options.ringCapacity = 64;
    options.threadedMonitors = false;
    Daemon second(options);
    QString error;
    QVERIFY2(second.start(&error), qPrintable(error));

    ControlBus::Options busOptions;
    busOptions.busAddress = m_bus.address();
    busOptions.nameSuffix = m_suffix;
    busOptions.retryMs = 20;
    ControlBus rival(second, busOptions);
    QVERIFY(!rival.start(&error));
    QVERIFY(error.contains(service()));
    QVERIFY(!rival.owned());
}

void ControlBusTest::theDaemonRunsWithoutABusAndClaimsTheNameWhenItAppears()
{
    // The daemon owns the thermal envelope, so a bus that is not there yet is not a
    // reason to stop managing the machine. The address has to be known before the bus
    // exists, which is why this one is asked for by path rather than printed.
    const QString socket = m_work->path() + QStringLiteral("/late-bus");
    const QString address = QStringLiteral("unix:path=") + socket;

    DaemonOptions options;
    options.sysfsRoot = m_tree->root();
    options.configPath = m_work->path() + QStringLiteral("/late.conf");
    options.statePath = m_work->path() + QStringLiteral("/late.state");
    options.ringPath = m_work->path() + QStringLiteral("/late.ring");
    options.ringCapacity = 64;
    options.threadedMonitors = false;
    Daemon late(options);
    QString error;
    QVERIFY2(late.start(&error), qPrintable(error));

    ControlBus::Options busOptions;
    busOptions.busAddress = address;
    busOptions.nameSuffix = QStringLiteral(".late");
    busOptions.retryMs = 20;
    ControlBus control(late, busOptions);
    // Started, running, and not answering on a bus that is not there.
    QVERIFY2(control.start(&error), qPrintable(error));
    QVERIFY(!control.owned());
    late.tick();
    QCOMPARE(late.tickCount(), 1u);

    PrivateBus late_bus;
    QVERIFY2(late_bus.startAt(socket, &error), qPrintable(error));
    // And it takes the name by itself, without anybody asking it to.
    QTRY_VERIFY(control.owned());
    QCOMPARE(control.name(), QString::fromLatin1(wire::kService) + QStringLiteral(".late"));
}

void ControlBusTest::theIntrospectionMatchesTheDefinition()
{
    QFile file(QString::fromLatin1(WIRE_XML));
    QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(file.errorString()));
    const QString definition = QString::fromUtf8(file.readAll());

    const Introspection declared =
            parseIntrospection(definition, QString::fromLatin1(wire::kInterface));
    const Introspection live =
            parseIntrospection(introspect(), QString::fromLatin1(wire::kInterface));

    // The adaptor is written by hand, because a method that waits for polkit has to
    // delay its reply and a generated one cannot reach the message it is answering.
    // This is what keeps "the wire is described in one place" a fact rather than a
    // promise: signatures, not argument names, because the file names its arguments
    // for a reader and the metaobject does not have to agree about that.
    QVERIFY(!declared.methods.isEmpty());
    QCOMPARE(live.methods, declared.methods);
    QCOMPARE(live.properties, declared.properties);
}

QTEST_MAIN(ControlBusTest)
#include "control_bus_test.moc"

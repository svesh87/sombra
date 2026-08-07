// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "daemon.h"
#include "dbus_service.h"
#include "fake_sysfs.h"

#include <QCoreApplication>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusReply>
#include <QDBusVariant>
#include <QDeadlineTimer>
#include <QFile>
#include <QMap>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>
#include <QVariantMap>
#include <QXmlStreamReader>

#include <functional>
#include <memory>

namespace testwire {

// The (yy) structure com.system76.PowerDaemon.SetChargeThresholds takes. The
// service registers its own C++ type for it; what the two sides share is the
// wire signature, not the declaration.
struct Thresholds {
    uchar start = 0;
    uchar end = 100;
};

QDBusArgument &operator<<(QDBusArgument &argument, const Thresholds &thresholds)
{
    argument.beginStructure();
    argument << thresholds.start << thresholds.end;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, Thresholds &thresholds)
{
    argument.beginStructure();
    argument >> thresholds.start >> thresholds.end;
    argument.endStructure();
    return argument;
}

} // namespace testwire

Q_DECLARE_METATYPE(testwire::Thresholds)

namespace {

constexpr char kHadessName[] = "net.hadess.PowerProfiles";
constexpr char kHadessPath[] = "/net/hadess/PowerProfiles";
constexpr char kUPowerName[] = "org.freedesktop.UPower.PowerProfiles";
constexpr char kUPowerPath[] = "/org/freedesktop/UPower/PowerProfiles";
constexpr char kVendorName[] = "com.system76.PowerDaemon";
constexpr char kVendorPath[] = "/com/system76/PowerDaemon";
constexpr char kPropertiesName[] = "org.freedesktop.DBus.Properties";

// What one interface of an introspection document declares. Comparing whole
// maps is what turns "the vendor signature set is reproduced" into a check
// rather than a claim.
struct Introspection {
    // Method name -> "<input signature> -> <output signature>".
    QMap<QString, QString> methods;
    // Signal name -> concatenated argument signature.
    QMap<QString, QString> signals_;
    // Property name -> "<signature> <access>".
    QMap<QString, QString> properties;
};

Introspection parseIntrospection(const QString &xml, const QString &interfaceName)
{
    Introspection result;
    QXmlStreamReader reader(xml);
    QString currentInterface;
    QString member;
    bool memberIsSignal = false;
    QString inputs;
    QString outputs;

    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement()) {
            const QString element = reader.name().toString();
            const QXmlStreamAttributes attributes = reader.attributes();
            if (element == QLatin1String("interface")) {
                currentInterface = attributes.value(QLatin1String("name")).toString();
            } else if (element == QLatin1String("method")
                       || element == QLatin1String("signal")) {
                member = attributes.value(QLatin1String("name")).toString();
                memberIsSignal = element == QLatin1String("signal");
                inputs.clear();
                outputs.clear();
            } else if (element == QLatin1String("arg") && !member.isEmpty()) {
                const QString type = attributes.value(QLatin1String("type")).toString();
                if (attributes.value(QLatin1String("direction")) == QLatin1String("out"))
                    outputs += type;
                else
                    inputs += type;
            } else if (element == QLatin1String("property")
                       && currentInterface == interfaceName) {
                result.properties.insert(attributes.value(QLatin1String("name")).toString(),
                                         attributes.value(QLatin1String("type")).toString()
                                                 + QLatin1Char(' ')
                                                 + attributes.value(QLatin1String("access"))
                                                           .toString());
            }
        } else if (reader.isEndElement()) {
            const QString element = reader.name().toString();
            if (element != QLatin1String("method") && element != QLatin1String("signal"))
                continue;
            if (currentInterface == interfaceName) {
                if (memberIsSignal)
                    result.signals_.insert(member, inputs + outputs);
                else
                    result.methods.insert(member, inputs + QStringLiteral(" -> ") + outputs);
            }
            member.clear();
        }
    }
    return result;
}

QList<QVariantMap> toMapList(const QVariant &value)
{
    if (value.metaType() != QMetaType::fromType<QDBusArgument>())
        return value.value<QList<QVariantMap>>();
    QList<QVariantMap> list;
    const QDBusArgument argument = value.value<QDBusArgument>();
    argument >> list;
    return list;
}

} // namespace

// Records the bus signals the service is supposed to send. A slot on a plain
// receiver is used rather than a proxy interface so that the connection is by
// name and nothing depends on introspection succeeding first.
class SignalCatcher : public QObject {
    Q_OBJECT

public:
    QStringList profileSwitches;
    QList<uint> releasedCookies;
    QList<QVariantMap> propertyChanges;
    QStringList changedInterfaces;

public slots:
    void onPowerProfileSwitch(const QString &profile) { profileSwitches.append(profile); }
    void onProfileReleased(uint cookie) { releasedCookies.append(cookie); }
    void onPropertiesChanged(const QString &interfaceName, const QVariantMap &changed,
                             const QStringList &)
    {
        changedInterfaces.append(interfaceName);
        propertyChanges.append(changed);
    }
};

class DBusServiceTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

    void claimsTheThreeNames();
    void reportsAnUnreachableBus();
    void reportsNamesAlreadyHeld();
    void theStandardInterfaceIsDeclaredInFull();
    void theVendorSignatureSetIsReproduced();
    void profilesAreListedWithTheirDrivers();
    void activeProfileIsReadAndWritten();
    void aModeChangeIsAnnouncedOnEveryInterface();
    void profilesAreHeldAndReleased();
    void performanceDegradedFollowsTheThermalGuard();
    void theVendorInterfaceSelectsModesAndReportsThem();
    void theGraphicsHalfAnswersWithConstants();
    void chargeThresholdsReachTheControlSurface();

private:
    QString service(const char *base) const { return QLatin1String(base) + m_suffix; }
    QDBusMessage call(const QString &target, const char *path, const char *interfaceName,
                      const QString &method, const QVariantList &arguments = {});
    QDBusMessage getProperty(const QString &target, const char *path, const char *interfaceName,
                             const QString &name);
    QString introspect(const QString &target, const char *path);
    static bool waitFor(const std::function<bool()> &done);

    QProcess m_bus;
    QString m_address;
    QString m_suffix;
    int m_run = 0;

    QDBusConnection m_verify { QString() };
    QString m_verifyName;
    std::unique_ptr<QTemporaryDir> m_work;
    std::unique_ptr<FakeSysfs> m_tree;
    std::unique_ptr<Daemon> m_daemon;
    std::unique_ptr<DBusService> m_service;
};

void DBusServiceTest::initTestCase()
{
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0-test"));
    qDBusRegisterMetaType<testwire::Thresholds>();

    // A private bus. Claiming these names on the real one would take them away
    // from the running desktop, so no test ever touches it.
    m_bus.start(QStringLiteral("dbus-daemon"),
                { QStringLiteral("--session"), QStringLiteral("--print-address"),
                  QStringLiteral("--nofork") });
    QVERIFY2(m_bus.waitForStarted(5000), "dbus-daemon did not start");
    QVERIFY2(m_bus.waitForReadyRead(5000), "dbus-daemon printed no address");
    m_address = QString::fromUtf8(m_bus.readLine()).trimmed();
    QVERIFY2(!m_address.isEmpty(), "dbus-daemon printed an empty address");
}

void DBusServiceTest::cleanupTestCase()
{
    m_bus.terminate();
    if (!m_bus.waitForFinished(5000))
        m_bus.kill();
}

void DBusServiceTest::init()
{
    ++m_run;
    // A suffix per case, so a name outlives nothing and the real names are
    // never even spelled. It is appended verbatim, dot included.
    m_suffix = QStringLiteral(".t%1").arg(m_run);

    m_tree = std::make_unique<FakeSysfs>();
    m_work = std::make_unique<QTemporaryDir>();
    QVERIFY(m_work->isValid());

    DaemonOptions options;
    options.sysfsRoot = m_tree->root();
    options.configPath = m_work->path() + QStringLiteral("/modes.conf");
    options.statePath = m_work->path() + QStringLiteral("/state");
    options.ringPath = m_work->path() + QStringLiteral("/metrics.ring");
    options.socketPath = m_work->path() + QStringLiteral("/control.sock");
    options.ringCapacity = 64;
    m_daemon = std::make_unique<Daemon>(options);
    QString error;
    QVERIFY2(m_daemon->start(&error), qPrintable(error));

    DBusService::Options busOptions;
    busOptions.busAddress = m_address;
    busOptions.nameSuffix = m_suffix;
    m_service = std::make_unique<DBusService>(*m_daemon, busOptions);
    QVERIFY2(m_service->start(&error), qPrintable(error));

    m_verifyName = QStringLiteral("verify%1").arg(m_run);
    m_verify = QDBusConnection::connectToBus(m_address, m_verifyName);
    QVERIFY(m_verify.isConnected());
}

void DBusServiceTest::cleanup()
{
    m_service.reset();
    m_daemon.reset();
    m_verify = QDBusConnection(QString());
    QDBusConnection::disconnectFromBus(m_verifyName);
    m_work.reset();
    m_tree.reset();
}

QDBusMessage DBusServiceTest::call(const QString &target, const char *path,
                                   const char *interfaceName, const QString &method,
                                   const QVariantList &arguments)
{
    QDBusMessage message = QDBusMessage::createMethodCall(
            target, QLatin1String(path), QLatin1String(interfaceName), method);
    message.setArguments(arguments);
    // The service is exported from this same process, so the reply only
    // arrives if the event loop keeps running while the call is outstanding.
    return m_verify.call(message, QDBus::BlockWithGui, 5000);
}

QDBusMessage DBusServiceTest::getProperty(const QString &target, const char *path,
                                          const char *interfaceName, const QString &name)
{
    return call(target, path, kPropertiesName, QStringLiteral("Get"),
                { QLatin1String(interfaceName), name });
}

QString DBusServiceTest::introspect(const QString &target, const char *path)
{
    const QDBusMessage reply = call(target, path,
                                    "org.freedesktop.DBus.Introspectable",
                                    QStringLiteral("Introspect"));
    return reply.arguments().value(0).toString();
}

bool DBusServiceTest::waitFor(const std::function<bool()> &done)
{
    QDeadlineTimer deadline(5000);
    while (!done() && !deadline.hasExpired())
        QTest::qWait(10);
    return done();
}

void DBusServiceTest::claimsTheThreeNames()
{
    QCOMPARE(m_service->claimedNames(),
             QStringList({ service(kHadessName), service(kUPowerName), service(kVendorName) }));
    // The middle mode is the one the machine boots into, and it is what the
    // panel reads back.
    QCOMPARE(m_service->activeProfile(), QStringLiteral("balanced"));

    const QDBusReply<QStringList> names = m_verify.interface()->registeredServiceNames();
    QVERIFY(names.isValid());
    for (const QString &name : m_service->claimedNames())
        QVERIFY2(names.value().contains(name), qPrintable(name));
}

void DBusServiceTest::reportsAnUnreachableBus()
{
    DBusService::Options options;
    options.busAddress = QStringLiteral("unix:path=/nonexistent/ombra-test-bus");
    options.nameSuffix = m_suffix + QStringLiteral(".unreachable");
    DBusService orphan(*m_daemon, options);

    QString error;
    QVERIFY(!orphan.start(&error));
    QVERIFY(!error.isEmpty());
    QVERIFY(orphan.claimedNames().isEmpty());
}

void DBusServiceTest::reportsNamesAlreadyHeld()
{
    // The names of this case are already claimed by the service the fixture
    // started, and a second holder is refused every one of them.
    DBusService::Options options;
    options.busAddress = m_address;
    options.nameSuffix = m_suffix;
    DBusService second(*m_daemon, options);

    QString error;
    QVERIFY(!second.start(&error));
    QVERIFY(!error.isEmpty());
    QVERIFY(second.claimedNames().isEmpty());
    // The first service keeps everything it had.
    QCOMPARE(m_service->claimedNames().size(), 3);
}

void DBusServiceTest::theStandardInterfaceIsDeclaredInFull()
{
    for (const auto &pair : { std::make_pair(kHadessName, kHadessPath),
                              std::make_pair(kUPowerName, kUPowerPath) }) {
        const Introspection found = parseIntrospection(introspect(service(pair.first), pair.second),
                                                       QLatin1String(pair.first));

        const QMap<QString, QString> methods {
            { QStringLiteral("HoldProfile"), QStringLiteral("sss -> u") },
            { QStringLiteral("ReleaseProfile"), QStringLiteral("u -> ") },
        };
        QCOMPARE(found.methods, methods);

        const QMap<QString, QString> signals_ {
            { QStringLiteral("ProfileReleased"), QStringLiteral("u") },
        };
        QCOMPARE(found.signals_, signals_);

        const QMap<QString, QString> properties {
            { QStringLiteral("ActiveProfile"), QStringLiteral("s readwrite") },
            { QStringLiteral("Profiles"), QStringLiteral("aa{sv} read") },
            { QStringLiteral("Actions"), QStringLiteral("as read") },
            { QStringLiteral("ActiveProfileHolds"), QStringLiteral("aa{sv} read") },
            { QStringLiteral("PerformanceInhibited"), QStringLiteral("s read") },
            { QStringLiteral("PerformanceDegraded"), QStringLiteral("s read") },
            { QStringLiteral("Version"), QStringLiteral("s read") },
        };
        QCOMPARE(found.properties, properties);
    }

    const QDBusMessage version =
            getProperty(service(kHadessName), kHadessPath, kHadessName,
                        QStringLiteral("Version"));
    QCOMPARE(version.arguments().value(0).value<QDBusVariant>().variant().toString(),
             QCoreApplication::applicationVersion());
    // Nothing on this platform forbids the performance profile outright.
    QCOMPARE(getProperty(service(kUPowerName), kUPowerPath, kUPowerName,
                         QStringLiteral("PerformanceInhibited"))
                     .arguments()
                     .value(0)
                     .value<QDBusVariant>()
                     .variant()
                     .toString(),
             QString());
    QVERIFY(getProperty(service(kHadessName), kHadessPath, kHadessName,
                        QStringLiteral("Actions"))
                    .arguments()
                    .value(0)
                    .value<QDBusVariant>()
                    .variant()
                    .toStringList()
                    .isEmpty());
}

void DBusServiceTest::theVendorSignatureSetIsReproduced()
{
    const Introspection found =
            parseIntrospection(introspect(service(kVendorName), kVendorPath),
                               QLatin1String(kVendorName));

    // Exactly what the vendor daemon introspects on this machine. The applet
    // binds to this set, so anything missing or renamed breaks the panel.
    const QMap<QString, QString> methods {
        { QStringLiteral("AutoGraphicsPower"), QStringLiteral(" -> ") },
        { QStringLiteral("Balanced"), QStringLiteral(" -> ") },
        { QStringLiteral("Battery"), QStringLiteral(" -> ") },
        { QStringLiteral("GetChargeProfiles"), QStringLiteral(" -> a(sssyy)") },
        { QStringLiteral("GetChargeThresholds"), QStringLiteral(" -> yy") },
        { QStringLiteral("GetDefaultGraphics"), QStringLiteral(" -> s") },
        { QStringLiteral("GetDesktop"), QStringLiteral(" -> b") },
        { QStringLiteral("GetExternalDisplaysRequireDgpu"), QStringLiteral(" -> b") },
        { QStringLiteral("GetGraphics"), QStringLiteral(" -> s") },
        { QStringLiteral("GetGraphicsPower"), QStringLiteral(" -> b") },
        { QStringLiteral("GetProfile"), QStringLiteral(" -> s") },
        { QStringLiteral("GetSwitchable"), QStringLiteral(" -> b") },
        { QStringLiteral("Performance"), QStringLiteral(" -> ") },
        { QStringLiteral("SetChargeThresholds"), QStringLiteral("(yy) -> ") },
        { QStringLiteral("SetGraphics"), QStringLiteral("s -> ") },
        { QStringLiteral("SetGraphicsPower"), QStringLiteral("b -> ") },
    };
    QCOMPARE(found.methods, methods);

    const QMap<QString, QString> signals_ {
        { QStringLiteral("HotPlugDetect"), QStringLiteral("t") },
        { QStringLiteral("PowerProfileSwitch"), QStringLiteral("s") },
    };
    QCOMPARE(found.signals_, signals_);
    QVERIFY(found.properties.isEmpty());
}

void DBusServiceTest::profilesAreListedWithTheirDrivers()
{
    const QDBusMessage reply =
            getProperty(service(kHadessName), kHadessPath, kHadessName,
                        QStringLiteral("Profiles"));
    QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
    const QList<QVariantMap> profiles =
            toMapList(reply.arguments().value(0).value<QDBusVariant>().variant());

    QCOMPARE(profiles.size(), 3);
    QStringList names;
    for (const QVariantMap &entry : profiles) {
        names.append(entry.value(QStringLiteral("Profile")).toString());
        QCOMPARE(entry.value(QStringLiteral("CpuDriver")).toString(),
                 QStringLiteral("intel_pstate"));
        QCOMPARE(entry.value(QStringLiteral("PlatformDriver")).toString(),
                 QStringLiteral("platform_profile"));
        QVERIFY(!entry.value(QStringLiteral("Driver")).toString().isEmpty());
    }
    QCOMPARE(names, QStringList({ QStringLiteral("power-saver"), QStringLiteral("balanced"),
                                  QStringLiteral("performance") }));

    // No hold is outstanding, so the list of them is empty rather than absent.
    const QDBusMessage holds =
            getProperty(service(kUPowerName), kUPowerPath, kUPowerName,
                        QStringLiteral("ActiveProfileHolds"));
    QCOMPARE(holds.type(), QDBusMessage::ReplyMessage);
    QVERIFY(toMapList(holds.arguments().value(0).value<QDBusVariant>().variant()).isEmpty());
}

void DBusServiceTest::activeProfileIsReadAndWritten()
{
    QDBusMessage reply = getProperty(service(kHadessName), kHadessPath, kHadessName,
                                     QStringLiteral("ActiveProfile"));
    QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
    QCOMPARE(reply.arguments().value(0).value<QDBusVariant>().variant().toString(),
             QStringLiteral("balanced"));

    // A write through the panel is a mode selection, and the mode is what the
    // daemon actually applies.
    reply = call(service(kUPowerName), kUPowerPath, kPropertiesName, QStringLiteral("Set"),
                 { QLatin1String(kUPowerName), QStringLiteral("ActiveProfile"),
                   QVariant::fromValue(QDBusVariant(QStringLiteral("power-saver"))) });
    QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
    QCOMPARE(m_daemon->activeModeId(), QStringLiteral("quiet"));
    QCOMPARE(m_service->activeProfile(), QStringLiteral("power-saver"));

    // Both names carry the same state, so the other one reads the new value.
    reply = getProperty(service(kHadessName), kHadessPath, kHadessName,
                        QStringLiteral("ActiveProfile"));
    QCOMPARE(reply.arguments().value(0).value<QDBusVariant>().variant().toString(),
             QStringLiteral("power-saver"));

    // A name that is not one of the three leaves the selection alone.
    call(service(kHadessName), kHadessPath, kPropertiesName, QStringLiteral("Set"),
         { QLatin1String(kHadessName), QStringLiteral("ActiveProfile"),
           QVariant::fromValue(QDBusVariant(QStringLiteral("turbo"))) });
    QCOMPARE(m_daemon->activeModeId(), QStringLiteral("quiet"));
}

void DBusServiceTest::aModeChangeIsAnnouncedOnEveryInterface()
{
    SignalCatcher catcher;
    QVERIFY(m_verify.connect(service(kHadessName), QLatin1String(kHadessPath),
                             QLatin1String(kPropertiesName),
                             QStringLiteral("PropertiesChanged"), &catcher,
                             SLOT(onPropertiesChanged(QString, QVariantMap, QStringList))));
    QVERIFY(m_verify.connect(service(kUPowerName), QLatin1String(kUPowerPath),
                             QLatin1String(kPropertiesName),
                             QStringLiteral("PropertiesChanged"), &catcher,
                             SLOT(onPropertiesChanged(QString, QVariantMap, QStringList))));
    QVERIFY(m_verify.connect(service(kVendorName), QLatin1String(kVendorPath),
                             QLatin1String(kVendorName), QStringLiteral("PowerProfileSwitch"),
                             &catcher, SLOT(onPowerProfileSwitch(QString))));

    // Selected over the control socket rather than over D-Bus: the panel has
    // to hear about a change it did not make.
    m_daemon->handleCommand(QStringLiteral("mode full"));

    QVERIFY(waitFor([&catcher] {
        return catcher.propertyChanges.size() == 2 && catcher.profileSwitches.size() == 1;
    }));
    QCOMPARE(catcher.changedInterfaces,
             QStringList({ QLatin1String(kHadessName), QLatin1String(kUPowerName) }));
    for (const QVariantMap &changed : catcher.propertyChanges)
        QCOMPARE(changed.value(QStringLiteral("ActiveProfile")).toString(),
                 QStringLiteral("performance"));
    QCOMPARE(catcher.profileSwitches, QStringList({ QStringLiteral("performance") }));

    // The vendor vocabulary differs for the coolest mode, and the signal uses
    // the vendor's word for it.
    m_daemon->handleCommand(QStringLiteral("mode quiet"));
    QVERIFY(waitFor([&catcher] { return catcher.profileSwitches.size() == 2; }));
    QCOMPARE(catcher.profileSwitches.last(), QStringLiteral("battery"));
}

void DBusServiceTest::profilesAreHeldAndReleased()
{
    SignalCatcher catcher;
    QVERIFY(m_verify.connect(service(kHadessName), QLatin1String(kHadessPath),
                             QLatin1String(kHadessName), QStringLiteral("ProfileReleased"),
                             &catcher, SLOT(onProfileReleased(uint))));

    QDBusMessage reply = call(service(kHadessName), kHadessPath, kHadessName,
                              QStringLiteral("HoldProfile"),
                              { QStringLiteral("performance"), QStringLiteral("compiling"),
                                QStringLiteral("org.example.Build") });
    QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
    const uint cookie = reply.arguments().value(0).toUInt();
    QVERIFY(cookie != 0);
    QCOMPARE(m_daemon->activeModeId(), QStringLiteral("full"));

    const QDBusMessage holds = getProperty(service(kHadessName), kHadessPath, kHadessName,
                                           QStringLiteral("ActiveProfileHolds"));
    const QList<QVariantMap> entries =
            toMapList(holds.arguments().value(0).value<QDBusVariant>().variant());
    QCOMPARE(entries.size(), 1);
    QCOMPARE(entries.first().value(QStringLiteral("Reason")).toString(),
             QStringLiteral("compiling"));
    QCOMPARE(entries.first().value(QStringLiteral("ApplicationId")).toString(),
             QStringLiteral("org.example.Build"));

    // A second hold, taken through the other name, wins while it lasts.
    reply = call(service(kUPowerName), kUPowerPath, kUPowerName, QStringLiteral("HoldProfile"),
                 { QStringLiteral("power-saver"), QStringLiteral("on battery"),
                   QStringLiteral("org.example.Power") });
    QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
    const uint second = reply.arguments().value(0).toUInt();
    QVERIFY(second != cookie);
    QCOMPARE(m_daemon->activeModeId(), QStringLiteral("quiet"));

    // A profile nobody defines is refused rather than held silently.
    reply = call(service(kHadessName), kHadessPath, kHadessName, QStringLiteral("HoldProfile"),
                 { QStringLiteral("ludicrous"), QStringLiteral("why not"),
                   QStringLiteral("org.example.Build") });
    QCOMPARE(reply.type(), QDBusMessage::ErrorMessage);

    // Dropping the newer hold falls back to the one still outstanding.
    reply = call(service(kHadessName), kHadessPath, kHadessName, QStringLiteral("ReleaseProfile"),
                 { second });
    QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
    QCOMPARE(m_daemon->activeModeId(), QStringLiteral("full"));

    reply = call(service(kHadessName), kHadessPath, kHadessName, QStringLiteral("ReleaseProfile"),
                 { cookie });
    QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
    QVERIFY(waitFor([&catcher] { return catcher.releasedCookies.size() == 2; }));
    QCOMPARE(catcher.releasedCookies, QList<uint>({ second, cookie }));
    // Releasing the last hold returns to what was selected before the first.
    QCOMPARE(m_daemon->activeModeId(), QStringLiteral("work"));

    reply = call(service(kUPowerName), kUPowerPath, kUPowerName, QStringLiteral("ReleaseProfile"),
                 { cookie });
    QCOMPARE(reply.type(), QDBusMessage::ErrorMessage);
}

void DBusServiceTest::performanceDegradedFollowsTheThermalGuard()
{
    QDBusMessage reply = getProperty(service(kHadessName), kHadessPath, kHadessName,
                                     QStringLiteral("PerformanceDegraded"));
    QCOMPARE(reply.arguments().value(0).value<QDBusVariant>().variant().toString(), QString());

    // The guard is an interlock, not a mode: the profile stays where it was
    // and only the reason string changes.
    m_tree->write(FakeSysfs::coretemp() + QStringLiteral("/temp1_input"),
                  QString::number(int(kGuardClampTempDeci) * 100));
    m_daemon->tick();

    reply = getProperty(service(kUPowerName), kUPowerPath, kUPowerName,
                        QStringLiteral("PerformanceDegraded"));
    QCOMPARE(reply.arguments().value(0).value<QDBusVariant>().variant().toString(),
             QStringLiteral("high-operating-temperature"));
    QCOMPARE(m_service->activeProfile(), QStringLiteral("balanced"));

    m_tree->write(FakeSysfs::coretemp() + QStringLiteral("/temp1_input"),
                  QStringLiteral("600"));
    for (int i = 0; i < kGuardReleaseTicks; ++i)
        m_daemon->tick();
    reply = getProperty(service(kHadessName), kHadessPath, kHadessName,
                        QStringLiteral("PerformanceDegraded"));
    QCOMPARE(reply.arguments().value(0).value<QDBusVariant>().variant().toString(), QString());
}

void DBusServiceTest::theVendorInterfaceSelectsModesAndReportsThem()
{
    QDBusMessage reply = call(service(kVendorName), kVendorPath, kVendorName,
                              QStringLiteral("GetProfile"));
    QCOMPARE(reply.arguments().value(0).toString(), QStringLiteral("balanced"));

    QCOMPARE(call(service(kVendorName), kVendorPath, kVendorName, QStringLiteral("Battery")).type(),
             QDBusMessage::ReplyMessage);
    QCOMPARE(m_daemon->activeModeId(), QStringLiteral("quiet"));
    QCOMPARE(call(service(kVendorName), kVendorPath, kVendorName, QStringLiteral("GetProfile"))
                     .arguments()
                     .value(0)
                     .toString(),
             QStringLiteral("battery"));

    QCOMPARE(call(service(kVendorName), kVendorPath, kVendorName, QStringLiteral("Performance"))
                     .type(),
             QDBusMessage::ReplyMessage);
    QCOMPARE(m_daemon->activeModeId(), QStringLiteral("full"));

    QCOMPARE(call(service(kVendorName), kVendorPath, kVendorName, QStringLiteral("Balanced"))
                     .type(),
             QDBusMessage::ReplyMessage);
    QCOMPARE(m_daemon->activeModeId(), QStringLiteral("work"));
    QCOMPARE(call(service(kVendorName), kVendorPath, kVendorName, QStringLiteral("GetProfile"))
                     .arguments()
                     .value(0)
                     .toString(),
             QStringLiteral("balanced"));
}

void DBusServiceTest::theGraphicsHalfAnswersWithConstants()
{
    const QString vendor = service(kVendorName);
    QCOMPARE(call(vendor, kVendorPath, kVendorName, QStringLiteral("GetGraphics"))
                     .arguments()
                     .value(0)
                     .toString(),
             QStringLiteral("integrated"));
    QCOMPARE(call(vendor, kVendorPath, kVendorName, QStringLiteral("GetDefaultGraphics"))
                     .arguments()
                     .value(0)
                     .toString(),
             QStringLiteral("integrated"));
    for (const QString &method :
         { QStringLiteral("GetSwitchable"), QStringLiteral("GetGraphicsPower"),
           QStringLiteral("GetDesktop"), QStringLiteral("GetExternalDisplaysRequireDgpu") }) {
        const QDBusMessage reply = call(vendor, kVendorPath, kVendorName, method);
        QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
        QVERIFY2(!reply.arguments().value(0).toBool(), qPrintable(method));
    }

    // The setters are accepted and do nothing: there is no second GPU, and the
    // display backlight is never touched on any path.
    const QString profile = m_service->activeProfile();
    QCOMPARE(call(vendor, kVendorPath, kVendorName, QStringLiteral("SetGraphics"),
                  { QStringLiteral("nvidia") })
                     .type(),
             QDBusMessage::ReplyMessage);
    QCOMPARE(call(vendor, kVendorPath, kVendorName, QStringLiteral("SetGraphicsPower"),
                  { true })
                     .type(),
             QDBusMessage::ReplyMessage);
    QCOMPARE(call(vendor, kVendorPath, kVendorName, QStringLiteral("AutoGraphicsPower")).type(),
             QDBusMessage::ReplyMessage);
    QCOMPARE(call(vendor, kVendorPath, kVendorName, QStringLiteral("GetGraphics"))
                     .arguments()
                     .value(0)
                     .toString(),
             QStringLiteral("integrated"));
    QCOMPARE(m_service->activeProfile(), profile);
}

void DBusServiceTest::chargeThresholdsReachTheControlSurface()
{
    const QString vendor = service(kVendorName);
    const QString node =
            QStringLiteral("sys/class/power_supply/BAT0/charge_control_end_threshold");

    // Before the first tick the daemon has no reading of the node yet, and an
    // unknown threshold reads as "charge to full" rather than as zero.
    QDBusMessage reply = call(vendor, kVendorPath, kVendorName,
                              QStringLiteral("GetChargeThresholds"));
    QCOMPARE(reply.arguments().value(1).value<uchar>(), uchar(100));

    reply = call(vendor, kVendorPath, kVendorName, QStringLiteral("GetChargeProfiles"));
    QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
    QCOMPARE(reply.signature(), QStringLiteral("a(sssyy)"));
    const QDBusArgument profiles = reply.arguments().value(0).value<QDBusArgument>();
    QStringList ids;
    QList<uchar> ends;
    profiles.beginArray();
    while (!profiles.atEnd()) {
        QString id;
        QString title;
        QString description;
        uchar start = 0;
        uchar end = 0;
        profiles.beginStructure();
        profiles >> id >> title >> description >> start >> end;
        profiles.endStructure();
        QVERIFY(!title.isEmpty());
        QVERIFY(!description.isEmpty());
        // This machine has no start threshold, so the pair is (0, end).
        QCOMPARE(start, uchar(0));
        ids.append(id);
        ends.append(end);
    }
    profiles.endArray();
    QCOMPARE(ids, QStringList({ QStringLiteral("full"), QStringLiteral("balanced"),
                                QStringLiteral("max_lifespan") }));
    QCOMPARE(ends, QList<uchar>({ uchar(100), uchar(80), uchar(50) }));

    reply = call(vendor, kVendorPath, kVendorName, QStringLiteral("SetChargeThresholds"),
                 { QVariant::fromValue(testwire::Thresholds { 0, 80 }) });
    QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
    QCOMPARE(m_tree->read(node), QStringLiteral("80"));

    // The reported end threshold comes from the daemon's own reading of the
    // node, which it refreshes once per tick.
    m_daemon->tick();
    reply = call(vendor, kVendorPath, kVendorName, QStringLiteral("GetChargeThresholds"));
    QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
    QCOMPARE(reply.signature(), QStringLiteral("yy"));
    QCOMPARE(reply.arguments().value(0).value<uchar>(), uchar(0));
    QCOMPARE(reply.arguments().value(1).value<uchar>(), uchar(80));

    // A machine without the node is graceful degradation: the call is answered
    // and the refusal is logged, and the threshold on record does not move.
    m_tree->remove(node);
    reply = call(vendor, kVendorPath, kVendorName, QStringLiteral("SetChargeThresholds"),
                 { QVariant::fromValue(testwire::Thresholds { 0, 60 }) });
    QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
    QVERIFY(!QFile::exists(m_tree->root() + QLatin1Char('/') + node));
}

QTEST_MAIN(DBusServiceTest)
#include "dbus_service_test.moc"

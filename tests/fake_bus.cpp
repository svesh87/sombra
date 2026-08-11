// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "fake_bus.h"

#include "control_wire.h"

#include <QCoreApplication>
#include <QDBusConnectionInterface>
#include <QDBusMetaType>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QTest>
#include <QXmlStreamReader>

namespace testbus {

QDBusArgument &operator<<(QDBusArgument &argument, const Subject &subject)
{
    argument.beginStructure();
    argument << subject.kind << subject.details;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, Subject &subject)
{
    argument.beginStructure();
    argument >> subject.kind >> subject.details;
    argument.endStructure();
    return argument;
}

QDBusArgument &operator<<(QDBusArgument &argument, const Result &result)
{
    argument.beginStructure();
    argument << result.authorized << result.challenge << result.details;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, Result &result)
{
    argument.beginStructure();
    argument >> result.authorized >> result.challenge >> result.details;
    argument.endStructure();
    return argument;
}

} // namespace testbus

namespace {

constexpr const char *kAuthorityService = "org.freedesktop.PolicyKit1";
constexpr const char *kAuthorityPath = "/org/freedesktop/PolicyKit1/Authority";

} // namespace

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

PrivateBus::~PrivateBus()
{
    if (m_process.state() == QProcess::NotRunning)
        return;
    m_process.terminate();
    if (!m_process.waitForFinished(5000))
        m_process.kill();
}

bool PrivateBus::start(QString *error)
{
    const auto fail = [error](const QString &reason) {
        if (error)
            *error = reason;
        return false;
    };

    m_process.start(QStringLiteral("dbus-daemon"),
                    { QStringLiteral("--session"), QStringLiteral("--print-address"),
                      QStringLiteral("--nofork") });
    if (!m_process.waitForStarted(5000))
        return fail(QStringLiteral("dbus-daemon did not start"));
    if (!m_process.waitForReadyRead(5000))
        return fail(QStringLiteral("dbus-daemon printed no address"));
    m_address = QString::fromUtf8(m_process.readLine()).trimmed();
    if (m_address.isEmpty())
        return fail(QStringLiteral("dbus-daemon printed an empty address"));
    return true;
}

bool PrivateBus::startAt(const QString &socketPath, QString *error)
{
    const auto fail = [error](const QString &reason) {
        if (error)
            *error = reason;
        return false;
    };

    QFile::remove(socketPath);
    m_address = QStringLiteral("unix:path=") + socketPath;
    m_process.start(QStringLiteral("dbus-daemon"),
                    { QStringLiteral("--session"), QStringLiteral("--nofork"),
                      QStringLiteral("--address=") + m_address });
    if (!m_process.waitForStarted(5000))
        return fail(QStringLiteral("dbus-daemon did not start"));
    return true;
}

TestDaemon::TestDaemon(const QString &sysfsRoot, QString workDir, QString busAddress)
    : m_work(std::move(workDir))
    , m_address(std::move(busAddress))
{
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    // Otherwise a daemon started before its bus waits the production interval, and two
    // cases doing that spend ten seconds asleep between them.
    environment.insert(QStringLiteral("SOMBRA_BUS_RETRY_MS"), QStringLiteral("20"));
    m_process.setProcessEnvironment(environment);
    m_process.start(binary(),
                    { QStringLiteral("--no-dbus"), QStringLiteral("--root"), sysfsRoot,
                      QStringLiteral("--ring"), m_work + QStringLiteral("/metrics.ring"),
                      QStringLiteral("--bus-address"), m_address, QStringLiteral("--state"),
                      m_work + QStringLiteral("/state"), QStringLiteral("--config"),
                      m_work + QStringLiteral("/settings.conf"), QStringLiteral("--capacity"),
                      QStringLiteral("64"), QStringLiteral("--interval-ms"),
                      QStringLiteral("20") });
}

TestDaemon::~TestDaemon()
{
    stop();
}

bool TestDaemon::waitUntilAnswering(QString *error)
{
    const auto fail = [error](const QString &reason) {
        if (error)
            *error = reason;
        return false;
    };

    if (!m_process.waitForStarted(10000))
        return fail(QStringLiteral("sombrad did not start"));

    const QString name = QStringLiteral("wait-for-sombrad-")
            + QString::number(reinterpret_cast<quintptr>(this), 16);
    QDBusConnection connection = QDBusConnection::connectToBus(m_address, name);
    if (!connection.isConnected() || !connection.interface())
        return fail(QStringLiteral("the private bus is unreachable"));

    bool answering = false;
    QDeadlineTimer deadline(10000);
    while (!deadline.hasExpired()) {
        if (connection.interface()->isServiceRegistered(QString::fromLatin1(wire::kService))) {
            answering = true;
            break;
        }
        if (m_process.state() == QProcess::NotRunning)
            break;
        QTest::qWait(10);
    }
    connection = QDBusConnection(QString());
    QDBusConnection::disconnectFromBus(name);
    if (!answering) {
        return fail(QStringLiteral("sombrad never claimed %1: %2")
                            .arg(QString::fromLatin1(wire::kService),
                                 QString::fromUtf8(m_process.readAllStandardError())));
    }
    return true;
}

void TestDaemon::stop()
{
    if (m_process.state() == QProcess::NotRunning)
        return;
    m_process.terminate();
    m_process.waitForFinished(10000);
}

QString TestDaemon::binary()
{
    return QDir::cleanPath(QCoreApplication::applicationDirPath()
                           + QStringLiteral("/../sombrad"));
}

FakeAuthority::FakeAuthority(QObject *parent)
    : QObject(parent)
{
    qDBusRegisterMetaType<testbus::Subject>();
    qDBusRegisterMetaType<testbus::Result>();
    // Inside the answer, and unknown to D-Bus until somebody says so. Without this
    // the reply fails to marshal and the error points at the call rather than here.
    qDBusRegisterMetaType<QMap<QString, QString>>();
}

FakeAuthority::~FakeAuthority()
{
    if (m_connection.isConnected()) {
        m_connection.unregisterService(QString::fromLatin1(kAuthorityService));
        m_connection.unregisterObject(QString::fromLatin1(kAuthorityPath));
    }
    if (!m_connectionName.isEmpty()) {
        m_connection = QDBusConnection(QString());
        QDBusConnection::disconnectFromBus(m_connectionName);
    }
}

bool FakeAuthority::start(const QString &address, QString *error)
{
    const auto fail = [error](const QString &reason) {
        if (error)
            *error = reason;
        return false;
    };

    m_connectionName = QStringLiteral("fake-authority-")
            + QString::number(reinterpret_cast<quintptr>(this), 16);
    m_connection = QDBusConnection::connectToBus(address, m_connectionName);
    if (!m_connection.isConnected())
        return fail(QStringLiteral("the private bus is unreachable"));
    if (!m_connection.registerObject(QString::fromLatin1(kAuthorityPath), this,
                                     QDBusConnection::ExportAllSlots)) {
        return fail(QStringLiteral("cannot export the authority"));
    }
    if (!m_connection.registerService(QString::fromLatin1(kAuthorityService)))
        return fail(QStringLiteral("cannot claim the authority's name"));
    return true;
}

testbus::Result FakeAuthority::CheckAuthorization(const testbus::Subject &subject,
                                                  const QString &action,
                                                  const QMap<QString, QString> &details,
                                                  uint flags, const QString &cancellationId)
{
    Q_UNUSED(details);
    Q_UNUSED(cancellationId);

    m_actions.append(action);
    m_subjects.append(subject.details.value(QStringLiteral("name")).toString());
    // A dialog must never be asked for. Recorded rather than refused here, so that the
    // test asserting it says what went wrong instead of a command failing obscurely.
    if (flags != 0)
        m_interactionAsked = true;

    testbus::Result result;
    result.authorized = m_allowed;
    return result;
}

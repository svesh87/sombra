// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "bus_client.h"

#include "control_wire.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusReply>
#include <QDBusServiceWatcher>
#include <QDBusVariant>
#include <QStringList>

namespace {

constexpr const char *kPropertiesInterface = "org.freedesktop.DBus.Properties";
// Long enough for a method that waits for polkit, short enough that a console does
// not look hung if the daemon is wedged.
constexpr int kCallTimeoutMs = 5000;

// `GetAll` hands back a dictionary whose values are still in their wire form: a
// dictionary or an array inside it arrives as `QDBusArgument` rather than as the type
// it will be. Unpacked once, here, so that no reader has to know it.
QVariant unpacked(const QVariant &value)
{
    // A value inside a dictionary of variants arrives wrapped in one more layer than
    // it looks like it should: the variant itself. Unwrapped first, or a temperature
    // ends up as a variant nobody can read a number out of.
    if (value.metaType() == QMetaType::fromType<QDBusVariant>())
        return unpacked(value.value<QDBusVariant>().variant());
    if (value.metaType() != QMetaType::fromType<QDBusArgument>())
        return value;

    // Demarshalled by Qt rather than walked by hand — the shapes this interface uses
    // are all types Qt knows — and then once more through here per value, because a
    // dictionary inside a dictionary comes back in the same wire form again.
    const QDBusArgument argument = value.value<QDBusArgument>();
    switch (argument.currentType()) {
    case QDBusArgument::MapType: {
        QVariantMap map = qdbus_cast<QVariantMap>(argument);
        for (auto entry = map.begin(); entry != map.end(); ++entry)
            *entry = unpacked(*entry);
        return map;
    }
    case QDBusArgument::ArrayType: {
        // Two shapes of array travel on this interface, and they are not
        // interchangeable. A property that is a list of dictionaries — the modes, the
        // monitors — is `aa{sv}`. A list that sits *inside* a dictionary of variants is
        // `av`, because that is what a variant holding a list marshals as, and reading
        // one as the other yields nothing at all rather than an error.
        if (argument.currentSignature().startsWith(QLatin1String("aa{"))) {
            QList<QVariantMap> list = qdbus_cast<QList<QVariantMap>>(argument);
            for (QVariantMap &entry : list) {
                for (auto inner = entry.begin(); inner != entry.end(); ++inner)
                    *inner = unpacked(*inner);
            }
            return QVariant::fromValue(list);
        }
        QVariantList list = qdbus_cast<QVariantList>(argument);
        for (QVariant &entry : list)
            entry = unpacked(entry);
        return list;
    }
    case QDBusArgument::VariantType: {
        QDBusVariant inner;
        argument >> inner;
        return unpacked(inner.variant());
    }
    default:
        return value;
    }
}

} // namespace

class BusClient::Private {
public:
    QString connectionName;
    QDBusConnection connection { QString() };
    std::unique_ptr<QDBusServiceWatcher> watcher;
    bool available = false;
};

BusClient::BusClient(QString busAddress, QObject *parent)
    : QObject(parent)
    , d(std::make_unique<Private>())
{
    qDBusRegisterMetaType<QList<QVariantMap>>();

    d->connectionName = QStringLiteral("sombra-client-")
            + QString::number(reinterpret_cast<quintptr>(this), 16);
    d->connection = busAddress.isEmpty()
            ? QDBusConnection::connectToBus(QDBusConnection::SystemBus, d->connectionName)
            : QDBusConnection::connectToBus(busAddress, d->connectionName);

    const QString service = QString::fromLatin1(wire::kService);
    d->connection.connect(service, QString::fromLatin1(wire::kPath),
                          QString::fromLatin1(kPropertiesInterface),
                          QStringLiteral("PropertiesChanged"), this,
                          SLOT(onPropertiesChanged(QString, QVariantMap, QStringList)));

    // The name having an owner is what "the daemon is running" means here. A timer
    // asking would answer the same question later and less often.
    d->watcher = std::make_unique<QDBusServiceWatcher>(service, d->connection,
                                                       QDBusServiceWatcher::WatchForRegistration
                                                               | QDBusServiceWatcher::
                                                                       WatchForUnregistration);
    connect(d->watcher.get(), &QDBusServiceWatcher::serviceRegistered, this, [this]() {
        d->available = true;
        emit availabilityChanged(true);
    });
    connect(d->watcher.get(), &QDBusServiceWatcher::serviceUnregistered, this, [this]() {
        d->available = false;
        emit availabilityChanged(false);
    });

    if (d->connection.interface())
        d->available = d->connection.interface()->isServiceRegistered(service);
}

BusClient::~BusClient()
{
    // No signal from here. A destructor that announced "the daemon is gone" would
    // reach a handler that touches a window, and by then the window may be the thing
    // being destroyed.
    d->watcher.reset();
    d->connection = QDBusConnection(QString());
    QDBusConnection::disconnectFromBus(d->connectionName);
}

bool BusClient::available() const
{
    return d->available;
}

void BusClient::onPropertiesChanged(const QString &interfaceName, const QVariantMap &changed,
                                    const QStringList &invalidated)
{
    if (interfaceName != QLatin1String(wire::kInterface))
        return;
    // Nothing here is ever invalidated rather than sent: every property this daemon
    // has is cheap to read, so a change carries its new value.
    Q_UNUSED(invalidated);

    QVariantMap values;
    for (auto entry = changed.constBegin(); entry != changed.constEnd(); ++entry)
        values.insert(entry.key(), unpacked(entry.value()));
    emit propertiesChanged(values);
}

QVariantMap BusClient::readAll()
{
    QDBusMessage message = QDBusMessage::createMethodCall(
            QString::fromLatin1(wire::kService), QString::fromLatin1(wire::kPath),
            QString::fromLatin1(kPropertiesInterface), QStringLiteral("GetAll"));
    message.setArguments({ QString::fromLatin1(wire::kInterface) });
    const QDBusMessage reply = d->connection.call(message, QDBus::BlockWithGui, kCallTimeoutMs);
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty())
        return {};
    return unpacked(reply.arguments().constFirst()).toMap();
}

QString BusClient::call(const QString &method, const QVariantList &arguments)
{
    QDBusMessage message =
            QDBusMessage::createMethodCall(QString::fromLatin1(wire::kService),
                                          QString::fromLatin1(wire::kPath),
                                          QString::fromLatin1(wire::kInterface), method);
    message.setArguments(arguments);
    const QDBusMessage reply = d->connection.call(message, QDBus::BlockWithGui, kCallTimeoutMs);
    if (reply.type() == QDBusMessage::ErrorMessage) {
        const QString text = reply.errorMessage();
        return QStringLiteral("err ")
                + (text.isEmpty() ? QStringLiteral("the daemon is not answering") : text);
    }
    if (reply.type() != QDBusMessage::ReplyMessage)
        return QStringLiteral("err the daemon is not answering");

    const QString text = reply.arguments().isEmpty()
            ? QString()
            : reply.arguments().constFirst().toString();
    return text.isEmpty() ? QStringLiteral("ok") : QStringLiteral("ok ") + text;
}

#include "bus_client.moc"

// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "control_bus.h"

#include "control_wire.h"
#include "polkit_check.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusContext>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDebug>
#include <QTimer>

#include <functional>

namespace {

constexpr const char *kPropertiesInterface = "org.freedesktop.DBus.Properties";
constexpr const char *kPropertiesChanged = "PropertiesChanged";
// Two refusals worth telling apart: polkit said no, or the daemon did.
constexpr const char *kErrorRefused = "io.github.svesh87.Sombra.Error.Refused";
constexpr const char *kErrorFailed = "io.github.svesh87.Sombra.Error.Failed";
// Long enough not to churn while a machine boots without a bus, short enough that
// a bus appearing is noticed while somebody is still looking at the console.
constexpr int kDefaultRetryMs = 5000;
constexpr const char *kRetryOverride = "SOMBRA_BUS_RETRY_MS";

// The object on the bus.
//
// Registered directly rather than through a generated adaptor, and that is not a
// matter of taste: a method that waits for polkit has to answer later than it
// returns, which needs the message it is answering, and a generated adaptor cannot
// reach it. `QDBusContext` gives it, and it can only be inherited by the object
// that carries the slots. What keeps this in step with
// `dbus/io.github.svesh87.Sombra.xml` is a test that reads the introspection off a
// running daemon and compares the two.
//
// The property values are not held here. They live in one map the daemon's own state
// is rendered into, and every getter is a lookup in it: one place decides what a
// property means, and a getter cannot answer with something the last announcement
// did not carry.
class BusObject : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "io.github.svesh87.Sombra")

    Q_PROPERTY(QVariantMap Readings READ readings)
    Q_PROPERTY(QString Mode READ mode)
    Q_PROPERTY(QList<QVariantMap> Modes READ modes)
    Q_PROPERTY(bool LiveTuning READ liveTuning)
    Q_PROPERTY(QVariantMap Requested READ requested)
    Q_PROPERTY(QVariantMap Applied READ applied)
    Q_PROPERTY(int ChargeLimit READ chargeLimit)
    Q_PROPERTY(bool BrightnessAvailable READ brightnessAvailable)
    Q_PROPERTY(bool AutoBrightness READ autoBrightness)
    Q_PROPERTY(int BrightnessLevel READ brightnessLevel)
    Q_PROPERTY(int BrightnessBase READ brightnessBase)
    Q_PROPERTY(int BrightnessResponsePct READ brightnessResponsePct)
    Q_PROPERTY(int BrightnessFloorPct READ brightnessFloorPct)
    Q_PROPERTY(int BrightnessCeilingPct READ brightnessCeilingPct)
    Q_PROPERTY(int BrightnessWindowLow READ brightnessWindowLow)
    Q_PROPERTY(int BrightnessWindowHigh READ brightnessWindowHigh)
    Q_PROPERTY(QList<QVariantMap> Monitors READ monitors)
    Q_PROPERTY(bool MetricsEnabled READ metricsEnabled)
    Q_PROPERTY(int RetentionDays READ retentionDays)
    Q_PROPERTY(qulonglong SampleCount READ sampleCount)
    Q_PROPERTY(QVariantMap Configuration READ configuration)

public:
    // Runs one command through the daemon and answers with its reply line.
    using Handle = std::function<QString(const QString &command)>;

    BusObject(const QVariantMap *properties, Handle handle, QObject *parent = nullptr)
        : QObject(parent)
        , m_properties(properties)
        , m_handle(std::move(handle))
    {
    }

    QVariantMap readings() const { return value("Readings").toMap(); }
    QString mode() const { return value("Mode").toString(); }
    QList<QVariantMap> modes() const { return value("Modes").value<QList<QVariantMap>>(); }
    bool liveTuning() const { return value("LiveTuning").toBool(); }
    QVariantMap requested() const { return value("Requested").toMap(); }
    QVariantMap applied() const { return value("Applied").toMap(); }
    int chargeLimit() const { return value("ChargeLimit").toInt(); }
    bool brightnessAvailable() const { return value("BrightnessAvailable").toBool(); }
    bool autoBrightness() const { return value("AutoBrightness").toBool(); }
    int brightnessLevel() const { return value("BrightnessLevel").toInt(); }
    int brightnessBase() const { return value("BrightnessBase").toInt(); }
    int brightnessResponsePct() const { return value("BrightnessResponsePct").toInt(); }
    int brightnessFloorPct() const { return value("BrightnessFloorPct").toInt(); }
    int brightnessCeilingPct() const { return value("BrightnessCeilingPct").toInt(); }
    int brightnessWindowLow() const { return value("BrightnessWindowLow").toInt(); }
    int brightnessWindowHigh() const { return value("BrightnessWindowHigh").toInt(); }
    QList<QVariantMap> monitors() const { return value("Monitors").value<QList<QVariantMap>>(); }
    bool metricsEnabled() const { return value("MetricsEnabled").toBool(); }
    int retentionDays() const { return value("RetentionDays").toInt(); }
    qulonglong sampleCount() const { return value("SampleCount").toULongLong(); }
    QVariantMap configuration() const { return value("Configuration").toMap(); }

public slots:
    QString SetMode(const QString &id) { return act(QStringLiteral("SetMode"), { id }); }
    QString SetLive(const QVariantMap &assignments)
    {
        return act(QStringLiteral("SetLive"), { assignments });
    }
    QString ResetLive() { return act(QStringLiteral("ResetLive"), {}); }

    QString SetAutoBrightness(bool on) { return act(QStringLiteral("SetAutoBrightness"), { on }); }
    QString SetBrightnessLevel(int percent)
    {
        return act(QStringLiteral("SetBrightnessLevel"), { percent });
    }
    QString SetBrightnessBase(int percent)
    {
        return act(QStringLiteral("SetBrightnessBase"), { percent });
    }
    QString SetBrightnessSettings(const QVariantMap &settings)
    {
        return act(QStringLiteral("SetBrightnessSettings"), { settings });
    }

    QString SetMonitorLevel(const QString &id, int percent)
    {
        return act(QStringLiteral("SetMonitorLevel"), { id, percent });
    }

    QString Mark(const QString &verdict) { return act(QStringLiteral("Mark"), { verdict }); }
    QString SetChargeLimit(int percent)
    {
        return act(QStringLiteral("SetChargeLimit"), { percent });
    }

    QString Configure(const QString &kind, const QVariantMap &values)
    {
        return act(QStringLiteral("Configure"), { kind, values });
    }
    QString SaveLiveAs(const QString &id) { return act(QStringLiteral("SaveLiveAs"), { id }); }
    QString SetDefaultMode(const QString &id)
    {
        return act(QStringLiteral("SetDefaultMode"), { id });
    }
    QString ResetConfiguration(const QString &what)
    {
        return act(QStringLiteral("ResetConfiguration"), { what });
    }
    QString Reload() { return act(QStringLiteral("Reload"), {}); }

private:
    QVariant value(const char *name) const
    {
        return m_properties->value(QString::fromLatin1(name));
    }

    // Every method goes through here, and every one of them is answered later than
    // it returns. The reply is delayed because polkit is a service on this same bus:
    // asking it inside the handler and waiting would be this daemon waiting for
    // polkit while polkit waits for this daemon.
    QString act(const QString &method, const QVariantList &arguments)
    {
        const QDBusMessage request = message();
        const QDBusConnection bus = connection();
        setDelayedReply(true);

        polkit::check(bus, request.service(), QString::fromLatin1(wire::kAction), this,
                      [this, bus, request, method, arguments](bool allowed,
                                                              const QString &error) {
                          answer(bus, request, method, arguments, allowed, error);
                      });
        return {};
    }

    void answer(const QDBusConnection &bus, const QDBusMessage &request, const QString &method,
                const QVariantList &arguments, bool allowed, const QString &error)
    {
        if (!allowed) {
            // The message is polkit's own where there is one: "not authorized" and
            // "polkit did not answer" are different things to be told.
            bus.send(request.createErrorReply(
                    QString::fromLatin1(kErrorRefused),
                    error.isEmpty() ? QStringLiteral("not authorized to manage this machine")
                                    : error));
            return;
        }

        QString reason;
        const QString command = wire::commandFor(method, arguments, &reason);
        if (command.isEmpty()) {
            bus.send(request.createErrorReply(QString::fromLatin1(kErrorFailed), reason));
            return;
        }

        const QString reply = m_handle(command);
        if (reply.startsWith(QLatin1String("err"))) {
            bus.send(request.createErrorReply(QString::fromLatin1(kErrorFailed),
                                              reply.mid(3).trimmed()));
            return;
        }
        // The daemon's own sentence, without the word that framed it. A clamped value
        // arrives here, not as an error: it was applied, and the answer says at what.
        bus.send(request.createReply(reply.mid(2).trimmed()));
    }

    const QVariantMap *m_properties;
    Handle m_handle;
};

int retryInterval(int wanted)
{
    if (wanted > 0)
        return wanted;
    const QByteArray override = qgetenv(kRetryOverride);
    bool ok = false;
    const int parsed = override.toInt(&ok);
    return ok && parsed > 0 ? parsed : kDefaultRetryMs;
}

} // namespace

class ControlBus::Private {
public:
    Private(Daemon &daemon, ControlBus::Options options)
        : daemon(daemon)
        , options(std::move(options))
    {
    }

    Daemon &daemon;
    ControlBus::Options options;
    QDBusConnection connection { QString() };
    QString connectionName;
    QString serviceName;
    std::unique_ptr<BusObject> object;
    // What was last announced, and what every property getter answers from.
    QVariantMap published;
    QTimer retry;
    bool owned = false;
    bool exported = false;
    // So that a machine running without a bus does not write the same line into the
    // log every few seconds.
    bool complained = false;
};

ControlBus::ControlBus(Daemon &daemon, Options options, QObject *parent)
    : QObject(parent)
    , d(std::make_unique<Private>(daemon, std::move(options)))
{
    qDBusRegisterMetaType<QList<QVariantMap>>();
    polkit::registerTypes();
}

ControlBus::~ControlBus()
{
    if (d->connection.isConnected()) {
        if (d->owned)
            d->connection.unregisterService(d->serviceName);
        d->connection.unregisterObject(QString::fromLatin1(wire::kPath));
    }
    if (!d->connectionName.isEmpty()) {
        // The last reference goes before the bus is dropped, or Qt keeps the
        // connection alive and complains about it at exit.
        d->connection = QDBusConnection(QString());
        QDBusConnection::disconnectFromBus(d->connectionName);
    }
}

void ControlBus::reopen()
{
    if (d->connectionName.isEmpty()) {
        d->connectionName = QStringLiteral("sombra-control-")
                + QString::number(reinterpret_cast<quintptr>(this), 16);
    }
    // The reference goes first: dropping a bus that something still holds is a
    // warning from Qt and a connection that stays alive.
    d->connection = QDBusConnection(QString());
    QDBusConnection::disconnectFromBus(d->connectionName);
    d->exported = false;
    d->owned = false;

    // A connection of this daemon's own, even on the system bus, rather than Qt's
    // shared default. The compatibility names in `dbus_service.cpp` hold that one,
    // and this surface has to be able to drop and reopen its own without touching
    // theirs.
    d->connection = d->options.busAddress.isEmpty()
            ? QDBusConnection::connectToBus(QDBusConnection::SystemBus, d->connectionName)
            : QDBusConnection::connectToBus(d->options.busAddress, d->connectionName);
}

bool ControlBus::claim()
{
    if (d->owned)
        return true;
    if (!d->connection.isConnected())
        reopen();
    if (!d->connection.isConnected()) {
        if (!d->complained) {
            d->complained = true;
            qWarning().noquote() << "no bus to answer on yet:"
                                 << d->connection.lastError().message()
                                 << "- the machine is still being managed";
        }
        return false;
    }

    // The object first, so that nothing can find a name with nothing behind it.
    if (!d->exported) {
        if (!d->connection.registerObject(QString::fromLatin1(wire::kPath), d->object.get(),
                                          QDBusConnection::ExportAllSlots
                                                  | QDBusConnection::ExportAllProperties)) {
            qWarning().noquote() << "cannot export" << wire::kPath << ":"
                                 << d->connection.lastError().message();
            return false;
        }
        d->exported = true;
    }

    if (!d->connection.registerService(d->serviceName)) {
        if (!d->complained) {
            d->complained = true;
            // Without the bus policy file this is `AccessDenied`, which from a build
            // tree is the expected answer rather than a defect.
            qWarning().noquote() << "cannot claim" << d->serviceName << ":"
                                 << d->connection.lastError().message();
        }
        return false;
    }

    d->owned = true;
    d->complained = false;
    // Fills the map every property getter answers from, so the first client to ask
    // is not answered with nothing.
    publish();
    return true;
}

bool ControlBus::start(QString *error)
{
    d->serviceName = QString::fromLatin1(wire::kService) + d->options.nameSuffix;

    d->object = std::make_unique<BusObject>(&d->published, [this](const QString &command) {
        const QString reply = d->daemon.handleCommand(command);
        // Announced immediately rather than at the next tick: a console that has just
        // moved a slider should not watch it sit still for a second.
        publish();
        return reply;
    });
    reopen();

    // A name somebody else already holds means another daemon owns the controls, and
    // that is the one thing worth refusing to start over. Everything else — no bus
    // yet, no policy file — leaves the daemon running and the timer trying.
    if (d->connection.isConnected() && d->connection.interface()
        && d->connection.interface()->isServiceRegistered(d->serviceName)) {
        if (error)
            *error = QStringLiteral("another daemon is answering on %1").arg(d->serviceName);
        return false;
    }

    d->retry.setInterval(retryInterval(d->options.retryMs));
    connect(&d->retry, &QTimer::timeout, this, [this]() {
        if (claim())
            d->retry.stop();
    });
    connect(&d->daemon, &Daemon::ticked, this, &ControlBus::publish);

    if (!claim())
        d->retry.start();
    return true;
}

bool ControlBus::owned() const
{
    return d->owned;
}

QString ControlBus::name() const
{
    return d->serviceName;
}

void ControlBus::publish()
{
    const QVariantMap properties =
            wire::propertiesFrom(d->daemon.statusObject(), d->daemon.configurationObject());
    QVariantMap changed;
    for (auto entry = properties.constBegin(); entry != properties.constEnd(); ++entry) {
        const auto previous = d->published.constFind(entry.key());
        if (previous == d->published.constEnd() || *previous != entry.value())
            changed.insert(entry.key(), entry.value());
    }
    d->published = properties;

    // Only the keys that moved. A change to the depth of the ring has no business
    // arriving as part of a fan speed, and a client that merges what it is told
    // keeps everything it was not.
    if (!d->owned || changed.isEmpty())
        return;
    QDBusMessage signal = QDBusMessage::createSignal(QString::fromLatin1(wire::kPath),
                                                     QString::fromLatin1(kPropertiesInterface),
                                                     QString::fromLatin1(kPropertiesChanged));
    signal << QString::fromLatin1(wire::kInterface) << changed << QStringList();
    d->connection.send(signal);
}

#include "control_bus.moc"

// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "dbus_service.h"

#include "profile_bridge.h"

#include <QCoreApplication>
#include <QDBusAbstractAdaptor>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QVariantMap>

#include <algorithm>
#include <utility>

// The two structures the vendor interface moves over the wire. They exist
// because com.system76.PowerDaemon's signatures demand them; no other part of
// the project has a use for them.
namespace dbuscompat {

// One entry of GetChargeProfiles, wire type (sssyy). A start threshold is part
// of the signature; this machine exposes only an end threshold, so start is
// always reported as zero.
struct ChargeProfile {
    QString id;
    QString title;
    QString description;
    uchar start = 0;
    uchar end = 100;
};

// The single (yy) structure SetChargeThresholds takes.
struct ChargeThresholds {
    uchar start = 0;
    uchar end = 100;
};

QDBusArgument &operator<<(QDBusArgument &argument, const ChargeProfile &profile)
{
    argument.beginStructure();
    argument << profile.id << profile.title << profile.description << profile.start << profile.end;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, ChargeProfile &profile)
{
    argument.beginStructure();
    argument >> profile.id >> profile.title >> profile.description >> profile.start >> profile.end;
    argument.endStructure();
    return argument;
}

QDBusArgument &operator<<(QDBusArgument &argument, const ChargeThresholds &thresholds)
{
    argument.beginStructure();
    argument << thresholds.start << thresholds.end;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, ChargeThresholds &thresholds)
{
    argument.beginStructure();
    argument >> thresholds.start >> thresholds.end;
    argument.endStructure();
    return argument;
}

} // namespace dbuscompat

Q_DECLARE_METATYPE(dbuscompat::ChargeProfile)
Q_DECLARE_METATYPE(dbuscompat::ChargeThresholds)
Q_DECLARE_METATYPE(QList<dbuscompat::ChargeProfile>)

namespace {

// The interface name and the bus name coincide for all three, which is why
// there is one constant per pair rather than two.
constexpr char kHadessName[] = "net.hadess.PowerProfiles";
constexpr char kHadessPath[] = "/net/hadess/PowerProfiles";
constexpr char kUPowerName[] = "org.freedesktop.UPower.PowerProfiles";
constexpr char kUPowerPath[] = "/org/freedesktop/UPower/PowerProfiles";
constexpr char kVendorName[] = "com.system76.PowerDaemon";
constexpr char kVendorPath[] = "/com/system76/PowerDaemon";

constexpr char kPropertiesInterface[] = "org.freedesktop.DBus.Properties";

// The reason string PerformanceDegraded reports while the emergency thermal
// guard is clamping. The property exists to say why the machine is slower than
// the selected profile asks for, and the guard is the only thing here that
// makes it so.
constexpr char kDegradedByHeat[] = "high-operating-temperature";

// This machine has no discrete GPU, so the graphics half of the vendor
// interface has nothing to switch and answers with this.
constexpr char kIntegrated[] = "integrated";

QString systemProfileName(const QString &standardProfile)
{
    // The vendor vocabulary differs from the standard one in a single name.
    if (standardProfile == QLatin1String("power-saver"))
        return QStringLiteral("battery");
    if (standardProfile.isEmpty())
        return QStringLiteral("balanced");
    return standardProfile;
}

// Everything the exported objects need in order to answer, in one place so
// that the adaptors below stay a thin translation of wire types into daemon
// commands.
class Backend {
public:
    Backend(Daemon &daemon, DBusService::Options options)
        : daemon(daemon)
        , options(std::move(options))
        , connection(QString())
    {
    }

    // One outstanding HoldProfile. The interface promises that releasing the
    // last hold returns to whatever was selected before the first one, which
    // is the only reason the previous mode is remembered.
    struct Hold {
        uint cookie = 0;
        QString profile;
        QString reason;
        QString applicationId;
    };

    QJsonObject status() const
    {
        const QString reply = daemon.handleCommand(QStringLiteral("status"));
        if (!reply.startsWith(QLatin1String("ok ")))
            return {};
        return QJsonDocument::fromJson(reply.mid(3).toUtf8()).object();
    }

    QString activeProfile() const
    {
        return profile_bridge::profileForMode(daemon.modes(), daemon.activeModeId());
    }

    bool isKnownProfile(const QString &profile) const
    {
        return !profile_bridge::modeForProfile(daemon.modes(), profile).isEmpty();
    }

    void selectProfile(const QString &profile)
    {
        const QString mode = profile_bridge::modeForProfile(daemon.modes(), profile);
        if (mode.isEmpty()) {
            qWarning().noquote() << "D-Bus: no mode answers to the profile" << profile;
            return;
        }
        const QString reply = daemon.handleCommand(QStringLiteral("mode ") + mode);
        if (!reply.startsWith(QLatin1String("ok")))
            qWarning().noquote() << "D-Bus: mode" << mode << "refused:" << reply;
    }

    QList<QVariantMap> profiles() const
    {
        QList<QVariantMap> list;
        const QStringList names = profile_bridge::standardProfiles();
        for (const QString &name : names) {
            QVariantMap entry;
            entry.insert(QStringLiteral("Profile"), name);
            // Driver is the older aggregate field; the two below name the
            // kernel surfaces that actually carry the setpoints here.
            entry.insert(QStringLiteral("Driver"), QStringLiteral("multiple"));
            entry.insert(QStringLiteral("CpuDriver"), QStringLiteral("intel_pstate"));
            entry.insert(QStringLiteral("PlatformDriver"), QStringLiteral("platform_profile"));
            list.append(entry);
        }
        return list;
    }

    QList<QVariantMap> holdEntries() const
    {
        QList<QVariantMap> list;
        for (const Hold &hold : holds) {
            QVariantMap entry;
            entry.insert(QStringLiteral("Profile"), hold.profile);
            entry.insert(QStringLiteral("Reason"), hold.reason);
            entry.insert(QStringLiteral("ApplicationId"), hold.applicationId);
            list.append(entry);
        }
        return list;
    }

    QString degradedReason() const
    {
        return status().value(QStringLiteral("guard_engaged")).toBool()
                ? QString::fromLatin1(kDegradedByHeat)
                : QString();
    }

    // Percent. Read from the daemon's status rather than kept here: the daemon
    // reads the node once per tick and one copy of that number is enough.
    uchar chargeEnd() const
    {
        const QJsonValue limit = status().value(QStringLiteral("charge_limit"));
        return limit.isDouble() ? static_cast<uchar>(limit.toInt()) : uchar(100);
    }

    void setChargeEnd(uchar percent)
    {
        const QString reply = daemon.handleCommand(QStringLiteral("charge %1").arg(int(percent)));
        if (!reply.startsWith(QLatin1String("ok")))
            qWarning().noquote() << "D-Bus: charge threshold refused:" << reply;
    }

    uint addHold(const QString &profile, const QString &reason, const QString &applicationId)
    {
        if (holds.isEmpty())
            modeBeforeHold = daemon.activeModeId();
        const Hold hold { nextCookie++, profile, reason, applicationId };
        holds.append(hold);
        selectProfile(profile);
        return hold.cookie;
    }

    bool releaseHold(uint cookie)
    {
        const auto it = std::find_if(holds.begin(), holds.end(),
                                     [cookie](const Hold &hold) { return hold.cookie == cookie; });
        if (it == holds.end())
            return false;
        holds.erase(it);

        if (!holds.isEmpty())
            selectProfile(holds.last().profile);
        else if (!modeBeforeHold.isEmpty())
            daemon.handleCommand(QStringLiteral("mode ") + std::exchange(modeBeforeHold, {}));

        announceRelease(cookie);
        return true;
    }

    void send(const char *path, const char *interface, const QString &name,
              const QVariantList &arguments)
    {
        QDBusMessage message = QDBusMessage::createSignal(QString::fromLatin1(path),
                                                         QString::fromLatin1(interface), name);
        message.setArguments(arguments);
        connection.send(message);
    }

    // Both standard objects carry the same state, so both announce. Qt does
    // not emit PropertiesChanged for adaptor properties by itself.
    void announceProfile()
    {
        QVariantMap changed;
        changed.insert(QStringLiteral("ActiveProfile"), activeProfile());
        send(kHadessPath, kPropertiesInterface, QStringLiteral("PropertiesChanged"),
             { QString::fromLatin1(kHadessName), QVariant::fromValue(changed), QStringList() });
        send(kUPowerPath, kPropertiesInterface, QStringLiteral("PropertiesChanged"),
             { QString::fromLatin1(kUPowerName), QVariant::fromValue(changed), QStringList() });
        send(kVendorPath, kVendorName, QStringLiteral("PowerProfileSwitch"),
             { systemProfileName(activeProfile()) });
    }

    // Refusing a call takes the message itself: the reply Qt would send on
    // return has to be suppressed first, or the caller gets two.
    void refuse(const QDBusMessage &message, const QString &reason)
    {
        message.setDelayedReply(true);
        connection.send(message.createErrorReply(QDBusError::InvalidArgs, reason));
    }

    void announceRelease(uint cookie)
    {
        const QVariantList arguments { QVariant::fromValue(cookie) };
        send(kHadessPath, kHadessName, QStringLiteral("ProfileReleased"), arguments);
        send(kUPowerPath, kUPowerName, QStringLiteral("ProfileReleased"), arguments);
    }

    Daemon &daemon;
    DBusService::Options options;
    QDBusConnection connection;
    // Non-empty only for a private bus, which is the case that has to be
    // disconnected again.
    QString privateConnectionName;
    QStringList claimed;
    // One exported object per path: a signal relayed from a single object
    // registered at two paths would go out twice.
    std::unique_ptr<QObject> hadessObject;
    std::unique_ptr<QObject> upowerObject;
    std::unique_ptr<QObject> vendorObject;
    QList<Hold> holds;
    uint nextCookie = 1;
    QString modeBeforeHold;
};

// The standard power-profiles interface.
//
// Two bus names carry an identical signature set at two object paths, so the
// behaviour lives here once and the two leaves below differ only in the
// interface name D-Bus reads from their class info — which has to sit on the
// most derived class for Qt to find it.
//
// The signal is declared for introspection and sent as a bus message rather
// than emitted, because the same state is exported at two paths and each path
// announces for itself. The trailing QDBusMessage on the two methods is Qt's
// way of handing a slot the call it is answering; it is not part of the D-Bus
// signature.
class PowerProfilesAdaptor : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_PROPERTY(QString ActiveProfile READ activeProfile WRITE setActiveProfile)
    Q_PROPERTY(QList<QVariantMap> Profiles READ profiles)
    Q_PROPERTY(QStringList Actions READ actions)
    Q_PROPERTY(QList<QVariantMap> ActiveProfileHolds READ activeProfileHolds)
    Q_PROPERTY(QString PerformanceInhibited READ performanceInhibited)
    Q_PROPERTY(QString PerformanceDegraded READ performanceDegraded)
    Q_PROPERTY(QString Version READ version)

public:
    PowerProfilesAdaptor(QObject *parent, Backend *backend)
        : QDBusAbstractAdaptor(parent)
        , m_backend(backend)
    {
    }

    QString activeProfile() const { return m_backend->activeProfile(); }
    void setActiveProfile(const QString &profile) { m_backend->selectProfile(profile); }
    QList<QVariantMap> profiles() const { return m_backend->profiles(); }
    // No profile here carries a side action such as a panel power switch.
    QStringList actions() const { return {}; }
    QList<QVariantMap> activeProfileHolds() const { return m_backend->holdEntries(); }
    // Nothing on this platform inhibits the performance profile outright.
    QString performanceInhibited() const { return {}; }
    QString performanceDegraded() const { return m_backend->degradedReason(); }
    // The daemon's own version, which is what a client asking this is after.
    QString version() const { return QCoreApplication::applicationVersion(); }

public slots:
    uint HoldProfile(const QString &profile, const QString &reason,
                     const QString &application_id, const QDBusMessage &message)
    {
        if (!m_backend->isKnownProfile(profile)) {
            m_backend->refuse(message, QStringLiteral("unknown profile %1").arg(profile));
            return 0;
        }
        return m_backend->addHold(profile, reason, application_id);
    }

    void ReleaseProfile(uint cookie, const QDBusMessage &message)
    {
        if (!m_backend->releaseHold(cookie))
            m_backend->refuse(message, QStringLiteral("no hold with cookie %1").arg(cookie));
    }

signals:
    void ProfileReleased(uint cookie);

private:
    Backend *m_backend;
};

class HadessProfilesAdaptor : public PowerProfilesAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "net.hadess.PowerProfiles")

public:
    using PowerProfilesAdaptor::PowerProfilesAdaptor;
};

class UPowerProfilesAdaptor : public PowerProfilesAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.UPower.PowerProfiles")

public:
    using PowerProfilesAdaptor::PowerProfilesAdaptor;
};

// The vendor interface the COSMIC panel applet talks to.
//
// The signature set is reproduced exactly as the vendor daemon introspects,
// because the applet binds to it. The graphics half answers with constants:
// this machine has no discrete GPU, so there is nothing to switch, nothing to
// power down and no hot-plug event to report. Nothing here touches display
// brightness — the vendor daemon changes the backlight on a profile switch,
// which this project treats as a defect rather than behaviour to match.
class PowerDaemonAdaptor : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "com.system76.PowerDaemon")

public:
    PowerDaemonAdaptor(QObject *parent, Backend *backend)
        : QDBusAbstractAdaptor(parent)
        , m_backend(backend)
    {
    }

public slots:
    void AutoGraphicsPower() { logGraphicsRequest(QStringLiteral("AutoGraphicsPower")); }
    void Battery() { m_backend->selectProfile(QStringLiteral("power-saver")); }
    void Balanced() { m_backend->selectProfile(QStringLiteral("balanced")); }
    void Performance() { m_backend->selectProfile(QStringLiteral("performance")); }

    QString GetProfile() const { return systemProfileName(m_backend->activeProfile()); }

    QList<dbuscompat::ChargeProfile> GetChargeProfiles() const
    {
        // The vendor's three named thresholds. Only the end threshold is
        // actuated, so the start of each is reported as zero.
        return { { QStringLiteral("full"), QStringLiteral("Full Charge"),
                   QStringLiteral("Battery is charged to its full capacity"), 0, 100 },
                 { QStringLiteral("balanced"), QStringLiteral("Balanced"),
                   QStringLiteral("Increases battery lifespan"), 0, 80 },
                 { QStringLiteral("max_lifespan"), QStringLiteral("Maximum Lifespan"),
                   QStringLiteral("Increases battery lifespan to its maximum"), 0, 50 } };
    }

    void GetChargeThresholds(uchar &start, uchar &end) const
    {
        start = 0;
        end = m_backend->chargeEnd();
    }

    void SetChargeThresholds(const dbuscompat::ChargeThresholds &thresholds)
    {
        m_backend->setChargeEnd(thresholds.end);
    }

    QString GetGraphics() const { return QString::fromLatin1(kIntegrated); }
    QString GetDefaultGraphics() const { return QString::fromLatin1(kIntegrated); }
    bool GetSwitchable() const { return false; }
    bool GetGraphicsPower() const { return false; }
    bool GetDesktop() const { return false; }
    bool GetExternalDisplaysRequireDgpu() const { return false; }

    void SetGraphics(const QString &vendor)
    {
        logGraphicsRequest(QStringLiteral("SetGraphics %1").arg(vendor));
    }

    void SetGraphicsPower(bool power)
    {
        logGraphicsRequest(QStringLiteral("SetGraphicsPower %1").arg(power ? 1 : 0));
    }

signals:
    // Declared because the applet binds to the interface as a whole. Never
    // sent: there is no second GPU to plug a display into.
    void HotPlugDetect(qulonglong port);
    void PowerProfileSwitch(const QString &profile);

private:
    static void logGraphicsRequest(const QString &request)
    {
        qInfo().noquote() << "D-Bus: ignoring" << request << "- this machine has one GPU";
    }

    Backend *m_backend;
};

} // namespace

class DBusService::Private : public Backend {
public:
    using Backend::Backend;
};

DBusService::DBusService(Daemon &daemon, Options options, QObject *parent)
    : QObject(parent)
    , d(std::make_unique<Private>(daemon, std::move(options)))
{
    qDBusRegisterMetaType<QList<QVariantMap>>();
    qDBusRegisterMetaType<dbuscompat::ChargeProfile>();
    qDBusRegisterMetaType<QList<dbuscompat::ChargeProfile>>();
    qDBusRegisterMetaType<dbuscompat::ChargeThresholds>();
}

DBusService::~DBusService()
{
    if (d->connection.isConnected()) {
        for (const QString &name : std::as_const(d->claimed))
            d->connection.unregisterService(name);
        for (const char *path : { kHadessPath, kUPowerPath, kVendorPath })
            d->connection.unregisterObject(QString::fromLatin1(path));
    }
    if (!d->privateConnectionName.isEmpty()) {
        // The last reference has to go before the bus is dropped, or Qt keeps
        // the connection alive and complains about it at exit.
        d->connection = QDBusConnection(QString());
        QDBusConnection::disconnectFromBus(d->privateConnectionName);
    }
}

bool DBusService::start(QString *error)
{
    const auto fail = [error](const QString &reason) {
        if (error)
            *error = reason;
        return false;
    };

    if (d->options.busAddress.isEmpty()) {
        d->connection = QDBusConnection::systemBus();
    } else {
        d->privateConnectionName = QStringLiteral("cpu-throttler-")
                + QString::number(reinterpret_cast<quintptr>(this), 16);
        d->connection =
                QDBusConnection::connectToBus(d->options.busAddress, d->privateConnectionName);
    }
    if (!d->connection.isConnected()) {
        const QString message = d->connection.lastError().message();
        return fail(message.isEmpty() ? QStringLiteral("the bus is unreachable") : message);
    }

    d->hadessObject = std::make_unique<QObject>();
    new HadessProfilesAdaptor(d->hadessObject.get(), d.get());
    d->upowerObject = std::make_unique<QObject>();
    new UPowerProfilesAdaptor(d->upowerObject.get(), d.get());
    d->vendorObject = std::make_unique<QObject>();
    new PowerDaemonAdaptor(d->vendorObject.get(), d.get());

    // Objects first, so that nothing can find a name with nothing behind it.
    const std::pair<const char *, QObject *> objects[] = {
        { kHadessPath, d->hadessObject.get() },
        { kUPowerPath, d->upowerObject.get() },
        { kVendorPath, d->vendorObject.get() },
    };
    for (const auto &object : objects) {
        if (!d->connection.registerObject(QString::fromLatin1(object.first), object.second,
                                          QDBusConnection::ExportAdaptors))
            return fail(QStringLiteral("cannot export %1").arg(QLatin1String(object.first)));
    }

    // The suffix is appended verbatim, separator included, so a test asks for
    // ".t1" and gets net.hadess.PowerProfiles.t1. Production leaves it empty.
    for (const char *name : { kHadessName, kUPowerName, kVendorName }) {
        const QString full = QString::fromLatin1(name) + d->options.nameSuffix;
        if (d->connection.registerService(full))
            d->claimed.append(full);
        else
            qWarning().noquote() << "D-Bus name already held, leaving it alone:" << full;
    }
    // Holding some of the names is still useful: whichever panel binds to a
    // claimed name keeps working, and the control socket is unaffected either
    // way. Holding none of them leaves nothing to answer on.
    if (d->claimed.isEmpty())
        return fail(QStringLiteral("every name is already held by another process"));

    connect(&d->daemon, &Daemon::modeChanged, this, [this] { d->announceProfile(); });
    return true;
}

QStringList DBusService::claimedNames() const
{
    return d->claimed;
}

QString DBusService::activeProfile() const
{
    return d->activeProfile();
}

#include "dbus_service.moc"

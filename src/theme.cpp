// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "theme.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIcon>
#include <QSettings>
#include <QStandardPaths>
#include <QVariant>

namespace {

constexpr const char *kPortalService = "org.freedesktop.portal.Desktop";
constexpr const char *kPortalPath = "/org/freedesktop/portal/desktop";
constexpr const char *kPortalInterface = "org.freedesktop.portal.Settings";
constexpr const char *kNamespace = "org.freedesktop.appearance";
constexpr const char *kKey = "color-scheme";
// The icon theme has no home in the appearance namespace, and every desktop
// that implements the portal answers for this one instead.
constexpr const char *kIconNamespace = "org.gnome.desktop.interface";
constexpr const char *kIconKey = "icon-theme";

// The portal is another process, and one that may be starting up alongside us.
// A second is long for a colour and short for a window that is already visible.
constexpr int kPortalTimeoutMs = 1000;

// A setting arrives as a variant inside a variant: the portal's own wrapper
// around a value whose type it does not know.
QVariant readPortalSetting(const char *space, const char *key)
{
    if (!QDBusConnection::sessionBus().isConnected())
        return {};

    QDBusInterface portal(QString::fromLatin1(kPortalService), QString::fromLatin1(kPortalPath),
                          QString::fromLatin1(kPortalInterface), QDBusConnection::sessionBus());
    portal.setTimeout(kPortalTimeoutMs);
    const QDBusReply<QDBusVariant> reply = portal.call(
            QStringLiteral("Read"), QString::fromLatin1(space), QString::fromLatin1(key));
    if (!reply.isValid())
        return {};

    QVariant value = reply.value().variant();
    while (value.canConvert<QDBusVariant>())
        value = value.value<QDBusVariant>().variant();
    return value;
}

// The portal answers 0 for "no preference", 1 for dark and 2 for light.
theme::Scheme fromPortalValue(uint value)
{
    switch (value) {
    case 1:
        return theme::Scheme::Dark;
    case 2:
        return theme::Scheme::Light;
    default:
        return theme::Scheme::Unknown;
    }
}

// The console's own settings, beside the autostart entry rather than in the
// daemon's file: what the window looks like is the window's business.
QString settingsPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
            + QStringLiteral("/ombra/console.conf");
}

} // namespace

namespace theme {

// The portal announces every setting it holds, so the namespace and the key are
// checked here rather than in a match rule: a rule narrow enough to do it would
// have to encode argument positions, which is the kind of thing that breaks
// silently when the portal adds a setting.
class PortalWatcher : public QObject {
    Q_OBJECT

public:
    PortalWatcher(QObject *parent, std::function<void()> onChange)
        : QObject(parent)
        , m_onChange(std::move(onChange))
    {
        QDBusConnection::sessionBus().connect(
                QString::fromLatin1(kPortalService), QString::fromLatin1(kPortalPath),
                QString::fromLatin1(kPortalInterface), QStringLiteral("SettingChanged"), this,
                SLOT(settingChanged(QString, QString, QDBusVariant)));
    }

public slots:
    void settingChanged(const QString &space, const QString &key, const QDBusVariant &)
    {
        const bool scheme = space == QLatin1String(kNamespace) && key == QLatin1String(kKey);
        const bool icons =
                space == QLatin1String(kIconNamespace) && key == QLatin1String(kIconKey);
        if (!scheme && !icons)
            return;
        // The value carried by the signal is deliberately ignored and the
        // portal asked again: one path into the answer means one place where a
        // surprising value is handled.
        m_onChange();
    }

private:
    std::function<void()> m_onChange;
};

Scheme desktopScheme()
{
    const QVariant value = readPortalSetting(kNamespace, kKey);
    bool ok = false;
    const uint scheme = value.toUInt(&ok);
    return ok ? fromPortalValue(scheme) : Scheme::Unknown;
}

QString desktopIconTheme()
{
    const QVariant value = readPortalSetting(kIconNamespace, kIconKey);
    return value.typeId() == QMetaType::QString ? value.toString() : QString();
}

Scheme schemeOf(const QPalette &palette)
{
    // Perceived luminance rather than the plain average: a mid-grey window with
    // a green tint is not lighter than one with a blue tint by the amount the
    // average would claim.
    const QColor window = palette.color(QPalette::Window);
    const double luminance =
            0.2126 * window.redF() + 0.7152 * window.greenF() + 0.0722 * window.blueF();
    return luminance < 0.5 ? Scheme::Dark : Scheme::Light;
}

QPalette paletteFor(Scheme scheme)
{
    QPalette palette;
    if (scheme == Scheme::Dark) {
        const QColor window(0x2b, 0x2b, 0x2b);
        const QColor base(0x1e, 0x1e, 0x1e);
        const QColor text(0xee, 0xee, 0xec);
        const QColor disabled(0x88, 0x88, 0x88);
        palette.setColor(QPalette::Window, window);
        palette.setColor(QPalette::WindowText, text);
        palette.setColor(QPalette::Base, base);
        palette.setColor(QPalette::AlternateBase, window);
        palette.setColor(QPalette::Text, text);
        palette.setColor(QPalette::Button, window);
        palette.setColor(QPalette::ButtonText, text);
        palette.setColor(QPalette::ToolTipBase, base);
        palette.setColor(QPalette::ToolTipText, text);
        palette.setColor(QPalette::Highlight, QColor(0x3d, 0x6f, 0xa8));
        palette.setColor(QPalette::HighlightedText, text);
        palette.setColor(QPalette::Disabled, QPalette::WindowText, disabled);
        palette.setColor(QPalette::Disabled, QPalette::Text, disabled);
        palette.setColor(QPalette::Disabled, QPalette::ButtonText, disabled);
        return palette;
    }

    const QColor window(0xf2, 0xf1, 0xf0);
    const QColor base(0xff, 0xff, 0xff);
    const QColor text(0x2e, 0x34, 0x36);
    const QColor disabled(0x9a, 0x9a, 0x9a);
    palette.setColor(QPalette::Window, window);
    palette.setColor(QPalette::WindowText, text);
    palette.setColor(QPalette::Base, base);
    palette.setColor(QPalette::AlternateBase, window);
    palette.setColor(QPalette::Text, text);
    palette.setColor(QPalette::Button, window);
    palette.setColor(QPalette::ButtonText, text);
    palette.setColor(QPalette::ToolTipBase, base);
    palette.setColor(QPalette::ToolTipText, text);
    palette.setColor(QPalette::Highlight, QColor(0x35, 0x84, 0xe4));
    palette.setColor(QPalette::HighlightedText, base);
    palette.setColor(QPalette::Disabled, QPalette::WindowText, disabled);
    palette.setColor(QPalette::Disabled, QPalette::Text, disabled);
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, disabled);
    return palette;
}

bool applyScheme(Scheme scheme)
{
    if (scheme == Scheme::Unknown)
        return false;
    if (schemeOf(QGuiApplication::palette()) == scheme)
        return false;
    QGuiApplication::setPalette(paletteFor(scheme));
    return true;
}

bool applyDesktopScheme()
{
    return applyScheme(desktopScheme());
}

QString nameOf(Preference wanted)
{
    switch (wanted) {
    case Preference::Light:
        return QStringLiteral("light");
    case Preference::Dark:
        return QStringLiteral("dark");
    case Preference::System:
        break;
    }
    return QStringLiteral("system");
}

Preference preferenceFrom(const QString &name)
{
    const QString value = name.trimmed().toLower();
    if (value == QLatin1String("light"))
        return Preference::Light;
    if (value == QLatin1String("dark"))
        return Preference::Dark;
    // Anything else, including a file written by hand and mistyped, is the
    // default: following the desktop is the answer that surprises nobody.
    return Preference::System;
}

Preference preference()
{
    QSettings settings(settingsPath(), QSettings::IniFormat);
    return preferenceFrom(settings.value(QStringLiteral("appearance")).toString());
}

void setPreference(Preference wanted)
{
    QDir().mkpath(QFileInfo(settingsPath()).absolutePath());
    QSettings settings(settingsPath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("appearance"), nameOf(wanted));
}

bool applyPreference()
{
    switch (preference()) {
    case Preference::Light:
        return applyScheme(Scheme::Light);
    case Preference::Dark:
        return applyScheme(Scheme::Dark);
    case Preference::System:
        break;
    }
    return applyDesktopScheme();
}

bool applyDesktopIconTheme()
{
    const QString wanted = desktopIconTheme();
    if (wanted.isEmpty() || QIcon::themeName() == wanted)
        return false;
    QIcon::setThemeName(wanted);
    return true;
}

void watch(QObject *context, std::function<void()> onChange)
{
    if (!QDBusConnection::sessionBus().isConnected())
        return;
    // Owned by the context, so the subscription dies with whoever asked for it.
    new PortalWatcher(context, std::move(onChange));
}

} // namespace theme

#include "theme.moc"

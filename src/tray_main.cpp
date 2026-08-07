// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "tray_client.h"
#include "tray_window.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCommandLineParser>
#include <QDebug>
#include <QIcon>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QSystemTrayIcon>
#include <QTimer>

namespace {

// Three states, because those are the three a glance at the panel has to
// distinguish: no daemon, running, and the emergency guard clamping.
QIcon trayIcon(bool connected, bool guardEngaged)
{
    QPixmap pixmap(64, 64);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    const QColor colour = !connected ? QColor(140, 140, 140)
                        : guardEngaged ? QColor(220, 70, 55)
                                       : QColor(90, 175, 230);
    painter.setPen(QPen(colour, 5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.drawRoundedRect(QRectF(18, 18, 28, 28), 4, 4);
    for (const int offset : { 25, 32, 39 }) {
        painter.drawLine(QPointF(offset, 8), QPointF(offset, 18));
        painter.drawLine(QPointF(offset, 46), QPointF(offset, 56));
        painter.drawLine(QPointF(8, offset), QPointF(18, offset));
        painter.drawLine(QPointF(46, offset), QPointF(56, offset));
    }
    if (!connected)
        painter.drawLine(QPointF(14, 14), QPointF(50, 50));
    return QIcon(pixmap);
}

QString summaryText(const TrayClient::Status &status)
{
    const QString mode = status.modeLabels.value(status.modeIds.indexOf(status.modeId),
                                                 status.modeId);
    if (status.packageC <= TrayClient::kNoReading)
        return mode;
    return QStringLiteral("%1 — %2 °C, %3 RPM")
            .arg(mode, QString::number(status.packageC, 'f', 1),
                 status.fanRpm <= TrayClient::kNoReading
                         ? QStringLiteral("—")
                         : QString::number(status.fanRpm, 'f', 0));
}

// The mode list belongs to the daemon's configuration file, so the menu is
// built from a status reply rather than compiled in here.
void rebuildModeActions(QMenu &menu, QAction *before, QActionGroup &group, TrayClient &client,
                        const TrayClient::Status &status)
{
    qDeleteAll(group.actions());
    for (int i = 0; i < status.modeIds.size(); ++i) {
        const QString id = status.modeIds.at(i);
        QAction *action = new QAction(status.modeLabels.value(i, id), &group);
        action->setCheckable(true);
        action->setData(id);
        QObject::connect(action, &QAction::triggered, &client,
                         [&client, id]() { client.sendAndRefresh(QStringLiteral("mode ") + id); });
        menu.insertAction(before, action);
    }
}

} // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("ombra-tray"));
    QApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    QApplication::setQuitOnLastWindowClosed(false);

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
            "Experimentation console for ombrad. Sliders apply immediately and are never "
            "written back; durable mode definitions live in the daemon's configuration file."));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption socketOption(
            QStringLiteral("socket"), QStringLiteral("Control socket."), QStringLiteral("path"),
            QStringLiteral("/run/ombra/control.sock"));
    const QCommandLineOption intervalOption(
            QStringLiteral("interval-ms"),
            QStringLiteral("How often to ask the daemon for readings."), QStringLiteral("ms"),
            QStringLiteral("1000"));
    // A test hook: it is what lets the suite run this binary against a test
    // daemon and still have it exit.
    const QCommandLineOption quitAfterOption(
            QStringLiteral("quit-after-ms"),
            QStringLiteral("Exit after this many milliseconds instead of running until quit."),
            QStringLiteral("ms"), QStringLiteral("0"));
    parser.addOptions({ socketOption, intervalOption, quitAfterOption });
    parser.process(app);

    TrayClient client(parser.value(socketOption));
    TrayWindow window(client);

    QSystemTrayIcon tray;
    QMenu menu;
    QAction *summary = menu.addAction(QStringLiteral("Waiting for the daemon"));
    summary->setEnabled(false);
    menu.addSeparator();
    // Mode entries are inserted in front of this separator as they arrive.
    QAction *modeAnchor = menu.addSeparator();
    QAction *consoleAction = menu.addAction(QStringLiteral("Console"));
    QAction *quitAction = menu.addAction(QStringLiteral("Quit"));
    QActionGroup modeGroup(&menu);
    modeGroup.setExclusive(true);

    tray.setToolTip(QStringLiteral("Ombra"));
    tray.setIcon(trayIcon(false, false));
    tray.setContextMenu(&menu);
    tray.show();

    const auto toggleConsole = [&window]() {
        if (window.isVisible()) {
            window.hide();
            return;
        }
        window.show();
        window.raise();
        window.activateWindow();
    };
    QObject::connect(consoleAction, &QAction::triggered, &window, toggleConsole);
    QObject::connect(&tray, &QSystemTrayIcon::activated, &window,
                     [toggleConsole](QSystemTrayIcon::ActivationReason reason) {
                         if (reason == QSystemTrayIcon::Trigger)
                             toggleConsole();
                     });
    QObject::connect(quitAction, &QAction::triggered, &app, &QApplication::quit);

    QStringList shownModes;
    QObject::connect(&client, &TrayClient::statusChanged, &app,
                     [&](const TrayClient::Status &status) {
                         if (shownModes != status.modeIds) {
                             shownModes = status.modeIds;
                             rebuildModeActions(menu, modeAnchor, modeGroup, client, status);
                         }
                         for (QAction *action : modeGroup.actions())
                             action->setChecked(action->data().toString() == status.modeId);
                         summary->setText(summaryText(status));
                         tray.setIcon(trayIcon(true, status.guardEngaged));
                     });
    QObject::connect(&client, &TrayClient::connectionChanged, &app, [&](bool connected) {
        if (!connected) {
            summary->setText(QStringLiteral("The daemon is not answering"));
            tray.setIcon(trayIcon(false, false));
        }
    });

    // Without a system tray the window is the only way in, so it opens
    // straight away instead of hiding behind an icon that does not exist.
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        qWarning() << "no system tray available; showing the console instead";
        toggleConsole();
    }

    const int quitAfterMs = parser.value(quitAfterOption).toInt();
    if (quitAfterMs > 0)
        QTimer::singleShot(quitAfterMs, &app, &QApplication::quit);

    const int intervalMs = parser.value(intervalOption).toInt();
    if (intervalMs > 0)
        client.start(intervalMs);
    else
        client.start();
    return app.exec();
}

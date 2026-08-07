// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#pragma once

#include "tray_client.h"

#include <QWidget>

// The experimentation console.
//
// This is not a settings dialog, and adding one is out of scope: durable mode
// definitions live in the configuration file, and a second way to edit them
// would be a second source of truth. What lives here is the thing a file
// cannot do — sliders that apply immediately and are never written back, so a
// setpoint can be judged by ear, or a backlight range by eye, while the numbers
// that produced it are on screen beside it.
//
// When a live setting turns out to be worth keeping, the window offers it as a
// configuration snippet to copy. Promoting it stays a deliberate edit by the
// maintainer.
//
// The one thing this window writes is the tray's own autostart entry, and that
// is not configuration: no mode definition and no brightness profile is
// reachable from that switch, and the daemon's file has no way to describe it.
//
// The groups are split across tabs, because one column of all of them is taller
// than the screen and a window whose bottom is off screen hides the controls at
// the end of it. What only reports and what commands are on separate tabs, and
// the connection line and the reply line sit outside them.
//
// There are no charts. History is read with the export tool.
class TrayWindow : public QWidget {
    Q_OBJECT

public:
    explicit TrayWindow(TrayClient &client, QWidget *parent = nullptr);
    ~TrayWindow() override;

public slots:
    // Repaints the readings and moves the sliders to match, without echoing
    // the change back to the daemon.
    void applyStatus(const TrayClient::Status &status);

    // Live tuning, applied on the spot.
    void sendCurrentSetpoints();
    // The same, for the backlight window: how far the automation may wander
    // from the level the person set, and the two absolute bounds.
    void sendBrightnessWindow();
    // Copies the live setpoints to the clipboard as a configuration snippet.
    void copyConfigSnippet();
    // Writes or removes the tray's own autostart entry. The only thing this
    // window writes, and not the daemon's configuration: no mode definition and
    // no brightness profile is reachable from here.
    void setAutostart(bool on);
    // A judgement made by ear, stamped onto the current sample.
    void markGood();
    void markTooLoud();
    void markTooSlow();

signals:
    void modeSelected(const QString &modeId);

protected:
    // The window sizes itself the first time it is shown, not when it is built:
    // see the implementation for why a freshly built console cannot know how
    // wide it needs to be.
    void showEvent(QShowEvent *event) override;

private slots:
    // The daemon may go away under a running tray; the window says so instead
    // of leaving the last readings on screen as if they were current.
    void onConnectionChanged(bool connected);

private:
    void build();
    // Greys out everything that sends a command while nothing is answering.
    void setCommandsEnabled(bool enabled);
    // Sends one command, refreshes, and shows the daemon's own reply. An
    // experimentation console is worth nothing if a refused command is silent.
    void sendCommand(const QString &command);
    void updateTunerLabels();

    class Ui;
    Ui *m_ui;
    TrayClient &m_client;
    // Set while the window is being repainted from a status update, so that
    // moving a slider programmatically does not look like the user moving it.
    bool m_updating = false;
    // Set by the first showEvent, so reopening the console does not undo a size
    // the person chose by dragging the frame.
    bool m_sized = false;
    // The mode list the combo box currently shows. Rebuilding it on every
    // status update would close the drop-down while it is being used.
    QStringList m_modeIds;
    QStringList m_brightnessProfileIds;
};

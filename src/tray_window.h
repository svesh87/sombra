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
// setpoint can be judged by ear while the numbers that produced it are on
// screen beside it.
//
// When a live setting turns out to be worth keeping, the window offers it as a
// configuration snippet to copy. Promoting it stays a deliberate edit by the
// maintainer.
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
    // Copies the live setpoints to the clipboard as a configuration snippet.
    void copyConfigSnippet();
    // A judgement made by ear, stamped onto the current sample.
    void markGood();
    void markTooLoud();
    void markTooSlow();

signals:
    void modeSelected(const QString &modeId);

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
    // The mode list the combo box currently shows. Rebuilding it on every
    // status update would close the drop-down while it is being used.
    QStringList m_modeIds;
    QStringList m_brightnessProfileIds;
};

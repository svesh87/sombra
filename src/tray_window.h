// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#pragma once

#include "theme.h"
#include "tray_client.h"

#include <QWidget>

class QResizeEvent;
class QTimer;

// The console.
//
// Four tabs — the readings, the mode, the backlight, the settings — and the
// split is by subject rather than by how long a change lasts. Splitting the
// other way, into the mode and the saved definitions and the live sliders, puts
// the same eight numbers in three places, because "applies now" and "written
// down" are two properties of one thing, not two things.
//
// What the mode tab does, in the order it is used:
//
//   pick a mode          the controls take its numbers
//   move a control       it applies at once, and the mode selector empties —
//                        what is running is no longer any of the three
//   keep it              one button writes the controls into a mode you name
//
// Every number has a field beside its slider, because a slider cannot be
// landed on a particular value and these values are compared between sessions.
//
// The mode selector follows the desktop panel as well: switching there moves
// the controls here, and the panel and the console never disagree about what
// is running.
//
// The one thing this window writes by itself is the tray's own autostart
// entry. Everything else goes through the daemon, on the commands `sombra`
// sends, so there is exactly one writer of the settings file.
class TrayWindow : public QWidget {
    Q_OBJECT

public:
    explicit TrayWindow(TrayClient &client, QWidget *parent = nullptr);
    ~TrayWindow() override;

    // What the status line is saying, whole. The label itself holds only what fits
    // in its width, so this is what a test — or anything else asking what the
    // window said — has to look at.
    QString statusText() const;

    // Resizes the window to the tab it is showing and unlocks the frame. For
    // the documentation pictures: on screen the console keeps one size, which
    // is the size of its largest tab.
    void fitToCurrentTab();

public slots:
    // Repaints everything from a status reply, without echoing the change back
    // to the daemon.
    void applyStatus(const TrayClient::Status &status);

    // The thermal controls, applied the moment they move.
    void sendCurrentSetpoints();
    // Writes them into the mode named beside the button, through the daemon.
    void saveToMode();
    // Back to the numbers of the mode in force.
    void revertToMode();
    // Every mode definition and every backlight setting back to what is
    // compiled into the daemon. The button that calls this asks first: it is
    // rare, it discards work, and there is no undo behind it.
    void resetEverything();

    // The backlight. The level is the ordinary brightness control; the other
    // two say how far the automation may move and what it may never go below.
    void sendBacklightLevel();
    // The level the automation works around. Moving it is the way to set the
    // baseline outright, rather than waiting for the light that would leave the
    // wanted baseline behind after moving the brightness itself.
    void sendBacklightBase();
    void sendBacklightSettings(bool durable);

    // Writes or removes the tray's own autostart entry.
    void setAutostart(bool on);

    // Light, dark, or whatever the desktop says. Stored in the console's own
    // settings and applied at once.
    // Light, dark, or whatever the desktop says. Stored in the console's own
    // settings and applied at once — to the widgets. The title bar is Qt's own
    // on a Wayland desktop and takes its colours when the window is created, so
    // it stays in the previous scheme until the console is closed and opened
    // again. Rebuilding the window to refresh it took the process down with it.
    void setAppearance(theme::Preference wanted);

    // A judgement made by ear, stamped onto the current sample.
    void markGood();
    void markTooLoud();
    void markTooSlow();

signals:
    void modeSelected(const QString &modeId);

protected:
    // The status line is elided to the width it has, so it is re-elided whenever
    // that width changes.
    void resizeEvent(QResizeEvent *event) override;

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
    // One size for every tab: the largest, capped by the display. Called once,
    // from the first showEvent, and the frame is fixed there — the window
    // already fits what it has to fit, so dragging it can only produce empty
    // space or a scrollbar over content that had room.
    void fitToLargestTab();
    // The height the window needs at a given width, which is what makes the
    // width a choice rather than a guess: a translated label wraps where an
    // English one did not, and a wider window is a shorter one.
    int tallestTab(int width);
    void setCommandsEnabled(bool enabled);
    // Sends one command, refreshes, and shows the daemon's own reply. A console
    // is worth nothing if a refused command is silent.
    void sendCommand(const QString &command);

public:
    // Puts a line on the status line. A refusal stays there until the next
    // command; anything else goes back to the daemon's state after a few
    // seconds, so the line is never a log and never an empty strip.
    void report(const QString &text, bool refused);
private:
    // What the status line says when nothing has just happened, and what its
    // tooltip says behind it: the short answer and the diagnosis.
    QString connectionText() const;
    QString connectionDetail() const;
    // Puts `m_statusFull` on the line, elided to the room there is.
    void paintStatus();
    void showConnectionState();
    // Gives every entry of the mode selectors an explanation of its own, built
    // from that mode's saved numbers: a list of three names says nothing to
    // somebody seeing it for the first time.
    void describeModes();
    // Asks the daemon what it has saved. On demand rather than polled:
    // definitions change when somebody changes them.
    void reloadConfiguration();
    // "fan=max", or "fan=auto profile=<curve>": one control, one answer.
    QString fanSetting() const;

    class Ui;
    Ui *m_ui;
    TrayClient &m_client;
    // Set while the window is being repainted from a status update, so that
    // moving a control programmatically does not look like the person moving
    // it.
    bool m_updating = false;
    bool m_sized = false;
    bool m_connected = false;
    // Set while the line is holding a refusal, which is what keeps a status
    // update from wiping an answer nobody has read yet.
    bool m_holdingRefusal = false;
    QTimer *m_statusTimer = nullptr;
    // What the status line is saying, before eliding. The label holds only what
    // fits, and the tooltip holds this.
    QString m_statusFull;
    // The mode list the selectors currently show. Rebuilding on every status
    // update would close a drop-down while it is being used.
    QStringList m_modeIds;
    // Set once the person picks a destination themselves, after which the
    // window stops proposing one.
    bool m_saveTargetChosen = false;
    TrayClient::Configuration m_configuration;
};

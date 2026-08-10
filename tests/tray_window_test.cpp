// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "autostart.h"
#include "control_socket.h"
#include "fake_sysfs.h"
#include "brightness_map.h"
#include "modes.h"
#include "theme.h"
#include "tray_window.h"

#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDir>
#include <QStandardPaths>
#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QImage>
#include <QLabel>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QSignalSpy>
#include <QSlider>
#include <QSpinBox>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

namespace {

// The daemon runs as its own process here, which is what it is on the machine
// as well. It cannot be an object in this process: the window drives it
// through a blocking exchange, and a daemon sharing the thread would never
// reach its own event loop to answer.
class TestDaemon {
public:
    TestDaemon(const QString &sysfsRoot, QString workDir)
        : m_work(std::move(workDir))
    {
        m_process.start(binary(),
                        { QStringLiteral("--no-dbus"), QStringLiteral("--root"), sysfsRoot,
                          QStringLiteral("--ring"), m_work + QStringLiteral("/metrics.ring"),
                          QStringLiteral("--socket"), socketPath(), QStringLiteral("--state"),
                          m_work + QStringLiteral("/state"), QStringLiteral("--config"),
                          m_work + QStringLiteral("/settings.conf"), QStringLiteral("--capacity"),
                          QStringLiteral("64"), QStringLiteral("--interval-ms"),
                          QStringLiteral("20") });
    }

    ~TestDaemon() { stop(); }

    QString socketPath() const { return m_work + QStringLiteral("/control.sock"); }

    bool waitUntilListening()
    {
        if (!m_process.waitForStarted(10000))
            return false;
        QDeadlineTimer deadline(10000);
        while (!QFile::exists(socketPath()) && !deadline.hasExpired())
            QTest::qWait(10);
        return QFile::exists(socketPath());
    }

    // SIGTERM is how systemd stops it, and it restores the hardware and
    // removes the socket on the way out.
    void stop()
    {
        if (m_process.state() == QProcess::NotRunning)
            return;
        m_process.terminate();
        m_process.waitForFinished(10000);
    }

    static QString binary()
    {
        return QDir::cleanPath(QCoreApplication::applicationDirPath()
                               + QStringLiteral("/../ombrad"));
    }

private:
    QString m_work;
    QProcess m_process;
};

// The environment a spawned ombra-tray gets.
//
// The offscreen platform keeps its window off the display, but that is not
// enough on its own: QSystemTrayIcon speaks StatusNotifierItem over the session
// bus, which is a protocol and not a window, so an offscreen process still puts
// a real icon into the panel of whoever is running the suite — and a killed one
// never gets to remove it again. Pointing the process at a bus that does not
// exist leaves it with no tray to register with, and main() then shows the
// console instead, which offscreen means nothing at all.
QProcessEnvironment isolatedEnvironment()
{
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    environment.insert(QStringLiteral("DBUS_SESSION_BUS_ADDRESS"),
                       QStringLiteral("unix:path=/nonexistent/ombra-test-has-no-session-bus"));
    return environment;
}

// A failing QVERIFY returns from the test, so a spawned process has to be
// reaped by a destructor: a line at the end of the case would be skipped, and
// the leftover would poll a dead socket for as long as the machine is up.
class Reaped {
public:
    explicit Reaped(QProcess &process)
        : m_process(process)
    {
    }
    ~Reaped()
    {
        if (m_process.state() == QProcess::NotRunning)
            return;
        m_process.kill();
        m_process.waitForFinished(10000);
    }

private:
    QProcess &m_process;
};

TrayClient::Status statusFrom(const QByteArray &json)
{
    return TrayClient::parseStatus(QJsonDocument::fromJson(json).object());
}

} // namespace

class TrayWindowTest : public QObject {
    Q_OBJECT

private slots:
    void init();

    void showsTheReadingsIncludingTheCorePowerSplit();
    void showsAbsentReadingsAsADash();
    void showsTheGuardAndTheWriteMismatch();
    void movingASliderTunesTheDaemonImmediately();
    void theOtherLiveControlsReachTheDaemon();
    void selectingAModeSendsTheModeCommand();
    void theMarkButtonsStampTheCurrentSample();
    void movingAControlEmptiesTheModeSelector();
    void savesTheChangedNumbersIntoTheModeThatIsNamed();
    void theFanIsOneControlWithFourAnswers();
    void theBacklightSwitchIsOneAction();
    void theBacklightIsThreeControlsInPerCent();
    void theAutostartSwitchOwnsItsOwnEntry();
    void theConsoleAppearanceIsAChoiceOfThree();
    void oneResetCoversEverySettingThereIs();
    void repaintingFromAStatusSendsNothing();
    void saysSoWhenTheDaemonIsUnreachable();
    void theWindowKeepsOneSizeForEveryTab();
    void theReplyLineTakesNoRoomUntilThereIsAReply();
    void theTrayBinaryRunsAgainstADaemon();
    void theTrayWaitsForALateTrayHost();
    void theTrayBinaryWritesTheDocumentationScreenshots();

private:
    QString socketPath() const { return m_work.path() + QStringLiteral("/control.sock"); }
    QString configPath() const { return m_work.path() + QStringLiteral("/settings.conf"); }

    QTemporaryDir m_work;
    // Rebuilt before every test: a shared tree would carry one case's writes
    // into the next.
    std::unique_ptr<FakeSysfs> m_tree;
};

void TrayWindowTest::init()
{
    m_tree = std::make_unique<FakeSysfs>();
    QFile::remove(m_work.path() + QStringLiteral("/state"));
    QFile::remove(m_work.path() + QStringLiteral("/metrics.ring"));
    // The settings file too: cases that save a definition would otherwise hand
    // it to every case after them, and a daemon booting into somebody else's
    // mode is a confusing way to fail.
    QFile::remove(configPath());
}

void TrayWindowTest::showsTheReadingsIncludingTheCorePowerSplit()
{
    TrayClient client(socketPath());
    TrayWindow window(client);

    window.applyStatus(statusFrom(R"({
        "mode": "work",
        "modes": [{"id": "quiet", "label": "Quiet"}, {"id": "work", "label": "Work"}],
        "live_tuning": true, "on_ac": true,
        "applied": {"profile": "balanced", "pl1_w": 20, "pl2_w": 30, "max_perf_pct": 50,
                    "turbo": false, "epp": "power", "hwp_boost": false, "gt_max_mhz": 2350,
                    "fan_mode": 2},
        "requested": {"profile": "quiet", "pl1_w": 17, "pl2_w": 26, "max_perf_pct": 40,
                      "turbo": true, "epp": "performance", "hwp_boost": true,
                      "gt_max_mhz": 1200},
        "sensors": {"pkg_c": 61.0, "board_c": 45.0, "nvme_c": 38.8, "fan_rpm": 1900,
                    "pkg_w": 7.25, "core_w": 4.5, "uncore_w": 1.25, "cpu_mhz": 2400,
                    "gpu_mhz": 300, "load1": 0.61}
    })"));

    QCOMPARE(window.findChild<QLabel *>(QStringLiteral("pkg_c"))->text(),
             QStringLiteral("61.0 °C"));
    QCOMPARE(window.findChild<QLabel *>(QStringLiteral("board_c"))->text(),
             QStringLiteral("45.0 °C"));
    QCOMPARE(window.findChild<QLabel *>(QStringLiteral("nvme_c"))->text(),
             QStringLiteral("38.8 °C"));
    QCOMPARE(window.findChild<QLabel *>(QStringLiteral("fan_rpm"))->text(),
             QStringLiteral("1900 RPM"));
    QCOMPARE(window.findChild<QLabel *>(QStringLiteral("pkg_w"))->text(),
             QStringLiteral("7.25 W"));
    // The split is what distinguishes CPU heat from iGPU heat.
    QCOMPARE(window.findChild<QLabel *>(QStringLiteral("core_w"))->text(),
             QStringLiteral("4.50 W"));
    QCOMPARE(window.findChild<QLabel *>(QStringLiteral("uncore_w"))->text(),
             QStringLiteral("1.25 W"));
    QCOMPARE(window.findChild<QLabel *>(QStringLiteral("cpu_mhz"))->text(),
             QStringLiteral("2400 MHz"));
    QCOMPARE(window.findChild<QLabel *>(QStringLiteral("gpu_mhz"))->text(),
             QStringLiteral("300 MHz"));
    QCOMPARE(window.findChild<QLabel *>(QStringLiteral("load1"))->text(),
             QStringLiteral("0.61"));

    // The sliders follow what was asked for; what actually reached the
    // hardware is stated separately, because the guard can differ from it.
    QCOMPARE(window.findChild<QSlider *>(QStringLiteral("pl1_slider"))->value(), 17);
    QCOMPARE(window.findChild<QSlider *>(QStringLiteral("pl2_slider"))->value(), 26);
    QCOMPARE(window.findChild<QSlider *>(QStringLiteral("max_perf_pct_slider"))->value(), 40);
    QCOMPARE(window.findChild<QSlider *>(QStringLiteral("gt_max_mhz_slider"))->value(), 1200);
    // The field beside each slider carries the same value and its unit: a
    // slider cannot be landed on a number, and these numbers are typed.
    QCOMPARE(window.findChild<QSpinBox *>(QStringLiteral("pl1_box"))->value(), 17);
    QCOMPARE(window.findChild<QSpinBox *>(QStringLiteral("gt_max_mhz_box"))->suffix(),
             QStringLiteral(" MHz"));
    QCOMPARE(window.findChild<QComboBox *>(QStringLiteral("epp"))->currentText(),
             QStringLiteral("performance"));
    QCOMPARE(window.findChild<QComboBox *>(QStringLiteral("profile"))->currentText(),
             QStringLiteral("quiet"));
    QVERIFY(window.findChild<QCheckBox *>(QStringLiteral("turbo"))->isChecked());
    QVERIFY(window.findChild<QCheckBox *>(QStringLiteral("hwp_boost"))->isChecked());
    QVERIFY(window.findChild<QLabel *>(QStringLiteral("applied"))->text()
                    .contains(QStringLiteral("PL1 20 W")));
    QCOMPARE(window.findChild<QLabel *>(QStringLiteral("power_source"))->text(),
             QStringLiteral("On mains"));
    // Live tuning is announced, because those values survive nothing.
    QVERIFY(!window.findChild<QLabel *>(QStringLiteral("live_tuning"))->isHidden());

    // Empty, because this status says the controls have been moved: what is
    // running is not any of the saved modes.
    auto *mode = window.findChild<QComboBox *>(QStringLiteral("mode"));
    QCOMPARE(mode->count(), 2);
    QCOMPARE(mode->currentIndex(), -1);
}

void TrayWindowTest::showsAbsentReadingsAsADash()
{
    TrayClient client(socketPath());
    TrayWindow window(client);

    window.applyStatus(statusFrom(R"({
        "mode": "work", "modes": [{"id": "work", "label": "Work"}],
        "applied": {"pl1_w": 20, "fan_mode": 2}, "requested": {"pl1_w": 20},
        "sensors": {"pkg_c": null, "board_c": null, "nvme_c": null, "fan_rpm": null,
                    "pkg_w": null, "core_w": null, "uncore_w": null, "cpu_mhz": null,
                    "gpu_mhz": null, "load1": null}
    })"));

    // A dash, never an invented zero: zero is a legal fan speed and a legal
    // power draw.
    for (const QString &name : { QStringLiteral("pkg_c"), QStringLiteral("board_c"),
                                 QStringLiteral("nvme_c"), QStringLiteral("fan_rpm"),
                                 QStringLiteral("pkg_w"), QStringLiteral("core_w"),
                                 QStringLiteral("uncore_w"), QStringLiteral("cpu_mhz"),
                                 QStringLiteral("gpu_mhz"), QStringLiteral("load1") }) {
        QCOMPARE(window.findChild<QLabel *>(name)->text(), QStringLiteral("—"));
    }
    QCOMPARE(window.findChild<QLabel *>(QStringLiteral("power_source"))->text(),
             QStringLiteral("On battery"));
}

void TrayWindowTest::showsTheGuardAndTheWriteMismatch()
{
    TrayClient client(socketPath());
    TrayWindow window(client);
    auto *guard = window.findChild<QLabel *>(QStringLiteral("guard"));
    auto *mismatch = window.findChild<QLabel *>(QStringLiteral("mismatch"));
    auto *sensors = window.findChild<QLabel *>(QStringLiteral("sensors"));
    auto *live = window.findChild<QLabel *>(QStringLiteral("live_tuning"));
    QVERIFY(guard->isHidden());
    QVERIFY(mismatch->isHidden());

    window.applyStatus(statusFrom(R"({
        "mode": "full", "modes": [{"id": "full", "label": "Full"}],
        "guard_engaged": true, "write_mismatch": true, "sensors_degraded": true,
        "live_tuning": false,
        "applied": {"pl1_w": 10, "pl2_w": 15, "fan_mode": 0},
        "requested": {"pl1_w": 45, "pl2_w": 65},
        "sensors": {"pkg_c": 99.5}
    })"));

    // The guard is a safety interlock and its state has to be visible.
    QVERIFY(!guard->isHidden());
    // A value the daemon did not write means something else is writing these
    // controls too, which is the one thing this design forbids.
    QVERIFY(!mismatch->isHidden());
    QVERIFY(!sensors->isHidden());
    QVERIFY(live->isHidden());
    QVERIFY(window.findChild<QLabel *>(QStringLiteral("applied"))->text()
                    .contains(QStringLiteral("PL1 10 W")));
    QCOMPARE(window.findChild<QSlider *>(QStringLiteral("pl1_slider"))->value(), 45);
    // The window reports the fan that is actually running, and the guard is
    // the one part of automatic operation that forces it.

    window.applyStatus(statusFrom(R"({
        "mode": "full", "modes": [{"id": "full", "label": "Full"}],
        "applied": {"pl1_w": 45, "fan_mode": 2}, "requested": {"pl1_w": 45},
        "sensors": {"pkg_c": 61.0}
    })"));
    QVERIFY(guard->isHidden());
    QVERIFY(mismatch->isHidden());
    QVERIFY(sensors->isHidden());
}

void TrayWindowTest::movingASliderTunesTheDaemonImmediately()
{
    TestDaemon daemon(m_tree->root(), m_work.path());
    QVERIFY(daemon.waitUntilListening());
    TrayClient client(socketPath());
    TrayWindow window(client);
    client.poll();

    auto *pl1 = window.findChild<QSlider *>(QStringLiteral("pl1_slider"));
    QCOMPARE(pl1->value(), 20);
    // Nothing, not a slider either, raises a limit past what the maintainer
    // has confirmed on this machine.
    QCOMPARE(pl1->maximum(), kMaxConfirmedPl1W);

    pl1->setValue(15);
    QVERIFY(client.status().liveTuning);
    QCOMPARE(client.status().requested.pl1W, 15);
    // The rest of the live overlay travels with it, unchanged.
    QCOMPARE(client.status().requested.pl2W, 30);
    QCOMPARE(client.status().requested.maxPerfPct, 50);
    QCOMPARE(client.status().requested.profile, Profile::Balanced);
    QCOMPARE(window.findChild<QSpinBox *>(QStringLiteral("pl1_box"))->value(), 15);
    QVERIFY(window.findChild<QLabel *>(QStringLiteral("notice"))->text()
                    .contains(QStringLiteral("pl1=15")));
    // Applied at once and not written down: keeping it is a separate act with
    // a named destination.
    QVERIFY(!QFile::exists(configPath()));

    window.findChild<QPushButton *>(QStringLiteral("revert"))->click();
    QVERIFY(!client.status().liveTuning);
    QCOMPARE(client.status().requested.pl1W, 20);
    QCOMPARE(pl1->value(), 20);
}

void TrayWindowTest::theOtherLiveControlsReachTheDaemon()
{
    TestDaemon daemon(m_tree->root(), m_work.path());
    QVERIFY(daemon.waitUntilListening());
    TrayClient client(socketPath());
    TrayWindow window(client);
    client.poll();

    window.findChild<QSlider *>(QStringLiteral("pl2_slider"))->setValue(40);
    QCOMPARE(client.status().requested.pl2W, 40);
    window.findChild<QSlider *>(QStringLiteral("max_perf_pct_slider"))->setValue(35);
    QCOMPARE(client.status().requested.maxPerfPct, 35);
    // The iGPU ceiling exists because none of the CPU-side controls touch the
    // graphics engine, and a video call loads it as heavily as the cores.
    window.findChild<QSlider *>(QStringLiteral("gt_max_mhz_slider"))->setValue(1200);
    QCOMPARE(client.status().requested.gtMaxMhz, 1200);
    window.findChild<QCheckBox *>(QStringLiteral("turbo"))->setChecked(true);
    QVERIFY(!client.status().requested.noTurbo);
    window.findChild<QCheckBox *>(QStringLiteral("hwp_boost"))->setChecked(true);
    QVERIFY(client.status().requested.hwpBoost);

    auto *epp = window.findChild<QComboBox *>(QStringLiteral("epp"));
    epp->setCurrentIndex(epp->findData(static_cast<int>(Epp::BalancePower)));
    QCOMPARE(client.status().requested.epp, Epp::BalancePower);

    // The platform profile is the EC fan curve, which is the noise-versus-
    // temperature dial and one of the things worth judging by ear.
    auto *profile = window.findChild<QComboBox *>(QStringLiteral("profile"));
    profile->setCurrentIndex(profile->findData(static_cast<int>(Profile::Quiet)));
    QCOMPARE(client.status().requested.profile, Profile::Quiet);
    QTRY_COMPARE(m_tree->read(FakeSysfs::platformProfile()), QStringLiteral("quiet"));
}

void TrayWindowTest::selectingAModeSendsTheModeCommand()
{
    TestDaemon daemon(m_tree->root(), m_work.path());
    QVERIFY(daemon.waitUntilListening());
    TrayClient client(socketPath());
    TrayWindow window(client);
    QSignalSpy selected(&window, &TrayWindow::modeSelected);
    client.poll();

    auto *mode = window.findChild<QComboBox *>(QStringLiteral("mode"));
    QCOMPARE(mode->count(), 3);
    // Ordered coolest first, with the working mode in the middle.
    QCOMPARE(mode->itemData(0).toString(), QStringLiteral("quiet"));
    QCOMPARE(mode->currentIndex(), 1);
    QCOMPARE(selected.count(), 0);

    mode->setCurrentIndex(0);
    QCOMPARE(selected.count(), 1);
    QCOMPARE(selected.first().first().toString(), QStringLiteral("quiet"));
    QCOMPARE(client.status().modeId, QStringLiteral("quiet"));
    QCOMPARE(mode->currentIndex(), 0);

    // Selecting a mode is the deliberate act that drops live tuning.
    window.findChild<QSlider *>(QStringLiteral("pl1_slider"))->setValue(11);
    QVERIFY(client.status().liveTuning);
    mode->setCurrentIndex(2);
    QVERIFY(!client.status().liveTuning);
    QCOMPARE(client.status().modeId, QStringLiteral("full"));
}

void TrayWindowTest::theMarkButtonsStampTheCurrentSample()
{
    TestDaemon daemon(m_tree->root(), m_work.path());
    QVERIFY(daemon.waitUntilListening());
    TrayClient client(socketPath());
    TrayWindow window(client);
    // The verdict buttons are dead until something answers, so the window has
    // to have seen a status before they can be clicked.
    client.poll();
    auto *notice = window.findChild<QLabel *>(QStringLiteral("notice"));

    // A judgement made by ear is worthless unless it can be traced back to the
    // numbers that produced it, which is what stamping the sample buys.
    window.findChild<QPushButton *>(QStringLiteral("mark_good"))->click();
    QCOMPARE(notice->text(), QStringLiteral("mark good → ok"));
    window.findChild<QPushButton *>(QStringLiteral("mark_too_loud"))->click();
    QCOMPARE(notice->text(), QStringLiteral("mark too-loud → ok"));
    window.findChild<QPushButton *>(QStringLiteral("mark_too_slow"))->click();
    QCOMPARE(notice->text(), QStringLiteral("mark too-slow → ok"));
}

void TrayWindowTest::movingAControlEmptiesTheModeSelector()
{
    TestDaemon daemon(m_tree->root(), m_work.path());
    QVERIFY(daemon.waitUntilListening());
    TrayClient client(socketPath());
    TrayWindow window(client);
    client.poll();

    auto *mode = window.findChild<QComboBox *>(QStringLiteral("mode"));
    QCOMPARE(mode->count(), 3);
    QCOMPARE(mode->currentData().toString(), QStringLiteral("work"));

    // Moving a control is what makes the running numbers stop being any of the
    // three, and the empty selector is the whole signal for that.
    window.findChild<QSpinBox *>(QStringLiteral("pl1_box"))->setValue(17);
    QTRY_VERIFY(client.status().liveTuning);
    QCOMPARE(mode->currentIndex(), -1);
    QVERIFY(window.findChild<QLabel *>(QStringLiteral("mode_state"))
                    ->text()
                    .contains(QStringLiteral("Changed from the saved Work")));
    // Which mode comes back at boot is a statement about a mode, and there is
    // no mode to make it about.
    QVERIFY(!window.findChild<QCheckBox *>(QStringLiteral("mode_at_boot"))->isEnabled());

    // Reverting puts the selector back.
    window.findChild<QPushButton *>(QStringLiteral("revert"))->click();
    QTRY_VERIFY(!client.status().liveTuning);
    QCOMPARE(mode->currentData().toString(), QStringLiteral("work"));
}

void TrayWindowTest::savesTheChangedNumbersIntoTheModeThatIsNamed()
{
    TestDaemon daemon(m_tree->root(), m_work.path());
    QVERIFY(daemon.waitUntilListening());
    TrayClient client(socketPath());
    TrayWindow window(client);
    client.poll();
    QTest::qWait(20);

    window.findChild<QSpinBox *>(QStringLiteral("pl1_box"))->setValue(17);
    QTRY_VERIFY(client.status().liveTuning);

    // The destination starts on the mode it was changed from, because that is
    // what the person is thinking about — and it is a choice, not an
    // assumption.
    auto *target = window.findChild<QComboBox *>(QStringLiteral("save_target"));
    QCOMPARE(target->currentData().toString(), QStringLiteral("work"));
    target->setCurrentIndex(target->findData(QStringLiteral("quiet")));

    QVERIFY(!QFile::exists(configPath()));
    window.findChild<QPushButton *>(QStringLiteral("save"))->click();

    // The daemon does the writing, and what lands on disk is what its own
    // parser reads back.
    QVERIFY(QFile::exists(configPath()));
    ModeTable table;
    QVERIFY(table.load(configPath()));
    QCOMPARE(table.find(QStringLiteral("quiet"))->setpoints.pl1W, 17);
    // The mode in force was not the destination, so its definition is
    // untouched.
    QCOMPARE(table.find(QStringLiteral("work"))->setpoints.pl1W, 20);
}

void TrayWindowTest::theFanIsOneControlWithFourAnswers()
{
    TestDaemon daemon(m_tree->root(), m_work.path());
    QVERIFY(daemon.waitUntilListening());
    TrayClient client(socketPath());
    TrayWindow window(client);
    client.poll();

    // Three of them are the firmware's curves and the fourth bypasses them,
    // but the machine has one fan and so the window has one control. Two
    // groups doing fan-shaped things would read as a duplicate, whatever the
    // difference between them.
    auto *fan = window.findChild<QComboBox *>(QStringLiteral("profile"));
    QCOMPARE(fan->count(), 4);
    QCOMPARE(fan->currentText(), QStringLiteral("balanced"));
    QCOMPARE(m_tree->read(FakeSysfs::asus() + QStringLiteral("/pwm1_enable")),
             QStringLiteral("2"));

    fan->setCurrentIndex(fan->findText(QStringLiteral("maximum")));
    QTRY_COMPARE(m_tree->read(FakeSysfs::asus() + QStringLiteral("/pwm1_enable")),
                 QStringLiteral("0"));
    // Full speed is a setpoint like the others now, so the machine is no
    // longer running a saved mode and the selector says so.
    QVERIFY(client.status().liveTuning);
    client.poll();
    QCOMPARE(fan->currentText(), QStringLiteral("maximum"));

    fan->setCurrentIndex(fan->findText(QStringLiteral("quiet")));
    QTRY_COMPARE(m_tree->read(FakeSysfs::asus() + QStringLiteral("/pwm1_enable")),
                 QStringLiteral("2"));
    QCOMPARE(m_tree->read(FakeSysfs::platformProfile()), QStringLiteral("quiet"));
}

void TrayWindowTest::theBacklightSwitchIsOneAction()
{
    TestDaemon daemon(m_tree->root(), m_work.path());
    QVERIFY(daemon.waitUntilListening());
    TrayClient client(socketPath());
    TrayWindow window(client);
    client.poll();

    auto *toggle = window.findChild<QCheckBox *>(QStringLiteral("auto_brightness"));
    QVERIFY(toggle->isEnabled());
    // Off by default: the automation is something the person switches on.
    QVERIFY(!toggle->isChecked());

    // One click, and the daemon has it.
    toggle->setChecked(true);
    QVERIFY(client.status().autoBrightness);
    client.poll();
    // What it says while on is a band in per cent, because that is the whole
    // of what a person needs to know about it.
    QVERIFY(window.findChild<QLabel *>(QStringLiteral("backlight_state"))
                    ->text()
                    .contains(QStringLiteral("you set")));

    toggle->setChecked(false);
    QVERIFY(!client.status().autoBrightness);
    client.poll();
    QVERIFY(window.findChild<QLabel *>(QStringLiteral("backlight_state"))
                    ->text()
                    .contains(QStringLiteral("yours alone")));
}

void TrayWindowTest::theBacklightIsThreeControlsInPerCent()
{
    TestDaemon daemon(m_tree->root(), m_work.path());
    QVERIFY(daemon.waitUntilListening());
    TrayClient client(socketPath());
    TrayWindow window(client);
    client.poll();

    auto *response = window.findChild<QSpinBox *>(QStringLiteral("response_box"));
    auto *floor = window.findChild<QSpinBox *>(QStringLiteral("floor_box"));
    QVERIFY(response);
    QCOMPARE(response->value(), kBrightnessDefaultResponsePct);
    QCOMPARE(floor->value(), kBrightnessDefaultFloorPct);
    // Per cent, and nothing else: no profile to choose, and nothing about the
    // sensor on screen.
    QCOMPARE(response->suffix(), QStringLiteral(" %"));
    QVERIFY(!window.findChild<QComboBox *>(QStringLiteral("brightness_profile")));

    // "It reacts too much" is answered here, and the answer applies at once.
    response->setValue(45);
    QTRY_COMPARE(client.status().brightnessResponsePct, 45);

    // And it is a setting rather than an experiment: typing into the field
    // ends the gesture, so it is written down.
    QTRY_VERIFY(QFile::exists(configPath()));
    BrightnessSettings saved = builtinBrightnessSettings();
    QVERIFY(loadBrightnessSettings(configPath(), &saved));
    QCOMPARE(saved.responsePct, 45);

    // The band is what the window says out loud, in per cent.
    floor->setValue(25);
    QTRY_COMPARE(client.status().brightnessFloorPct, 25);
    client.poll();
    const QString state = window.findChild<QLabel *>(QStringLiteral("backlight_state"))->text();
    QVERIFY2(!state.contains(QStringLiteral("lux")), qPrintable(state));
}

void TrayWindowTest::theAutostartSwitchOwnsItsOwnEntry()
{
    QTemporaryDir config;
    QVERIFY(config.isValid());
    qputenv("XDG_CONFIG_HOME", config.path().toUtf8());

    // No daemon on purpose: the tray starting with the session has nothing to
    // do with whether the daemon is answering, so the switch must work anyway.
    TrayClient client(socketPath());
    TrayWindow window(client);
    auto *toggle = window.findChild<QCheckBox *>(QStringLiteral("autostart"));
    QVERIFY(toggle);
    QVERIFY(toggle->isEnabled());
    QVERIFY(!toggle->isChecked());

    toggle->setChecked(true);
    QVERIFY(QFile::exists(autostart::entryPath()));
    QVERIFY(window.findChild<QLabel *>(QStringLiteral("notice"))
                    ->text()
                    .contains(QStringLiteral("next login")));

    toggle->setChecked(false);
    QVERIFY(!QFile::exists(autostart::entryPath()));

    // An entry already there is what a freshly opened window reads, because the
    // file is the state and nothing else is remembered.
    QVERIFY(autostart::setEnabled(true));
    TrayWindow reopened(client);
    QVERIFY(reopened.findChild<QCheckBox *>(QStringLiteral("autostart"))->isChecked());
    QVERIFY(autostart::setEnabled(false));

    // A failed write puts the switch back: a checkbox that stayed on would be
    // claiming something the file does not say.
    qputenv("XDG_CONFIG_HOME", QByteArray("/proc/self/impossible"));
    toggle->setChecked(true);
    QVERIFY(!toggle->isChecked());
    QVERIFY(window.findChild<QLabel *>(QStringLiteral("notice"))
                    ->text()
                    .startsWith(QStringLiteral("autostart")));

    qputenv("XDG_CONFIG_HOME", config.path().toUtf8());
}

void TrayWindowTest::theConsoleAppearanceIsAChoiceOfThree()
{
    // The console's own look, not the machine's, and not the panel's either: the
    // tray icon is the desktop's to draw whatever is chosen here.
    QStandardPaths::setTestModeEnabled(true);
    theme::setPreference(theme::Preference::System);

    TrayClient client(socketPath());
    TrayWindow window(client);
    auto *appearance = window.findChild<QComboBox *>(QStringLiteral("appearance"));
    QVERIFY(appearance);
    QCOMPARE(appearance->count(), 3);
    QCOMPARE(appearance->currentData().toString(), QStringLiteral("system"));

    appearance->setCurrentIndex(appearance->findData(QStringLiteral("dark")));
    QCOMPARE(theme::preference(), theme::Preference::Dark);
    QCOMPARE(theme::schemeOf(QApplication::palette()), theme::Scheme::Dark);

    appearance->setCurrentIndex(appearance->findData(QStringLiteral("light")));
    QCOMPARE(theme::preference(), theme::Preference::Light);
    QCOMPARE(theme::schemeOf(QApplication::palette()), theme::Scheme::Light);

    // And a window opened afterwards shows the stored choice rather than the
    // first item.
    TrayWindow reopened(client);
    QCOMPARE(reopened.findChild<QComboBox *>(QStringLiteral("appearance"))
                     ->currentData()
                     .toString(),
             QStringLiteral("light"));

    theme::setPreference(theme::Preference::System);
}

void TrayWindowTest::oneResetCoversEverySettingThereIs()
{
    TestDaemon daemon(m_tree->root(), m_work.path());
    QVERIFY(daemon.waitUntilListening());
    TrayClient client(socketPath());
    TrayWindow window(client);
    client.poll();

    // Something saved on both sides, so a reset that covered only one of them
    // would show.
    QVERIFY(client.send(QStringLiteral("config set mode work pl1_w=11"))
                    .startsWith(QLatin1String("ok")));
    QVERIFY(client.send(QStringLiteral("config set brightness response_pct=45"))
                    .startsWith(QLatin1String("ok")));

    auto *reset = window.findChild<QPushButton *>(QStringLiteral("reset_all"));
    QVERIFY(reset);
    // One button, and it is not on the tab whose numbers it discards: it covers
    // every setting there is and is used about once.
    QCOMPARE(window.findChild<QPushButton *>(QStringLiteral("reset_modes")), nullptr);

    window.resetEverything();
    QTest::qWait(50);

    ModeTable modes;
    QVERIFY(modes.load(configPath()));
    QCOMPARE(modes.find(QStringLiteral("work"))->setpoints.pl1W,
             ModeTable::builtinDefaults().find(QStringLiteral("work"))->setpoints.pl1W);
    BrightnessSettings brightness;
    QVERIFY(loadBrightnessSettings(configPath(), &brightness));
    QCOMPARE(brightness.responsePct, kBrightnessDefaultResponsePct);
}

void TrayWindowTest::repaintingFromAStatusSendsNothing()
{
    TestDaemon daemon(m_tree->root(), m_work.path());
    QVERIFY(daemon.waitUntilListening());
    TrayClient client(socketPath());
    TrayWindow window(client);
    client.poll();

    // Moving the controls to match a status update must not look like the user
    // moving them, or the tray would tune the daemon towards its own display.
    window.applyStatus(statusFrom(R"({
        "mode": "full",
        "modes": [{"id": "quiet", "label": "Quiet"}, {"id": "work", "label": "Work"},
                  {"id": "full", "label": "Full"}],
        "applied": {"profile": "performance", "pl1_w": 33, "fan_mode": 0},
        "requested": {"profile": "performance", "pl1_w": 33, "pl2_w": 44,
                      "max_perf_pct": 90, "turbo": true, "epp": "performance",
                      "hwp_boost": true, "gt_max_mhz": 1500},
        "sensors": {"pkg_c": 61.0}
    })"));
    QCOMPARE(window.findChild<QSlider *>(QStringLiteral("pl1_slider"))->value(), 33);
    QCOMPARE(window.findChild<QComboBox *>(QStringLiteral("mode"))->currentIndex(), 2);

    client.poll();
    QVERIFY(!client.status().liveTuning);
    QCOMPARE(client.status().modeId, QStringLiteral("work"));
    QCOMPARE(client.status().requested.pl1W, 20);
    QCOMPARE(client.status().applied.fanMode, 2);
}

void TrayWindowTest::saysSoWhenTheDaemonIsUnreachable()
{
    TrayClient client(m_work.path() + QStringLiteral("/absent.sock"));
    TrayWindow window(client);

    // The banner names the socket and the group, because "not answering" on
    // its own leaves the person guessing which of the two usual causes it is.
    auto *banner = window.findChild<QLabel *>(QStringLiteral("connection"));
    QVERIFY(!banner->isHidden());
    QVERIFY(banner->text().contains(QStringLiteral("not answering")));
    QVERIFY(banner->text().contains(client.socketPath()));
    QVERIFY(banner->text().contains(QString::fromLatin1(kControlSocketGroup)));

    // Nothing that would send a command is usable, so a slider parked at its
    // minimum cannot be read as a setpoint the daemon is holding.
    QVERIFY(!window.findChild<QSlider *>(QStringLiteral("pl1_slider"))->isEnabled());
    QVERIFY(!window.findChild<QComboBox *>(QStringLiteral("mode"))->isEnabled());
    QVERIFY(!window.findChild<QCheckBox *>(QStringLiteral("auto_brightness"))->isEnabled());

    // A command that goes nowhere says so rather than looking like it worked.
    window.markGood();
    QCOMPARE(window.findChild<QLabel *>(QStringLiteral("notice"))->text(),
             QStringLiteral("mark good → no reply"));
    // With no daemon there are no modes either, so there is nothing to save
    // into and the window says that rather than sending into the void.
    window.saveToMode();
    QCOMPARE(window.findChild<QLabel *>(QStringLiteral("notice"))->text(),
             QStringLiteral("No modes to save into yet."));

    TestDaemon daemon(m_tree->root(), m_work.path());
    QVERIFY(daemon.waitUntilListening());
    TrayClient live(socketPath());
    TrayWindow second(live);
    auto *connection = second.findChild<QLabel *>(QStringLiteral("connection"));
    QVERIFY(connection->text().contains(QStringLiteral("not answering")));
    live.poll();
    QVERIFY(connection->text().contains(QStringLiteral("Connected")));
    QVERIFY(connection->text().contains(live.socketPath()));
    QVERIFY(second.findChild<QSlider *>(QStringLiteral("pl1_slider"))->isEnabled());
}

void TrayWindowTest::theReplyLineTakesNoRoomUntilThereIsAReply()
{
    TestDaemon daemon(m_tree->root(), m_work.path());
    QVERIFY(daemon.waitUntilListening());
    TrayClient client(socketPath());
    TrayWindow window(client);
    client.poll();
    window.show();
    QTest::qWait(50);

    auto *notice = window.findChild<QLabel *>(QStringLiteral("notice"));
    QVERIFY(notice);
    // Nothing said yet, so nothing takes room: a reply line kept visible from
    // the start is a strip of empty space along the bottom of every tab.
    QVERIFY(!notice->isVisible());
    const int before = window.height();

    window.markGood();
    QTest::qWait(50);
    QVERIFY(notice->isVisible());
    QVERIFY2(window.height() > before, "the frame is fixed, so it has to make the room");
    QVERIFY(notice->text().contains(QStringLiteral("mark good")));
}

void TrayWindowTest::theTrayBinaryRunsAgainstADaemon()
{
    const QString binary = QDir::cleanPath(QCoreApplication::applicationDirPath()
                                           + QStringLiteral("/../ombra-tray"));
    QVERIFY2(QFile::exists(binary), qPrintable(binary));

    auto daemon = std::make_unique<TestDaemon>(m_tree->root(), m_work.path());
    QVERIFY(daemon->waitUntilListening());

    QProcess tray;
    const Reaped reaped(tray);
    tray.setProcessEnvironment(isolatedEnvironment());
    tray.start(binary, { QStringLiteral("--socket"), socketPath(),
                         QStringLiteral("--interval-ms"), QStringLiteral("40"),
                         QStringLiteral("--quit-after-ms"), QStringLiteral("2000") });
    QVERIFY(tray.waitForStarted(10000));

    // Long enough to have polled, so the menu and the icon are built from a
    // real status reply rather than from the initial blank state.
    QTest::qWait(600);
    QCOMPARE(tray.state(), QProcess::Running);
    // And it survives the daemon going away underneath it.
    daemon.reset();
    QVERIFY(tray.waitForFinished(20000));
    QCOMPARE(tray.exitStatus(), QProcess::NormalExit);
    QCOMPARE(tray.exitCode(), 0);

    QProcess help;
    help.setProcessEnvironment(isolatedEnvironment());
    help.start(binary, { QStringLiteral("--help") });
    QVERIFY(help.waitForFinished(10000));
    QCOMPARE(help.exitCode(), 0);
    QVERIFY(!help.readAllStandardOutput().isEmpty());

    QProcess version;
    version.setProcessEnvironment(isolatedEnvironment());
    version.start(binary, { QStringLiteral("--version") });
    QVERIFY(version.waitForFinished(10000));
    QCOMPARE(version.exitCode(), 0);

    QProcess nonsense;
    nonsense.setProcessEnvironment(isolatedEnvironment());
    nonsense.start(binary, { QStringLiteral("--nonsense") });
    QVERIFY(nonsense.waitForFinished(10000));
    QCOMPARE(nonsense.exitCode(), 1);
}

void TrayWindowTest::theTrayWaitsForALateTrayHost()
{
    const QString binary = QDir::cleanPath(QCoreApplication::applicationDirPath()
                                           + QStringLiteral("/../ombra-tray"));
    QVERIFY2(QFile::exists(binary), qPrintable(binary));
    TestDaemon daemon(m_tree->root(), m_work.path());
    QVERIFY(daemon.waitUntilListening());

    // A tray host that turns up late, which is the ordinary case at login: the
    // panel registers over the session bus a second or two after an autostarted
    // process. Without the first hook this branch cannot be reached at all —
    // on the offscreen platform the availability check answers true at once.
    QProcessEnvironment late = isolatedEnvironment();
    late.insert(QStringLiteral("OMBRA_TEST_TRAY_ABSENT_MS"), QStringLiteral("1000"));
    late.insert(QStringLiteral("OMBRA_TEST_TRAY_TIMEOUT_MS"), QStringLiteral("5000"));

    QProcess appears;
    const Reaped reapedAppears(appears);
    appears.setProcessEnvironment(late);
    appears.start(binary, { QStringLiteral("--socket"), socketPath(),
                            QStringLiteral("--interval-ms"), QStringLiteral("40"),
                            QStringLiteral("--quit-after-ms"), QStringLiteral("3000") });
    QVERIFY(appears.waitForStarted(10000));
    QVERIFY(appears.waitForFinished(20000));
    QCOMPARE(appears.exitCode(), 0);
    // It waited rather than deciding at the first tick, so nothing was said
    // about a missing tray.
    QVERIFY(!QString::fromUtf8(appears.readAllStandardError())
                     .contains(QStringLiteral("no system tray")));

    // And the other branch: a host that never arrives. The console opens, and
    // the process ends when it is closed — otherwise it would live on with no
    // window and no icon, reachable only by name.
    QProcessEnvironment never = isolatedEnvironment();
    never.insert(QStringLiteral("OMBRA_TEST_TRAY_ABSENT_MS"), QStringLiteral("100000"));
    never.insert(QStringLiteral("OMBRA_TEST_TRAY_TIMEOUT_MS"), QStringLiteral("1000"));

    QProcess absent;
    const Reaped reapedAbsent(absent);
    absent.setProcessEnvironment(never);
    absent.start(binary, { QStringLiteral("--socket"), socketPath(),
                           QStringLiteral("--interval-ms"), QStringLiteral("40"),
                           QStringLiteral("--quit-after-ms"), QStringLiteral("4000") });
    QVERIFY(absent.waitForStarted(10000));
    QVERIFY(absent.waitForFinished(20000));
    QVERIFY(QString::fromUtf8(absent.readAllStandardError())
                    .contains(QStringLiteral("no system tray")));
}

void TrayWindowTest::theWindowKeepsOneSizeForEveryTab()
{
    TestDaemon daemon(m_tree->root(), m_work.path());
    QVERIFY(daemon.waitUntilListening());
    TrayClient client(socketPath());
    TrayWindow window(client);
    client.poll();
    window.show();
    QTest::qWait(50);

    const QSize sized = window.size();
    auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("tabs"));
    QVERIFY(tabs);
    // Every tab fits, so none of them opens clipped and switching moves
    // nothing. The frame is fixed there: what dragging it could produce is
    // either empty space or a scrollbar over content that had room.
    for (int i = 0; i < tabs->count(); ++i) {
        tabs->setCurrentIndex(i);
        QTest::qWait(20);
        QCOMPARE(window.size(), sized);
    }
    QCOMPARE(window.minimumSize(), sized);
    QCOMPARE(window.maximumSize(), sized);

    // Except for the documentation pictures, which need each tab photographed
    // at its own height.
    window.fitToCurrentTab();
    QVERIFY(window.maximumSize() != sized);
}

void TrayWindowTest::theTrayBinaryWritesTheDocumentationScreenshots()
{
    const QString binary = QDir::cleanPath(QCoreApplication::applicationDirPath()
                                           + QStringLiteral("/../ombra-tray"));
    QVERIFY2(QFile::exists(binary), qPrintable(binary));

    TestDaemon daemon(m_tree->root(), m_work.path());
    QVERIFY(daemon.waitUntilListening());

    QTemporaryDir target;
    QProcess shot;
    const Reaped reapedShot(shot);
    shot.setProcessEnvironment(isolatedEnvironment());
    shot.start(binary, { QStringLiteral("--socket"), socketPath(),
                         QStringLiteral("--screenshot"), target.path() });
    QVERIFY(shot.waitForFinished(30000));
    QCOMPARE(shot.exitCode(), 0);

    // The menu and one picture per tab, all readable as images: a documentation
    // command that writes an empty file is worse than one that fails.
    QStringList expected { QStringLiteral("tray-menu.png") };
    {
        TrayClient client(socketPath());
        const TrayWindow window(client);
        const auto *tabs = window.findChild<QTabWidget *>();
        QVERIFY(tabs);
        QVERIFY(tabs->count() > 1);
        for (int i = 0; i < tabs->count(); ++i)
            expected.append(QStringLiteral("console-%1.png").arg(tabs->tabText(i).toLower()));
    }
    for (const QString &name : std::as_const(expected)) {
        const QImage picture(target.filePath(name));
        QVERIFY2(!picture.isNull(), qPrintable(name));
        QVERIFY2(picture.width() > 100 && picture.height() > 100, qPrintable(name));
    }

    // A directory that is not there is a failure with a reason, not a silent
    // success: the pictures would simply be missing from the next commit.
    QProcess nowhere;
    const Reaped reapedNowhere(nowhere);
    nowhere.setProcessEnvironment(isolatedEnvironment());
    nowhere.start(binary, { QStringLiteral("--socket"), socketPath(),
                            QStringLiteral("--screenshot"),
                            target.filePath(QStringLiteral("absent")) });
    QVERIFY(nowhere.waitForFinished(30000));
    QCOMPARE(nowhere.exitCode(), 1);
}

QTEST_MAIN(TrayWindowTest)
#include "tray_window_test.moc"

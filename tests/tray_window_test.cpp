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
                               + QStringLiteral("/../sombrad"));
    }

private:
    QString m_work;
    QProcess m_process;
};

// The environment a spawned sombra-tray gets.
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
                       QStringLiteral("unix:path=/nonexistent/sombra-test-has-no-session-bus"));
    // A runtime directory of the suite's own, because the single-instance lock
    // lives in one. Sharing the session's would mean the tests answer to the
    // console the person is running, and that the person's console answers to
    // them.
    static QTemporaryDir runtime;
    if (runtime.isValid())
        environment.insert(QStringLiteral("XDG_RUNTIME_DIR"), runtime.path());
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
    void theBacklightIsFourControlsInPerCent();
    void theAutostartSwitchOwnsItsOwnEntry();
    void theConsoleAppearanceIsAChoiceOfThree();
    void oneResetCoversEverySettingThereIs();
    void repaintingFromAStatusSendsNothing();
    void saysSoWhenTheDaemonIsUnreachable();
    void theWindowKeepsOneSizeForEveryTab();
    void oneLineCarriesTheStateAndTheReplies();
    void everyControlThatSendsACommandExplainsItself();
    void everyChoiceInAListExplainsItself();
    void theBaselineIsAControlOfItsOwn();
    void theTrayBinaryRunsAgainstADaemon();
    void aSecondConsoleOpensTheFirstOneInstead();
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
    QVERIFY(window.statusText()
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

    // A judgement made by ear is worthless unless it can be traced back to the
    // numbers that produced it, which is what stamping the sample buys.
    window.findChild<QPushButton *>(QStringLiteral("mark_good"))->click();
    QCOMPARE(window.statusText(), QStringLiteral("mark good → ok"));
    window.findChild<QPushButton *>(QStringLiteral("mark_too_loud"))->click();
    QCOMPARE(window.statusText(), QStringLiteral("mark too-loud → ok"));
    window.findChild<QPushButton *>(QStringLiteral("mark_too_slow"))->click();
    QCOMPARE(window.statusText(), QStringLiteral("mark too-slow → ok"));
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

void TrayWindowTest::theBacklightIsFourControlsInPerCent()
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

    // The band is what the window says out loud, in per cent, and it is bounded
    // from both sides: the floor and the ceiling are one mechanism seen twice.
    auto *ceiling = window.findChild<QSpinBox *>(QStringLiteral("ceiling_box"));
    QVERIFY(ceiling);
    QCOMPARE(ceiling->value(), kBrightnessDefaultCeilingPct);
    ceiling->setValue(70);
    QTRY_COMPARE(client.status().brightnessCeilingPct, 70);
    QVERIFY(loadBrightnessSettings(configPath(), &saved));
    QCOMPARE(saved.ceilingPct, 70);

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
    QVERIFY(window.statusText()
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
    QVERIFY(window.statusText()
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

    window.show();
    QTest::qWait(50);

    appearance->setCurrentIndex(appearance->findData(QStringLiteral("dark")));
    QCOMPARE(theme::preference(), theme::Preference::Dark);
    QCOMPARE(theme::schemeOf(QApplication::palette()), theme::Scheme::Dark);
    // The window stays exactly where it was: the choice repaints widgets and
    // nothing about the window itself is rebuilt.
    QVERIFY(window.isVisible());

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

    // Short on the line and specific behind it: the socket and the group are
    // what a diagnosis needs, and "not answering" is what a glance needs.
    auto *banner = window.findChild<QLabel *>(QStringLiteral("status"));
    QVERIFY(!banner->isHidden());
    QVERIFY(window.statusText().contains(QStringLiteral("not answering")));
    QVERIFY(!window.statusText().contains(client.socketPath()));
    QVERIFY(banner->toolTip().contains(client.socketPath()));
    QVERIFY(banner->toolTip().contains(QString::fromLatin1(kControlSocketGroup)));

    // Nothing that would send a command is usable, so a slider parked at its
    // minimum cannot be read as a setpoint the daemon is holding.
    QVERIFY(!window.findChild<QSlider *>(QStringLiteral("pl1_slider"))->isEnabled());
    QVERIFY(!window.findChild<QComboBox *>(QStringLiteral("mode"))->isEnabled());
    QVERIFY(!window.findChild<QCheckBox *>(QStringLiteral("auto_brightness"))->isEnabled());

    // A command that goes nowhere says so rather than looking like it worked.
    window.markGood();
    QCOMPARE(window.statusText(),
             QStringLiteral("mark good → no reply"));
    // With no daemon there are no modes either, so there is nothing to save
    // into and the window says that rather than sending into the void.
    window.saveToMode();
    QCOMPARE(window.statusText(),
             QStringLiteral("No modes to save into yet."));

    TestDaemon daemon(m_tree->root(), m_work.path());
    QVERIFY(daemon.waitUntilListening());
    TrayClient live(socketPath());
    TrayWindow second(live);
    auto *connection = second.findChild<QLabel *>(QStringLiteral("status"));
    QVERIFY(second.statusText().contains(QStringLiteral("not answering")));
    live.poll();
    QVERIFY(second.statusText().contains(QStringLiteral("Connected")));
    QVERIFY(connection->toolTip().contains(live.socketPath()));
    QVERIFY(second.findChild<QSlider *>(QStringLiteral("pl1_slider"))->isEnabled());
}

void TrayWindowTest::oneLineCarriesTheStateAndTheReplies()
{
    auto daemon = std::make_unique<TestDaemon>(m_tree->root(), m_work.path());
    QVERIFY(daemon->waitUntilListening());
    TrayClient client(socketPath());
    TrayWindow window(client);
    client.poll();
    window.show();
    QTest::qWait(50);

    auto *status = window.findChild<QLabel *>(QStringLiteral("status"));
    QVERIFY(status);
    const QString connected = window.statusText();
    QVERIFY(connected.contains(QStringLiteral("Connected")));
    const int height = window.height();

    // An accepted command shows its reply and gives the line back by itself. The
    // window does not change size for it: one line carries both, which is why
    // there is no strip of empty space waiting for a reply that never came.
    window.markGood();
    QVERIFY(window.statusText().contains(QStringLiteral("mark good")));
    QCOMPARE(window.height(), height);

    // A message longer than the window is elided rather than wrapped: a second
    // line would come out of the tab above it. The whole of it is in the tooltip,
    // and `statusText()` is what anything asking what the window said looks at.
    const int oneLine = status->height();
    window.report(QString(400, QLatin1Char('x')), false);
    QCOMPARE(status->height(), oneLine);
    QCOMPARE(window.height(), height);
    QCOMPARE(status->toolTip(), window.statusText());
    QVERIFY2(status->text().size() < window.statusText().size(),
             "the line holds what fits and the whole of it is behind it");
    QTRY_COMPARE_WITH_TIMEOUT(window.statusText(), connected, 8000);

    // A command that goes nowhere is a refusal, and a refusal stays: it is the
    // answer to something the person did, and a few seconds is not long enough
    // to be sure it was read.
    daemon.reset();
    client.poll();
    window.markGood();
    const QString refusal = window.statusText();
    QVERIFY2(refusal.contains(QStringLiteral("no reply")), qPrintable(refusal));
    QTest::qWait(300);
    QCOMPARE(window.statusText(), refusal);

    // Not even a change of connection state wipes it. The next command does.
    client.poll();
    QCOMPARE(window.statusText(), refusal);
    window.markTooLoud();
    QVERIFY(window.statusText().contains(QStringLiteral("mark too-loud")));
}

void TrayWindowTest::everyControlThatSendsACommandExplainsItself()
{
    TestDaemon daemon(m_tree->root(), m_work.path());
    QVERIFY(daemon.waitUntilListening());
    TrayClient client(socketPath());
    TrayWindow window(client);
    client.poll();

    // The explanations used to be paragraphs in the window, which cost height the
    // controls needed and had to be translated twice over. They live on the
    // controls now, so this is the test that keeps one from being forgotten.
    const QStringList named = { QStringLiteral("mode"),      QStringLiteral("mode_at_boot"),
                                QStringLiteral("pl1"),       QStringLiteral("pl2"),
                                QStringLiteral("max_perf_pct"), QStringLiteral("gt_max_mhz"),
                                QStringLiteral("epp"),       QStringLiteral("profile"),
                                QStringLiteral("turbo"),     QStringLiteral("hwp_boost"),
                                QStringLiteral("save"),      QStringLiteral("save_target"),
                                QStringLiteral("revert"),    QStringLiteral("reset_all"),
                                QStringLiteral("auto_brightness"), QStringLiteral("level"),
                                QStringLiteral("base"),      QStringLiteral("response"),
                                QStringLiteral("floor"),     QStringLiteral("ceiling"),
                                QStringLiteral("appearance"), QStringLiteral("autostart") };
    for (const QString &name : named) {
        auto *control = window.findChild<QWidget *>(name);
        QVERIFY2(control, qPrintable(name));
        QVERIFY2(!control->toolTip().isEmpty(), qPrintable(name));
    }

    // And the paragraphs are gone: two labels that said what a tooltip says now.
    const QList<QLabel *> labels = window.findChildren<QLabel *>();
    for (const QLabel *label : labels) {
        QVERIFY2(!label->text().contains(QStringLiteral("next login")),
                 qPrintable(label->text()));
        QVERIFY2(!label->text().contains(QStringLiteral("compiled into the daemon")),
                 qPrintable(label->text()));
    }
}

void TrayWindowTest::everyChoiceInAListExplainsItself()
{
    TestDaemon daemon(m_tree->root(), m_work.path());
    QVERIFY(daemon.waitUntilListening());
    TrayClient client(socketPath());
    TrayWindow window(client);
    client.poll();
    QTest::qWait(50);

    // A list of three names says nothing to somebody seeing it for the first
    // time, so every entry carries what it is: the modes their own numbers, the
    // energy preferences and the fan curves what they mean.
    auto *mode = window.findChild<QComboBox *>(QStringLiteral("mode"));
    QCOMPARE(mode->count(), 3);
    for (int i = 0; i < mode->count(); ++i) {
        const QString hint = mode->itemData(i, Qt::ToolTipRole).toString();
        QVERIFY2(!hint.isEmpty(), qPrintable(mode->itemText(i)));
        // Built from the definition rather than written down, so a mode that is
        // tuned takes its description with it.
        QVERIFY2(hint.contains(QStringLiteral(" W")), qPrintable(hint));
    }
    QVERIFY(mode->itemData(0, Qt::ToolTipRole).toString().contains(QStringLiteral("coolest")));
    QVERIFY(mode->itemData(2, Qt::ToolTipRole).toString().contains(QStringLiteral("widest")));

    // The saved-into list is the same three modes and says the same things.
    auto *target = window.findChild<QComboBox *>(QStringLiteral("save_target"));
    QCOMPARE(target->itemData(0, Qt::ToolTipRole), mode->itemData(0, Qt::ToolTipRole));

    for (const QString &name : { QStringLiteral("epp"), QStringLiteral("profile") }) {
        auto *list = window.findChild<QComboBox *>(name);
        QVERIFY(list);
        for (int i = 0; i < list->count(); ++i) {
            QVERIFY2(!list->itemData(i, Qt::ToolTipRole).toString().isEmpty(),
                     qPrintable(name + QLatin1Char('/') + list->itemText(i)));
        }
    }
}

void TrayWindowTest::theBaselineIsAControlOfItsOwn()
{
    TestDaemon daemon(m_tree->root(), m_work.path());
    QVERIFY(daemon.waitUntilListening());
    TrayClient client(socketPath());
    TrayWindow window(client);
    client.poll();

    auto *base = window.findChild<QSpinBox *>(QStringLiteral("base_box"));
    QVERIFY(base);
    // Nothing for it to do while the automation is off: the baseline is what the
    // automation works around, and moving a number nothing acts on is a control
    // that lies.
    QVERIFY(!base->isEnabled());

    window.findChild<QCheckBox *>(QStringLiteral("auto_brightness"))->setChecked(true);
    client.poll();
    QVERIFY(base->isEnabled());

    // Set outright, and the level follows from it and the light of the moment —
    // which is the direction the window could not offer before.
    base->setValue(64);
    // No waiting for a tick: the level and the baseline are read when the status
    // is asked for, so the answer to a command is in the very next reply.
    QCOMPARE(client.status().brightnessBase, 64.0);
    const double level = client.status().brightness;
    QVERIFY(level > TrayClient::kNoReading);

    // The floor binds it, like everything else about the backlight.
    QVERIFY(client.send(QStringLiteral("config set brightness floor_pct=40"))
                    .startsWith(QLatin1String("ok")));
    base->setValue(5);
    QCOMPARE(client.status().brightnessBase, 40.0);
}

void TrayWindowTest::theTrayBinaryRunsAgainstADaemon()
{
    const QString binary = QDir::cleanPath(QCoreApplication::applicationDirPath()
                                           + QStringLiteral("/../sombra-tray"));
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

void TrayWindowTest::aSecondConsoleOpensTheFirstOneInstead()
{
    const QString binary = QDir::cleanPath(QCoreApplication::applicationDirPath()
                                           + QStringLiteral("/../sombra-tray"));
    QVERIFY2(QFile::exists(binary), qPrintable(binary));

    TestDaemon daemon(m_tree->root(), m_work.path());
    QVERIFY(daemon.waitUntilListening());

    QProcess first;
    const Reaped reaped(first);
    first.setProcessEnvironment(isolatedEnvironment());
    first.start(binary, { QStringLiteral("--socket"), socketPath(),
                          QStringLiteral("--interval-ms"), QStringLiteral("40"),
                          QStringLiteral("--quit-after-ms"), QStringLiteral("4000") });
    QVERIFY(first.waitForStarted(10000));
    QTest::qWait(600);
    QCOMPARE(first.state(), QProcess::Running);

    // Two of them would put two icons in the panel and two pollers on the
    // daemon, and nothing would say which icon was which.
    QProcess second;
    second.setProcessEnvironment(isolatedEnvironment());
    second.setProcessChannelMode(QProcess::MergedChannels);
    second.start(binary, { QStringLiteral("--socket"), socketPath() });
    QVERIFY(second.waitForFinished(10000));
    QCOMPARE(second.exitStatus(), QProcess::NormalExit);
    QCOMPARE(second.exitCode(), 0);
    QVERIFY2(second.readAll().contains("already running"), "it has to say why it left");

    // And the one that was there is untouched.
    QCOMPARE(first.state(), QProcess::Running);
    QVERIFY(first.waitForFinished(20000));
    QCOMPARE(first.exitCode(), 0);
}

void TrayWindowTest::theTrayWaitsForALateTrayHost()
{
    const QString binary = QDir::cleanPath(QCoreApplication::applicationDirPath()
                                           + QStringLiteral("/../sombra-tray"));
    QVERIFY2(QFile::exists(binary), qPrintable(binary));
    TestDaemon daemon(m_tree->root(), m_work.path());
    QVERIFY(daemon.waitUntilListening());

    // A tray host that turns up late, which is the ordinary case at login: the
    // panel registers over the session bus a second or two after an autostarted
    // process. Without the first hook this branch cannot be reached at all —
    // on the offscreen platform the availability check answers true at once.
    QProcessEnvironment late = isolatedEnvironment();
    late.insert(QStringLiteral("SOMBRA_TEST_TRAY_ABSENT_MS"), QStringLiteral("1000"));
    late.insert(QStringLiteral("SOMBRA_TEST_TRAY_TIMEOUT_MS"), QStringLiteral("5000"));

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
    never.insert(QStringLiteral("SOMBRA_TEST_TRAY_ABSENT_MS"), QStringLiteral("100000"));
    never.insert(QStringLiteral("SOMBRA_TEST_TRAY_TIMEOUT_MS"), QStringLiteral("1000"));

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
                                           + QStringLiteral("/../sombra-tray"));
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

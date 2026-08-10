// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "tray_window.h"

#include "theme.h"

#include "autostart.h"
#include "brightness_map.h"
#include "control_socket.h"
#include "setpoints.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace {

// Room for a vertical scrollbar beside the widest page, in device-independent
// pixels, so that a page is never squeezed sideways to make room for one.
constexpr int kSideChromePx = 48;
// Deliberately larger than any tab: the size the window is given before it is
// asked how much room it actually wants.
constexpr int kMeasureSizePx = 1600;
// The fan selection that is not a curve. Numbered past the Profile enum so the
// two can share one combo box without colliding.
constexpr int kFanMaximumItem = 100;

// A reading the daemon could not take arrives as the sentinel. Rendering it as
// a dash rather than as a number is the whole point of having a sentinel: a
// zero here would be indistinguishable from a stopped fan.
QString formatted(double value, const QString &unit, int decimals)
{
    if (value <= TrayClient::kNoReading)
        return QStringLiteral("—");
    const QString number = QString::number(value, 'f', decimals);
    return unit.isEmpty() ? number : QStringLiteral("%1 %2").arg(number, unit);
}

QLabel *addReading(QFormLayout *form, const QString &caption, const QString &name)
{
    auto *label = new QLabel(QStringLiteral("—"), form->parentWidget());
    label->setObjectName(name);
    form->addRow(caption, label);
    return label;
}

QLabel *addIndicator(QVBoxLayout *layout, const QString &text, const QString &name)
{
    auto *label = new QLabel(text, layout->parentWidget());
    label->setObjectName(name);
    label->setVisible(false);
    label->setWordWrap(true);
    layout->addWidget(label);
    return label;
}

} // namespace

// A slider with a field beside it, moving together.
//
// The field is not decoration: a slider cannot be landed on a particular
// number, and these numbers are compared between sessions and typed out of a
// telemetry analysis. Both halves carry one value, and the pair reports two
// different things — that it moved, and that it has settled — because one is
// worth applying and the other is worth writing to disk.
class Tuner : public QWidget {
    Q_OBJECT

public:
    Tuner(int minimum, int maximum, int step, const QString &suffix, QWidget *parent)
        : QWidget(parent)
    {
        auto *layout = new QHBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);

        m_slider = new QSlider(Qt::Horizontal, this);
        m_slider->setRange(minimum, maximum);
        m_slider->setSingleStep(step);
        m_slider->setPageStep(step * 5);
        m_box = new QSpinBox(this);
        m_box->setRange(minimum, maximum);
        m_box->setSingleStep(step);
        m_box->setSuffix(suffix);
        // Otherwise every keystroke inside the field is a value, and typing
        // "20" over "5" would pass through 2 on the way.
        m_box->setKeyboardTracking(false);
        layout->addWidget(m_slider, 1);
        layout->addWidget(m_box);

        connect(m_slider, &QSlider::valueChanged, this, [this](int value) {
            if (m_box->value() != value) {
                m_syncing = true;
                m_box->setValue(value);
                m_syncing = false;
            }
            emit changed(value);
        });
        // With keyboard tracking off the field reports a value only once the
        // edit is over, so a value arriving from the field is a settled one by
        // construction. The exception is the field following the slider beside
        // it, which is still mid-gesture.
        connect(m_box, &QSpinBox::valueChanged, this, [this](int value) {
            if (m_syncing)
                return;
            if (m_slider->value() != value)
                m_slider->setValue(value);
            emit settled(value);
        });
        connect(m_slider, &QSlider::sliderReleased, this, [this]() { emit settled(value()); });
    }

    int value() const { return m_slider->value(); }
    void setValue(int value)
    {
        m_syncing = true;
        m_box->setValue(value);
        m_syncing = false;
        m_slider->setValue(value);
    }
    void nameParts(const QString &name)
    {
        setObjectName(name);
        m_slider->setObjectName(name + QStringLiteral("_slider"));
        m_box->setObjectName(name + QStringLiteral("_box"));
    }

signals:
    void changed(int value);
    void settled(int value);

private:
    QSlider *m_slider = nullptr;
    QSpinBox *m_box = nullptr;
    // Set while the two halves are brought into step, so that the field
    // following the slider does not read as an edit of the field.
    bool m_syncing = false;
};

class TrayWindow::Ui {
public:
    QTabWidget *tabs = nullptr;
    QList<QWidget *> pages;
    // The groups that command the daemon, as opposed to the ones that only
    // report. Disabled while the daemon is not answering, so that a control
    // sitting at its minimum cannot be read as a setting the daemon holds.
    QList<QWidget *> commands;

    QLabel *connection = nullptr;
    QLabel *notice = nullptr;

    QLabel *packageC = nullptr;
    QLabel *boardC = nullptr;
    QLabel *nvmeC = nullptr;
    QLabel *fanRpm = nullptr;
    QLabel *packageW = nullptr;
    QLabel *coreW = nullptr;
    QLabel *uncoreW = nullptr;
    QLabel *cpuMhz = nullptr;
    QLabel *gpuMhz = nullptr;
    QLabel *load1 = nullptr;
    QLabel *applied = nullptr;
    QLabel *powerSource = nullptr;
    QLabel *live = nullptr;
    QLabel *guard = nullptr;
    QLabel *mismatch = nullptr;
    QLabel *sensors = nullptr;

    QComboBox *mode = nullptr;
    QCheckBox *atBoot = nullptr;
    Tuner *pl1 = nullptr;
    Tuner *pl2 = nullptr;
    Tuner *maxPerfPct = nullptr;
    Tuner *gtMaxMhz = nullptr;
    QComboBox *epp = nullptr;
    QComboBox *profile = nullptr;
    QCheckBox *turbo = nullptr;
    QCheckBox *hwpBoost = nullptr;
    QLabel *modeState = nullptr;
    QComboBox *saveTarget = nullptr;
    QLabel *fanState = nullptr;

    QCheckBox *autoBrightness = nullptr;
    Tuner *level = nullptr;
    Tuner *response = nullptr;
    Tuner *floor = nullptr;
    QLabel *backlightState = nullptr;

    // The two controls that are about the console rather than about the machine.
    // Never greyed out with the others: neither the tray starting with the
    // session nor what this window looks like depends on the daemon.
    QCheckBox *autostart = nullptr;
    QComboBox *appearance = nullptr;
};

TrayWindow::TrayWindow(TrayClient &client, QWidget *parent)
    : QWidget(parent)
    , m_ui(new Ui)
    , m_client(client)
{
    setWindowTitle(QStringLiteral("Ombra"));
    build();

    connect(&m_client, &TrayClient::statusChanged, this, &TrayWindow::applyStatus);
    connect(&m_client, &TrayClient::connectionChanged, this, &TrayWindow::onConnectionChanged);
    onConnectionChanged(m_client.connected());
}

TrayWindow::~TrayWindow()
{
    delete m_ui;
}

// Sized here rather than in the constructor, and once.
//
// A newly built window cannot say how wide it needs to be: the readings are
// still dashes, the state lines are still empty, and the platform style and its
// font have not been applied yet, so every size hint is far too small. By the
// time the window is first shown it has been polled at, so the labels carry the
// text that actually decides the width.
//
// The size is then fixed. The window already fits its largest tab, which is the
// only size that suits every tab at once, so what dragging the frame can
// produce is either empty space or a scrollbar over content that had room.
void TrayWindow::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    if (m_sized)
        return;
    m_sized = true;
    fitToLargestTab();
}

void TrayWindow::fitToLargestTab()
{
    int width = 0;
    for (const QWidget *page : std::as_const(m_ui->pages)) {
        // Both hints, because a word-wrapped label reports a narrow preferred
        // width and a tall height for it — take the preferred size alone and
        // one such label drags the whole console narrow.
        width = std::max({ width, page->sizeHint().width(), page->minimumSizeHint().width() });
    }
    width += kSideChromePx;

    // The height is measured rather than guessed at. What surrounds a page — the
    // tab bar, the connection banner, the reply line, every margin — is asked to
    // report itself by giving the window more room than any page needs and then
    // reading how much of it the viewport got. A constant in its place is a
    // constant that is wrong by a strip of empty space under the tallest tab.
    const int restore = m_ui->tabs->currentIndex();
    resize(width, kMeasureSizePx);
    int height = 0;
    for (int i = 0; i < m_ui->tabs->count(); ++i) {
        m_ui->tabs->setCurrentIndex(i);
        auto *area = qobject_cast<QScrollArea *>(m_ui->tabs->widget(i));
        if (!area || !area->widget())
            continue;
        if (QLayout *pageLayout = area->widget()->layout())
            pageLayout->activate();
        const int chrome = kMeasureSizePx - area->viewport()->height();
        height = std::max(height, area->widget()->sizeHint().height() + chrome);
    }
    m_ui->tabs->setCurrentIndex(restore);

    if (const QScreen *screen = QGuiApplication::primaryScreen()) {
        const QRect available = screen->availableGeometry();
        // A display too short for the largest tab is the one case where the
        // scroll area earns its keep, and the window is then as tall as the
        // screen allows rather than taller.
        width = std::min(width, available.width());
        height = std::min(height, available.height() * 9 / 10);
    }
    setFixedSize(width, height);
}

void TrayWindow::fitToCurrentTab()
{
    // For the documentation pictures only, which is why it undoes the fixed
    // size the window otherwise keeps.
    //
    // Only the height is fitted. The width is the one the console actually has
    // in use — one width for every tab — and a picture at some other width
    // would document a window nobody sees.
    const int inUseWidth = width();
    setMinimumSize(0, 0);
    setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    auto *area = qobject_cast<QScrollArea *>(m_ui->tabs->currentWidget());
    if (!area || !area->widget())
        return;

    // Height first, measurement second: a page squeezed into a short viewport
    // reports the height it was squeezed into.
    resize(inUseWidth, kMeasureSizePx);
    if (QLayout *pageLayout = area->widget()->layout())
        pageLayout->activate();
    const int wanted = area->widget()->sizeHint().height();
    resize(inUseWidth, height() + wanted - area->viewport()->height());
}

void TrayWindow::build()
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    // Outside the tabs on purpose: whether the daemon is answering is the first
    // thing to know, and it must not be something to switch tabs for.
    m_ui->connection = new QLabel(this);
    m_ui->connection->setObjectName(QStringLiteral("connection"));
    m_ui->connection->setWordWrap(true);
    m_ui->connection->setContentsMargins(8, 6, 8, 6);
    outer->addWidget(m_ui->connection);

    m_ui->tabs = new QTabWidget(this);
    m_ui->tabs->setObjectName(QStringLiteral("tabs"));
    outer->addWidget(m_ui->tabs, 1);

    const auto addPage = [this](const QString &title) {
        auto *area = new QScrollArea(m_ui->tabs);
        area->setWidgetResizable(true);
        area->setFrameShape(QFrame::NoFrame);
        auto *page = new QWidget(area);
        auto *layout = new QVBoxLayout(page);
        area->setWidget(page);
        m_ui->pages.append(page);
        m_ui->tabs->addTab(area, title);
        return layout;
    };

    // ---- Now: what only reports, and the verdicts about it ----------------
    auto *nowLayout = addPage(QStringLiteral("Now"));
    QWidget *const nowPage = m_ui->pages.constLast();

    auto *readings = new QGroupBox(QStringLiteral("Readings"), nowPage);
    auto *readingsForm = new QFormLayout(readings);
    m_ui->packageC = addReading(readingsForm, QStringLiteral("Package:"),
                                QStringLiteral("pkg_c"));
    m_ui->boardC = addReading(readingsForm, QStringLiteral("Board:"), QStringLiteral("board_c"));
    m_ui->nvmeC = addReading(readingsForm, QStringLiteral("NVMe:"), QStringLiteral("nvme_c"));
    m_ui->fanRpm = addReading(readingsForm, QStringLiteral("Fan:"), QStringLiteral("fan_rpm"));
    m_ui->packageW = addReading(readingsForm, QStringLiteral("Package power:"),
                                QStringLiteral("pkg_w"));
    // The core/uncore split is what tells CPU heat from iGPU heat.
    m_ui->coreW = addReading(readingsForm, QStringLiteral("Core power:"),
                             QStringLiteral("core_w"));
    m_ui->uncoreW = addReading(readingsForm, QStringLiteral("Uncore power:"),
                               QStringLiteral("uncore_w"));
    m_ui->cpuMhz = addReading(readingsForm, QStringLiteral("CPU frequency:"),
                              QStringLiteral("cpu_mhz"));
    m_ui->gpuMhz = addReading(readingsForm, QStringLiteral("iGPU frequency:"),
                              QStringLiteral("gpu_mhz"));
    m_ui->load1 = addReading(readingsForm, QStringLiteral("Load average:"),
                             QStringLiteral("load1"));
    nowLayout->addWidget(readings);

    auto *state = new QGroupBox(QStringLiteral("State"), nowPage);
    auto *stateLayout = new QVBoxLayout(state);
    m_ui->applied = new QLabel(QStringLiteral("—"), state);
    m_ui->applied->setObjectName(QStringLiteral("applied"));
    m_ui->applied->setWordWrap(true);
    stateLayout->addWidget(m_ui->applied);
    m_ui->powerSource = new QLabel(QStringLiteral("—"), state);
    m_ui->powerSource->setObjectName(QStringLiteral("power_source"));
    stateLayout->addWidget(m_ui->powerSource);
    m_ui->live = addIndicator(stateLayout,
                              QStringLiteral("The controls have been moved: what is running is "
                                             "not one of the saved modes."),
                              QStringLiteral("live_tuning"));
    // The guard is a safety interlock, so its state has to be visible: while it
    // clamps, the controls say what was asked for and not what is applied.
    m_ui->guard = addIndicator(stateLayout,
                               QStringLiteral("Emergency thermal guard engaged: the package is "
                                              "near the hardware cutout and power is clamped."),
                               QStringLiteral("guard"));
    m_ui->mismatch = addIndicator(stateLayout,
                                  QStringLiteral("A control read back a value the daemon did not "
                                                 "write: another process is writing them too."),
                                  QStringLiteral("mismatch"));
    m_ui->sensors = addIndicator(stateLayout, QStringLiteral("Some sensors could not be read."),
                                 QStringLiteral("sensors"));
    nowLayout->addWidget(state);

    // The verdicts live beside the readings they are about: a judgement is made
    // while looking at the numbers, and it is stamped onto the sample carrying
    // them.
    auto *verdict = new QGroupBox(QStringLiteral("How does it sound"), nowPage);
    auto *verdictLayout = new QHBoxLayout(verdict);
    auto *goodButton = new QPushButton(QStringLiteral("Good"), verdict);
    goodButton->setObjectName(QStringLiteral("mark_good"));
    auto *loudButton = new QPushButton(QStringLiteral("Too loud"), verdict);
    loudButton->setObjectName(QStringLiteral("mark_too_loud"));
    auto *slowButton = new QPushButton(QStringLiteral("Too slow"), verdict);
    slowButton->setObjectName(QStringLiteral("mark_too_slow"));
    verdictLayout->addWidget(goodButton);
    verdictLayout->addWidget(loudButton);
    verdictLayout->addWidget(slowButton);
    nowLayout->addWidget(verdict);
    nowLayout->addStretch(1);

    // ---- Mode: pick one, move it, keep it --------------------------------
    auto *modeLayout = addPage(QStringLiteral("Mode"));
    QWidget *const modePage = m_ui->pages.constLast();

    auto *modeBox = new QGroupBox(QStringLiteral("Mode"), modePage);
    auto *modeForm = new QFormLayout(modeBox);
    m_ui->mode = new QComboBox(modeBox);
    m_ui->mode->setObjectName(QStringLiteral("mode"));
    m_ui->mode->setToolTip(QStringLiteral("Empty means the controls below are not any of the "
                                          "saved modes."));
    modeForm->addRow(QStringLiteral("Mode:"), m_ui->mode);
    m_ui->atBoot = new QCheckBox(QStringLiteral("Apply this one at boot"), modeBox);
    m_ui->atBoot->setObjectName(QStringLiteral("mode_at_boot"));
    modeForm->addRow(QString(), m_ui->atBoot);
    modeLayout->addWidget(modeBox);

    auto *numbers = new QGroupBox(QStringLiteral("Its numbers"), modePage);
    auto *numbersForm = new QFormLayout(numbers);
    const auto addTuner = [&](const QString &caption, const QString &name, int low, int high,
                              int step, const QString &suffix, Tuner **target) {
        auto *tuner = new Tuner(low, high, step, suffix, numbers);
        tuner->nameParts(name);
        numbersForm->addRow(caption, tuner);
        *target = tuner;
    };
    addTuner(QStringLiteral("Long-term limit:"), QStringLiteral("pl1"), 1, kMaxConfirmedPl1W, 1,
             QStringLiteral(" W"), &m_ui->pl1);
    addTuner(QStringLiteral("Short-term limit:"), QStringLiteral("pl2"), 1, kMaxConfirmedPl2W, 1,
             QStringLiteral(" W"), &m_ui->pl2);
    addTuner(QStringLiteral("Frequency ceiling:"), QStringLiteral("max_perf_pct"), kMinPerfPct,
             100, 1, QStringLiteral(" %"), &m_ui->maxPerfPct);
    addTuner(QStringLiteral("iGPU ceiling:"), QStringLiteral("gt_max_mhz"), kMinGtMhz, kMaxGtMhz,
             50, QStringLiteral(" MHz"), &m_ui->gtMaxMhz);

    m_ui->epp = new QComboBox(numbers);
    m_ui->epp->setObjectName(QStringLiteral("epp"));
    // Epp::Default is not offered: see the comment on it in sample.h.
    for (const Epp value : { Epp::Performance, Epp::BalancePerformance, Epp::BalancePower,
                             Epp::Power }) {
        m_ui->epp->addItem(eppToString(value), static_cast<int>(value));
    }
    numbersForm->addRow(QStringLiteral("Energy preference:"), m_ui->epp);

    // One control for the fan, because the machine has one fan and a person
    // wanting it loud should not have to know that three of these four are a
    // firmware curve and the fourth bypasses them.
    //
    // The three curves are what the firmware offers in
    // platform_profile_choices, and they are a noise-versus-temperature dial:
    // measured at identical power under sustained load, performance holds
    // 74 °C at 5000 RPM, balanced 80 °C at 3700, quiet above 84 °C at 2900.
    // Maximum is not a curve at all — it is pwm1_enable, roughly 6100 RPM,
    // ignoring whichever curve is selected.
    m_ui->profile = new QComboBox(numbers);
    m_ui->profile->setObjectName(QStringLiteral("profile"));
    for (const Profile value : { Profile::Quiet, Profile::Balanced, Profile::Performance })
        m_ui->profile->addItem(profileToString(value), static_cast<int>(value));
    m_ui->profile->addItem(QStringLiteral("maximum"), kFanMaximumItem);
    numbersForm->addRow(QStringLiteral("Fan:"), m_ui->profile);

    // One row for the two of them. They are both switches on the same subject —
    // how far the processor may boost — and a row each cost height the controls
    // above needed more.
    m_ui->turbo = new QCheckBox(QStringLiteral("Turbo"), numbers);
    m_ui->turbo->setObjectName(QStringLiteral("turbo"));
    m_ui->hwpBoost = new QCheckBox(QStringLiteral("HWP dynamic boost"), numbers);
    m_ui->hwpBoost->setObjectName(QStringLiteral("hwp_boost"));
    auto *boostRow = new QHBoxLayout;
    boostRow->setContentsMargins(0, 0, 0, 0);
    boostRow->addWidget(m_ui->turbo);
    boostRow->addWidget(m_ui->hwpBoost, 1);
    numbersForm->addRow(QStringLiteral("Boost:"), boostRow);
    modeLayout->addWidget(numbers);

    auto *keep = new QGroupBox(QStringLiteral("Keep it"), modePage);
    auto *keepLayout = new QVBoxLayout(keep);
    m_ui->modeState = new QLabel(keep);
    m_ui->modeState->setObjectName(QStringLiteral("mode_state"));
    m_ui->modeState->setWordWrap(true);
    keepLayout->addWidget(m_ui->modeState);
    auto *keepRow = new QHBoxLayout;
    auto *saveButton = new QPushButton(QStringLiteral("Save to"), keep);
    saveButton->setObjectName(QStringLiteral("save"));
    m_ui->saveTarget = new QComboBox(keep);
    m_ui->saveTarget->setObjectName(QStringLiteral("save_target"));
    auto *revertButton = new QPushButton(QStringLiteral("Revert"), keep);
    revertButton->setObjectName(QStringLiteral("revert"));
    keepRow->addWidget(saveButton);
    keepRow->addWidget(m_ui->saveTarget, 1);
    keepRow->addWidget(revertButton);
    keepLayout->addLayout(keepRow);
    modeLayout->addWidget(keep);

    modeLayout->addStretch(1);

    // ---- Backlight -------------------------------------------------------
    auto *backlightLayout = addPage(QStringLiteral("Backlight"));
    QWidget *const backlightPage = m_ui->pages.constLast();

    auto *backlight = new QGroupBox(QStringLiteral("Backlight"), backlightPage);
    auto *backlightForm = new QFormLayout(backlight);
    m_ui->autoBrightness =
            new QCheckBox(QStringLiteral("Adjust to the light in the room"), backlight);
    m_ui->autoBrightness->setObjectName(QStringLiteral("auto_brightness"));
    backlightForm->addRow(QString(), m_ui->autoBrightness);

    m_ui->backlightState = new QLabel(QStringLiteral("—"), backlight);
    m_ui->backlightState->setObjectName(QStringLiteral("backlight_state"));
    m_ui->backlightState->setWordWrap(true);
    backlightForm->addRow(QString(), m_ui->backlightState);

    const auto addBacklightTuner = [&](const QString &caption, const QString &name,
                                       Tuner **target) {
        auto *tuner = new Tuner(0, 100, 1, QStringLiteral(" %"), backlight);
        tuner->nameParts(name);
        backlightForm->addRow(caption, tuner);
        *target = tuner;
    };
    addBacklightTuner(QStringLiteral("Brightness:"), QStringLiteral("level"), &m_ui->level);
    addBacklightTuner(QStringLiteral("Reaction to light:"), QStringLiteral("response"),
                      &m_ui->response);
    addBacklightTuner(QStringLiteral("Never darker than:"), QStringLiteral("floor"),
                      &m_ui->floor);
    m_ui->response->setToolTip(QStringLiteral("How far the automation may move from the level "
                                              "you set."));
    m_ui->floor->setToolTip(QStringLiteral("Nothing goes below this — not the automation, and "
                                           "not a slip of the hand either."));
    backlightLayout->addWidget(backlight);
    backlightLayout->addStretch(1);

    // ---- Settings --------------------------------------------------------
    // The tab for what is about the program rather than about the machine. The
    // reset lives here rather than beside the numbers it discards: it covers
    // every setting there is, it is used about once, and on the mode tab it cost
    // height that the controls above it needed.
    auto *settingsLayout = addPage(QStringLiteral("Settings"));
    QWidget *const settingsPage = m_ui->pages.constLast();

    auto *trayBox = new QGroupBox(QStringLiteral("This console"), settingsPage);
    auto *trayForm = new QFormLayout(trayBox);

    m_ui->appearance = new QComboBox(trayBox);
    m_ui->appearance->setObjectName(QStringLiteral("appearance"));
    m_ui->appearance->addItem(QStringLiteral("Follow the desktop"),
                              theme::nameOf(theme::Preference::System));
    m_ui->appearance->setToolTip(QStringLiteral("Following the desktop takes its colours as "
                                                "they are and again whenever it changes them."));
    m_ui->appearance->addItem(QStringLiteral("Light"), theme::nameOf(theme::Preference::Light));
    m_ui->appearance->addItem(QStringLiteral("Dark"), theme::nameOf(theme::Preference::Dark));
    m_ui->appearance->setCurrentIndex(
            m_ui->appearance->findData(theme::nameOf(theme::preference())));
    trayForm->addRow(QStringLiteral("Appearance:"), m_ui->appearance);

    // Captioned like the row above it. Left in a row of its own with no caption,
    // a checkbox reads as another line of whatever is above — and here that
    // would be the appearance, which it has nothing to do with.
    m_ui->autostart = new QCheckBox(QStringLiteral("Start with the session"), trayBox);
    m_ui->autostart->setObjectName(QStringLiteral("autostart"));
    m_ui->autostart->setChecked(autostart::enabled());
    trayForm->addRow(QStringLiteral("Autostart:"), m_ui->autostart);
    auto *autostartNote =
            new QLabel(QStringLiteral("Takes effect at the next login: the session reads its "
                                      "autostart entries when it starts."),
                       trayBox);
    autostartNote->setWordWrap(true);
    trayForm->addRow(QString(), autostartNote);
    settingsLayout->addWidget(trayBox);

    auto *defaults = new QGroupBox(QStringLiteral("Back to the built-in values"), settingsPage);
    auto *defaultsLayout = new QVBoxLayout(defaults);
    auto *defaultsNote = new QLabel(QStringLiteral("Every mode definition and every backlight "
                                                   "setting, back to what is compiled into the "
                                                   "daemon."),
                                    defaults);
    defaultsNote->setWordWrap(true);
    defaultsLayout->addWidget(defaultsNote);
    auto *resetButton = new QPushButton(QStringLiteral("Reset everything"), defaults);
    resetButton->setObjectName(QStringLiteral("reset_all"));
    defaultsLayout->addWidget(resetButton);
    settingsLayout->addWidget(defaults);
    settingsLayout->addStretch(1);

    // Outside the tabs, like the banner: a command sent from one tab is
    // answered where the eye already is.
    m_ui->notice = new QLabel(this);
    m_ui->notice->setObjectName(QStringLiteral("notice"));
    m_ui->notice->setWordWrap(true);
    m_ui->notice->setContentsMargins(8, 4, 8, 8);
    // Hidden until there is something to say. Kept visible it would be a strip of
    // nothing along the bottom of every tab, which is what a reply line looks
    // like before the first command.
    m_ui->notice->setVisible(false);
    outer->addWidget(m_ui->notice);

    m_ui->commands = { modeBox, numbers, keep, defaults, backlight, verdict };

    // ---- Wiring ----------------------------------------------------------
    for (Tuner *tuner : { m_ui->pl1, m_ui->pl2, m_ui->maxPerfPct, m_ui->gtMaxMhz })
        connect(tuner, &Tuner::changed, this, &TrayWindow::sendCurrentSetpoints);
    for (QComboBox *box : { m_ui->epp, m_ui->profile })
        connect(box, &QComboBox::currentIndexChanged, this, &TrayWindow::sendCurrentSetpoints);
    for (QCheckBox *box : { m_ui->turbo, m_ui->hwpBoost })
        connect(box, &QCheckBox::toggled, this, &TrayWindow::sendCurrentSetpoints);

    connect(m_ui->mode, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (m_updating || index < 0)
            return;
        const QString id = m_ui->mode->itemData(index).toString();
        emit modeSelected(id);
        sendCommand(QStringLiteral("mode ") + id);
    });
    connect(m_ui->atBoot, &QCheckBox::toggled, this, [this](bool on) {
        if (m_updating || !on || m_ui->mode->currentIndex() < 0)
            return;
        sendCommand(QStringLiteral("config default_mode ")
                    + m_ui->mode->currentData().toString());
        reloadConfiguration();
    });
    connect(m_ui->saveTarget, &QComboBox::activated, this,
            [this](int) { m_saveTargetChosen = true; });
    connect(saveButton, &QPushButton::clicked, this, &TrayWindow::saveToMode);
    connect(revertButton, &QPushButton::clicked, this, &TrayWindow::revertToMode);
    // The question lives with the button rather than inside the slot: the slot is
    // the action, and an action that cannot be performed without a dialog cannot
    // be performed by a test either.
    connect(resetButton, &QPushButton::clicked, this, [this]() {
        if (QMessageBox::question(this, QStringLiteral("Back to the built-in values"),
                                  QStringLiteral("Put every mode definition and every backlight "
                                                 "setting back to the values compiled into the "
                                                 "daemon? Anything tuned since is lost."))
            == QMessageBox::Yes) {
            resetEverything();
        }
    });
    connect(m_ui->appearance, &QComboBox::currentIndexChanged, this, [this](int index) {
        setAppearance(theme::preferenceFrom(m_ui->appearance->itemData(index).toString()));
    });
    connect(m_ui->autoBrightness, &QCheckBox::toggled, this, [this](bool on) {
        if (m_updating)
            return;
        sendCommand(on ? QStringLiteral("brightness on") : QStringLiteral("brightness off"));
    });
    connect(m_ui->level, &Tuner::changed, this, &TrayWindow::sendBacklightLevel);
    // Applied while dragging and written when the drag ends: the value is a
    // setting rather than an experiment, and a slider must not write a file
    // thirty times on the way across.
    for (Tuner *tuner : { m_ui->response, m_ui->floor }) {
        connect(tuner, &Tuner::changed, this, [this]() { sendBacklightSettings(false); });
        connect(tuner, &Tuner::settled, this, [this]() { sendBacklightSettings(true); });
    }

    connect(m_ui->autostart, &QCheckBox::toggled, this, &TrayWindow::setAutostart);
    connect(goodButton, &QPushButton::clicked, this, &TrayWindow::markGood);
    connect(loudButton, &QPushButton::clicked, this, &TrayWindow::markTooLoud);
    connect(slowButton, &QPushButton::clicked, this, &TrayWindow::markTooSlow);
}

void TrayWindow::applyStatus(const TrayClient::Status &status)
{
    m_updating = true;
    if (status.valid)
        setCommandsEnabled(true);

    m_ui->packageC->setText(formatted(status.packageC, QStringLiteral("°C"), 1));
    m_ui->boardC->setText(formatted(status.boardC, QStringLiteral("°C"), 1));
    m_ui->nvmeC->setText(formatted(status.nvmeC, QStringLiteral("°C"), 1));
    m_ui->fanRpm->setText(formatted(status.fanRpm, QStringLiteral("RPM"), 0));
    m_ui->packageW->setText(formatted(status.packageW, QStringLiteral("W"), 2));
    m_ui->coreW->setText(formatted(status.coreW, QStringLiteral("W"), 2));
    m_ui->uncoreW->setText(formatted(status.uncoreW, QStringLiteral("W"), 2));
    m_ui->cpuMhz->setText(formatted(status.cpuMhz, QStringLiteral("MHz"), 0));
    m_ui->gpuMhz->setText(formatted(status.gpuMhz, QStringLiteral("MHz"), 0));
    m_ui->load1->setText(formatted(status.load1, QString(), 2));

    if (m_modeIds != status.modeIds) {
        m_modeIds = status.modeIds;
        m_ui->mode->clear();
        m_ui->saveTarget->clear();
        for (int i = 0; i < status.modeIds.size(); ++i) {
            const QString label = status.modeLabels.value(i, status.modeIds.at(i));
            m_ui->mode->addItem(label, status.modeIds.at(i));
            m_ui->saveTarget->addItem(label, status.modeIds.at(i));
        }
    }

    // Empty when the controls are not any of the saved modes. That is the whole
    // signal: something was moved, and what is running has no name.
    const int modeIndex = status.modeIds.indexOf(status.modeId);
    const QString modeLabel = status.modeLabels.value(modeIndex, status.modeId);
    m_ui->mode->setCurrentIndex(status.liveTuning ? -1 : modeIndex);
    m_ui->atBoot->setEnabled(!status.liveTuning && modeIndex >= 0);
    m_ui->atBoot->setChecked(!status.liveTuning && m_configuration.valid
                             && m_configuration.defaultModeId == status.modeId);
    if (status.liveTuning) {
        m_ui->modeState->setText(
                QStringLiteral("Changed from the saved %1. Save it into a mode, or revert.")
                        .arg(modeLabel));
        // The mode it was changed from is the likeliest destination, and it is
        // the one the person is thinking about. Only until they pick another:
        // after that the choice is theirs and is not overwritten.
        if (!m_saveTargetChosen && modeIndex >= 0)
            m_ui->saveTarget->setCurrentIndex(modeIndex);
    } else {
        m_ui->modeState->setText(
                QStringLiteral("These are the saved numbers of %1.").arg(modeLabel));
        if (modeIndex >= 0)
            m_ui->saveTarget->setCurrentIndex(modeIndex);
    }

    // What was asked for, not what reached the hardware: when the guard clamps,
    // the difference between the two is the information.
    m_ui->pl1->setValue(status.requested.pl1W);
    m_ui->pl2->setValue(status.requested.pl2W);
    m_ui->maxPerfPct->setValue(status.requested.maxPerfPct);
    m_ui->gtMaxMhz->setValue(status.requested.gtMaxMhz);
    m_ui->epp->setCurrentIndex(m_ui->epp->findData(static_cast<int>(status.requested.epp)));
    // Full speed is a selection here, not a separate switch: the applied fan
    // mode wins over the curve underneath it, because that is what is running.
    m_ui->profile->setCurrentIndex(
            status.requested.fanMode == 0
                    ? m_ui->profile->findData(kFanMaximumItem)
                    : m_ui->profile->findData(static_cast<int>(status.requested.profile)));
    m_ui->turbo->setChecked(!status.requested.noTurbo);
    m_ui->hwpBoost->setChecked(status.requested.hwpBoost);

    m_ui->applied->setText(
            QStringLiteral("Applied: PL1 %1 W, PL2 %2 W, ceiling %3 %, turbo %4, EPP %5, "
                           "fan curve %6, iGPU %7 MHz")
                    .arg(status.applied.pl1W)
                    .arg(status.applied.pl2W)
                    .arg(status.applied.maxPerfPct)
                    .arg(status.applied.noTurbo ? QStringLiteral("off") : QStringLiteral("on"),
                         eppToString(status.applied.epp), profileToString(status.applied.profile))
                    .arg(status.applied.gtMaxMhz));
    m_ui->powerSource->setText(status.onAc ? QStringLiteral("On mains")
                                           : QStringLiteral("On battery"));
    m_ui->live->setVisible(status.liveTuning);
    m_ui->guard->setVisible(status.guardEngaged);
    m_ui->mismatch->setVisible(status.writeMismatch);
    m_ui->sensors->setVisible(status.sensorsDegraded);
    m_ui->autoBrightness->setChecked(status.autoBrightness);
    m_ui->autoBrightness->setEnabled(status.brightnessAvailable);
    m_ui->level->setEnabled(status.brightnessAvailable);
    if (status.brightness > TrayClient::kNoReading)
        m_ui->level->setValue(static_cast<int>(status.brightness));
    m_ui->response->setValue(status.brightnessResponsePct);
    m_ui->floor->setValue(status.brightnessFloorPct);
    if (!status.brightnessAvailable) {
        m_ui->backlightState->setText(QStringLiteral("This machine has no backlight control."));
    } else if (status.autoBrightness) {
        // Per cent, and only per cent: what the sensor reads is the daemon's
        // business and means nothing to anybody looking at a screen.
        m_ui->backlightState->setText(
                QStringLiteral("Moving between %1 % and %2 %, around the %3 % you set.")
                        .arg(status.brightnessWindowLow)
                        .arg(status.brightnessWindowHigh)
                        .arg(status.brightnessBase > TrayClient::kNoReading
                                     ? QString::number(static_cast<int>(status.brightnessBase))
                                     : QStringLiteral("—")));
    } else {
        m_ui->backlightState->setText(
                QStringLiteral("Not adjusting. The brightness is yours alone."));
    }

    m_updating = false;

    // Read once, when the daemon first answers: definitions change when
    // somebody changes them, so there is nothing to poll for. Queued because
    // this is a blocking exchange on the socket whose reply is being handled.
    if (status.valid && !m_configuration.valid)
        QTimer::singleShot(0, this, &TrayWindow::reloadConfiguration);
}

void TrayWindow::reloadConfiguration()
{
    m_configuration = m_client.fetchConfiguration();
    if (!m_configuration.valid)
        return;
    m_updating = true;
    m_ui->atBoot->setChecked(m_ui->mode->currentIndex() >= 0
                             && m_configuration.defaultModeId
                                     == m_ui->mode->currentData().toString());
    m_updating = false;
}

// The fan half of the setpoint line. Maximum and a curve are mutually
// exclusive by construction: one control, one answer.
QString TrayWindow::fanSetting() const
{
    if (m_ui->profile->currentData().toInt() == kFanMaximumItem)
        return QStringLiteral("fan=max");
    return QStringLiteral("fan=auto profile=") + m_ui->profile->currentText();
}

void TrayWindow::sendCurrentSetpoints()
{
    if (m_updating)
        return;
    // The live vocabulary, which is shorter than the file's on purpose: these
    // are applied now and named for what they do rather than for how they are
    // written down.
    sendCommand(QStringLiteral("set pl1=%1 pl2=%2 max_perf_pct=%3 gt_max_mhz=%4 turbo=%5 "
                               "hwp_boost=%6 epp=%7 %8")
                        .arg(m_ui->pl1->value())
                        .arg(m_ui->pl2->value())
                        .arg(m_ui->maxPerfPct->value())
                        .arg(m_ui->gtMaxMhz->value())
                        .arg(m_ui->turbo->isChecked() ? QStringLiteral("on")
                                                      : QStringLiteral("off"),
                             m_ui->hwpBoost->isChecked() ? QStringLiteral("on")
                                                         : QStringLiteral("off"),
                             m_ui->epp->currentText(), fanSetting()));
}

void TrayWindow::saveToMode()
{
    const int index = m_ui->saveTarget->currentIndex();
    if (index < 0) {
        say(QStringLiteral("No modes to save into yet."));
        return;
    }
    sendCommand(QStringLiteral("config save live to ")
                + m_ui->saveTarget->itemData(index).toString());
    reloadConfiguration();
}

void TrayWindow::revertToMode()
{
    sendCommand(QStringLiteral("reset"));
}

void TrayWindow::setAppearance(theme::Preference wanted)
{
    // Stored before it is applied, because anything else that wants to know
    // reads the stored choice rather than being told about it.
    theme::setPreference(wanted);
    theme::applyPreference();
}

void TrayWindow::resetEverything()
{
    sendCommand(QStringLiteral("config reset all"));
    reloadConfiguration();
}

void TrayWindow::sendBacklightLevel()
{
    if (m_updating)
        return;
    sendCommand(QStringLiteral("brightness level %1").arg(m_ui->level->value()));
}

void TrayWindow::sendBacklightSettings(bool durable)
{
    if (m_updating)
        return;
    const QString assignments = QStringLiteral("response_pct=%1 floor_pct=%2")
                                        .arg(m_ui->response->value())
                                        .arg(m_ui->floor->value());
    sendCommand(durable ? QStringLiteral("config set brightness ") + assignments
                        : QStringLiteral("brightness set ") + assignments);
}

void TrayWindow::setAutostart(bool on)
{
    if (m_updating)
        return;
    QString error;
    if (autostart::setEnabled(on, &error)) {
        say(on ? QStringLiteral("Will start with the session from the next login.")
               : QStringLiteral("Will not start with the session."));
        return;
    }
    // The switch goes back, because the file is the state and the file did not
    // change. A checkbox that stayed on after a failed write would be a lie.
    m_updating = true;
    m_ui->autostart->setChecked(!on);
    m_updating = false;
    say(QStringLiteral("autostart → %1").arg(error));
}

void TrayWindow::markGood()
{
    sendCommand(QStringLiteral("mark good"));
}

void TrayWindow::markTooLoud()
{
    sendCommand(QStringLiteral("mark too-loud"));
}

void TrayWindow::markTooSlow()
{
    sendCommand(QStringLiteral("mark too-slow"));
}

void TrayWindow::onConnectionChanged(bool connected)
{
    if (connected) {
        m_ui->connection->setText(
                QStringLiteral("Connected to the daemon on %1").arg(m_client.socketPath()));
    } else {
        // The socket and the group are both named: "not answering" on its own
        // leaves the person guessing which of the two usual causes it is.
        m_ui->connection->setText(
                QStringLiteral("The daemon is not answering on %1. It may not be running, or "
                               "you may not be in the %2 group.")
                        .arg(m_client.socketPath(), QString::fromLatin1(kControlSocketGroup)));
    }
    setCommandsEnabled(connected);
}

void TrayWindow::setCommandsEnabled(bool enabled)
{
    for (QWidget *group : std::as_const(m_ui->commands))
        group->setEnabled(enabled);
}

void TrayWindow::sendCommand(const QString &command)
{
    const QString reply = m_client.sendAndRefresh(command);
    say(QStringLiteral("%1 → %2")
                .arg(command, reply.isEmpty() ? QStringLiteral("no reply") : reply));
}

void TrayWindow::say(const QString &text)
{
    m_ui->notice->setText(text);
    if (m_ui->notice->isVisible())
        return;
    m_ui->notice->setVisible(true);
    // The frame is fixed, so it has to be told to make room. The alternative is
    // reserving the room from the start, and that is the strip of nothing this
    // exists to avoid.
    if (m_sized) {
        const int extra = m_ui->notice->sizeHint().height();
        setFixedHeight(height() + extra);
    }
}

#include "tray_window.moc"

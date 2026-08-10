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
// How long an accepted command's reply stays on the status line before it goes
// back to saying what the daemon's state is.
constexpr int kReplyLingerMs = 5000;
// Between the two columns of readings, so a value on the left and a caption on
// the right are not one run of text.
constexpr int kColumnGapPx = 24;
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
    auto *label = new QLabel(QStringLiteral("—"));
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

    // One line for both, and it sits under the tabs: what the daemon's state is,
    // and what it said to the last command. Two of them meant a strip of nothing
    // under every tab for as long as nothing had been commanded yet.
    QLabel *status = nullptr;

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
    Tuner *base = nullptr;
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
    setWindowTitle(tr("Sombra"));
    build();

    connect(&m_client, &TrayClient::statusChanged, this, &TrayWindow::applyStatus);
    connect(&m_client, &TrayClient::connectionChanged, this, &TrayWindow::onConnectionChanged);

    // One shot, restarted by every accepted reply: what it restores is the line
    // the window shows when nothing has just happened.
    m_statusTimer = new QTimer(this);
    m_statusTimer->setSingleShot(true);
    connect(m_statusTimer, &QTimer::timeout, this, &TrayWindow::showConnectionState);

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
void TrayWindow::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    paintStatus();
}

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
    int narrowest = 0;
    for (const QWidget *page : std::as_const(m_ui->pages)) {
        // Both hints, because a word-wrapped label reports a narrow preferred
        // width and a tall height for it — take the preferred size alone and
        // one such label drags the whole console narrow.
        narrowest =
                std::max({ narrowest, page->sizeHint().width(), page->minimumSizeHint().width() });
    }
    narrowest += kSideChromePx;

    int ceiling = kMeasureSizePx;
    int widthLimit = narrowest * 2;
    if (const QScreen *screen = QGuiApplication::primaryScreen()) {
        const QRect available = screen->availableGeometry();
        ceiling = available.height() * 9 / 10;
        widthLimit = std::min(widthLimit, available.width());
    }

    // Wider costs nothing and shorter is what matters: a translated label wraps
    // where an English one did not, and three wrapped sentences are the
    // difference between a window that fits on the screen and one that opens with
    // a scrollbar. So the width is tried at the preferred one and then wider,
    // and the first that brings the tallest tab under the screen's ceiling wins.
    int width = narrowest;
    int height = 0;
    for (const int candidate : { narrowest, narrowest * 5 / 4, narrowest * 3 / 2 }) {
        width = std::min(candidate, widthLimit);
        height = tallestTab(width);
        if (height <= ceiling)
            break;
    }

    // Still taller than the screen: the scroll area earns its keep, and the frame
    // is left free so the window can be dragged bigger. A fixed size here would
    // be a window that cannot show what it holds and cannot be made to.
    if (height > ceiling) {
        setMinimumSize(0, 0);
        setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
        resize(width, ceiling);
        return;
    }
    setFixedSize(width, height);
}

int TrayWindow::tallestTab(int width)
{
    // The height is measured rather than guessed at. What surrounds a page — the
    // tab bar, the status line, every margin — is asked to report itself by
    // giving the window more room than any page needs and then reading how much
    // of it the viewport got. A constant in its place is a constant that is wrong
    // by a strip of empty space under the tallest tab.
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
    return height;
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
    m_ui->tabs = new QTabWidget(this);
    m_ui->tabs->setObjectName(QStringLiteral("tabs"));
    outer->addWidget(m_ui->tabs, 1);

    // Below the tabs, which is where a status line belongs: that is where every
    // other window on the machine puts one, and a person looking for "did that
    // work" looks at the bottom edge.
    m_ui->status = new QLabel(this);
    m_ui->status->setObjectName(QStringLiteral("status"));
    // One line, always. A command echoed back with its assignments is longer than
    // any sensible window, and wrapping it would take a line away from the tab
    // above — the alternative is reserving a second line for a message that is
    // usually four words. What does not fit is elided, and the whole of it is in
    // the tooltip.
    m_ui->status->setWordWrap(false);
    m_ui->status->setContentsMargins(8, 6, 8, 6);
    outer->addWidget(m_ui->status);

    // The name is not the title: the title is translated and the name is what the
    // documentation pictures are called, which has to be the same on a machine in
    // any language. Prefixed, because a page and a control may otherwise want the
    // same word — "mode" is both a tab and the selector on it.
    const auto addPage = [this](const QString &name, const QString &title) {
        auto *area = new QScrollArea(m_ui->tabs);
        area->setObjectName(QStringLiteral("tab_") + name);
        area->setWidgetResizable(true);
        area->setFrameShape(QFrame::NoFrame);
        auto *page = new QWidget(area);
        auto *layout = new QVBoxLayout(page);
        area->setWidget(page);
        m_ui->pages.append(page);
        m_ui->tabs->addTab(area, title);
        return layout;
    };

    // ---- Status: what only reports, and the verdicts about it -------------
    auto *nowLayout = addPage(QStringLiteral("status"), tr("Status"));
    QWidget *const nowPage = m_ui->pages.constLast();

    // Two columns, because ten readings in one are ten rows of height and the
    // window has a screen to fit into — in a language whose labels are longer
    // than English, that difference is a scrollbar.
    auto *readings = new QGroupBox(tr("Readings"), nowPage);
    auto *readingsColumns = new QHBoxLayout(readings);
    auto *leftForm = new QFormLayout;
    auto *rightForm = new QFormLayout;
    readingsColumns->addLayout(leftForm, 1);
    readingsColumns->addSpacing(kColumnGapPx);
    readingsColumns->addLayout(rightForm, 1);

    // Temperatures and the fan on one side, what the power is doing on the
    // other: the two questions a glance asks are "how hot" and "why".
    m_ui->packageC = addReading(leftForm, tr("Package:"), QStringLiteral("pkg_c"));
    m_ui->boardC = addReading(leftForm, tr("Board:"), QStringLiteral("board_c"));
    m_ui->nvmeC = addReading(leftForm, tr("NVMe:"), QStringLiteral("nvme_c"));
    m_ui->fanRpm = addReading(leftForm, tr("Fan:"), QStringLiteral("fan_rpm"));
    m_ui->load1 = addReading(leftForm, tr("Load average:"), QStringLiteral("load1"));

    m_ui->packageW = addReading(rightForm, tr("Package power:"), QStringLiteral("pkg_w"));
    // The core/uncore split is what tells CPU heat from iGPU heat.
    m_ui->coreW = addReading(rightForm, tr("Core power:"), QStringLiteral("core_w"));
    m_ui->uncoreW = addReading(rightForm, tr("Uncore power:"), QStringLiteral("uncore_w"));
    m_ui->cpuMhz = addReading(rightForm, tr("CPU frequency:"), QStringLiteral("cpu_mhz"));
    m_ui->gpuMhz = addReading(rightForm, tr("iGPU frequency:"), QStringLiteral("gpu_mhz"));
    nowLayout->addWidget(readings);

    auto *state = new QGroupBox(tr("State"), nowPage);
    auto *stateLayout = new QVBoxLayout(state);
    m_ui->applied = new QLabel(QStringLiteral("—"), state);
    m_ui->applied->setObjectName(QStringLiteral("applied"));
    m_ui->applied->setWordWrap(true);
    stateLayout->addWidget(m_ui->applied);
    m_ui->powerSource = new QLabel(QStringLiteral("—"), state);
    m_ui->powerSource->setObjectName(QStringLiteral("power_source"));
    stateLayout->addWidget(m_ui->powerSource);
    m_ui->live = addIndicator(stateLayout,
                              tr("The controls have been moved: what is running is "
                                             "not one of the saved modes."),
                              QStringLiteral("live_tuning"));
    // The guard is a safety interlock, so its state has to be visible: while it
    // clamps, the controls say what was asked for and not what is applied.
    m_ui->guard = addIndicator(stateLayout,
                               tr("Emergency thermal guard engaged: the package is "
                                              "near the hardware cutout and power is clamped."),
                               QStringLiteral("guard"));
    m_ui->mismatch = addIndicator(stateLayout,
                                  tr("A control read back a value the daemon did not "
                                                 "write: another process is writing them too."),
                                  QStringLiteral("mismatch"));
    m_ui->sensors = addIndicator(stateLayout, tr("Some sensors could not be read."),
                                 QStringLiteral("sensors"));
    nowLayout->addWidget(state);

    // The verdicts live beside the readings they are about: a judgement is made
    // while looking at the numbers, and it is stamped onto the sample carrying
    // them.
    // Not only about noise: two of the three answers are about noise and speed,
    // and the question a person answers here is whether the machine is behaving.
    auto *verdict = new QGroupBox(tr("How is it working"), nowPage);
    auto *verdictLayout = new QHBoxLayout(verdict);
    auto *goodButton = new QPushButton(tr("Good"), verdict);
    goodButton->setToolTip(tr("Stamps this judgement onto the sample being recorded now, so an "
                              "opinion made by ear stays attached to the numbers behind it."));
    goodButton->setObjectName(QStringLiteral("mark_good"));
    auto *loudButton = new QPushButton(tr("Too loud"), verdict);
    loudButton->setToolTip(tr("The same, for a machine that is cool enough and audible."));
    loudButton->setObjectName(QStringLiteral("mark_too_loud"));
    auto *slowButton = new QPushButton(tr("Too slow"), verdict);
    slowButton->setToolTip(tr("The same, for a machine that is quiet enough and sluggish."));
    slowButton->setObjectName(QStringLiteral("mark_too_slow"));
    verdictLayout->addWidget(goodButton);
    verdictLayout->addWidget(loudButton);
    verdictLayout->addWidget(slowButton);
    nowLayout->addWidget(verdict);
    nowLayout->addStretch(1);

    // ---- Mode: pick one, move it, keep it --------------------------------
    auto *modeLayout = addPage(QStringLiteral("mode"), tr("Mode"));
    QWidget *const modePage = m_ui->pages.constLast();

    auto *modeBox = new QGroupBox(tr("Mode"), modePage);
    auto *modeForm = new QFormLayout(modeBox);
    m_ui->mode = new QComboBox(modeBox);
    m_ui->mode->setObjectName(QStringLiteral("mode"));
    m_ui->mode->setToolTip(tr("Pick one and the controls below take its numbers. Empty means "
                              "what is running is not any of the saved modes."));
    modeForm->addRow(tr("Mode:"), m_ui->mode);
    m_ui->atBoot = new QCheckBox(tr("Apply this one at boot"), modeBox);
    m_ui->atBoot->setObjectName(QStringLiteral("mode_at_boot"));
    m_ui->atBoot->setToolTip(tr("Which mode the daemon applies when the machine starts."));
    modeForm->addRow(QString(), m_ui->atBoot);
    modeLayout->addWidget(modeBox);

    auto *numbers = new QGroupBox(tr("Its numbers"), modePage);
    auto *numbersForm = new QFormLayout(numbers);
    const auto addTuner = [&](const QString &caption, const QString &name, int low, int high,
                              int step, const QString &suffix, const QString &hint,
                              Tuner **target) {
        auto *tuner = new Tuner(low, high, step, suffix, numbers);
        tuner->nameParts(name);
        tuner->setToolTip(hint);
        numbersForm->addRow(caption, tuner);
        *target = tuner;
    };
    addTuner(tr("Long-term limit:"), QStringLiteral("pl1"), 1, kMaxConfirmedPl1W, 1,
             QStringLiteral(" W"),
             tr("The power the processor may draw for as long as it likes. This is the control "
                "that decides how hot the machine gets under sustained load."),
             &m_ui->pl1);
    addTuner(tr("Short-term limit:"), QStringLiteral("pl2"), 1, kMaxConfirmedPl2W, 1,
             QStringLiteral(" W"),
             tr("The power it may draw in a burst of a few seconds. It decides how quick the "
                "machine feels rather than how warm it runs."),
             &m_ui->pl2);
    addTuner(tr("Frequency ceiling:"), QStringLiteral("max_perf_pct"), kMinPerfPct,
             100, 1, QStringLiteral(" %"),
             tr("How much of the processor's frequency range is available, in per cent."),
             &m_ui->maxPerfPct);
    addTuner(tr("iGPU ceiling:"), QStringLiteral("gt_max_mhz"), kMinGtMhz, kMaxGtMhz,
             50, QStringLiteral(" MHz"),
             tr("The graphics frequency ceiling. Lowering it saves power, and an external "
                "monitor is work the graphics cannot postpone — too low a ceiling shows as "
                "stutter."),
             &m_ui->gtMaxMhz);

    m_ui->epp = new QComboBox(numbers);
    m_ui->epp->setObjectName(QStringLiteral("epp"));
    // Epp::Default is not offered: see the comment on it in sample.h.
    // The words are the kernel's and stay as they are — they are what goes into
    // the settings file — but each carries what it means, because "balance_power"
    // is a value and not a sentence.
    const auto eppMeaning = [this](Epp value) {
        switch (value) {
        case Epp::Performance:
            return tr("Answer as fast as possible, whatever it costs.");
        case Epp::BalancePerformance:
            return tr("Lean towards answering quickly.");
        case Epp::BalancePower:
            return tr("Lean towards saving energy.");
        case Epp::Power:
            return tr("Save energy wherever it can. This is what suppresses the short bursts "
                      "that heat the package while the machine is idle.");
        default:
            return QString();
        }
    };
    for (const Epp value : { Epp::Performance, Epp::BalancePerformance, Epp::BalancePower,
                             Epp::Power }) {
        m_ui->epp->addItem(eppToString(value), static_cast<int>(value));
        m_ui->epp->setItemData(m_ui->epp->count() - 1, eppMeaning(value), Qt::ToolTipRole);
    }
    m_ui->epp->setToolTip(tr("What the processor is told to prefer between saving energy and "
                             "answering quickly. Together with HWP dynamic boost this is what "
                             "suppresses the short bursts that heat the package while the "
                             "machine is nominally idle."));
    numbersForm->addRow(tr("Energy preference:"), m_ui->epp);

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
    // Measured on this machine at identical power under sustained load, which is
    // the only honest way to describe a curve whose shape the firmware keeps to
    // itself.
    const auto curveMeaning = [this](Profile value) {
        switch (value) {
        case Profile::Quiet:
            return tr("The quietest curve: about 2900 RPM under load, and above 84 °C for it.");
        case Profile::Balanced:
            return tr("The middle curve: about 3700 RPM under load and 80 °C.");
        case Profile::Performance:
            return tr("The eager curve: about 5000 RPM under load and 74 °C.");
        default:
            return QString();
        }
    };
    for (const Profile value : { Profile::Quiet, Profile::Balanced, Profile::Performance }) {
        m_ui->profile->addItem(profileToString(value), static_cast<int>(value));
        m_ui->profile->setItemData(m_ui->profile->count() - 1, curveMeaning(value),
                                   Qt::ToolTipRole);
    }
    m_ui->profile->addItem(QStringLiteral("maximum"), kFanMaximumItem);
    m_ui->profile->setItemData(m_ui->profile->count() - 1,
                               tr("Not a curve: full speed, roughly 6100 RPM, whatever the "
                                  "temperature is."),
                               Qt::ToolTipRole);
    m_ui->profile->setToolTip(tr("Three of these are the fan curves the firmware carries, from "
                                 "the quietest to the most eager. Maximum is not a curve: it "
                                 "runs the fan at full speed, roughly 6100 RPM."));
    numbersForm->addRow(tr("Fan:"), m_ui->profile);

    // One row for the two of them. They are both switches on the same subject —
    // how far the processor may boost — and a row each cost height the controls
    // above needed more.
    m_ui->turbo = new QCheckBox(tr("Turbo"), numbers);
    m_ui->turbo->setObjectName(QStringLiteral("turbo"));
    m_ui->turbo->setToolTip(tr("Whether the processor may go above its base frequency at all."));
    m_ui->hwpBoost = new QCheckBox(tr("HWP dynamic boost"), numbers);
    m_ui->hwpBoost->setObjectName(QStringLiteral("hwp_boost"));
    m_ui->hwpBoost->setToolTip(tr("Whether the hardware may raise the frequency on its own when "
                                  "it thinks the work needs it."));
    auto *boostRow = new QHBoxLayout;
    boostRow->setContentsMargins(0, 0, 0, 0);
    boostRow->addWidget(m_ui->turbo);
    boostRow->addWidget(m_ui->hwpBoost, 1);
    numbersForm->addRow(tr("Boost:"), boostRow);
    modeLayout->addWidget(numbers);

    auto *keep = new QGroupBox(tr("Keep it"), modePage);
    auto *keepLayout = new QVBoxLayout(keep);
    m_ui->modeState = new QLabel(keep);
    m_ui->modeState->setObjectName(QStringLiteral("mode_state"));
    m_ui->modeState->setWordWrap(true);
    keepLayout->addWidget(m_ui->modeState);
    auto *keepRow = new QHBoxLayout;
    auto *saveButton = new QPushButton(tr("Save to"), keep);
    saveButton->setObjectName(QStringLiteral("save"));
    saveButton->setToolTip(tr("Writes the numbers above into the mode named beside this button. "
                              "Nothing else here is durable."));
    m_ui->saveTarget = new QComboBox(keep);
    m_ui->saveTarget->setObjectName(QStringLiteral("save_target"));
    m_ui->saveTarget->setToolTip(tr("Which of the modes the numbers above land in."));
    auto *revertButton = new QPushButton(tr("Revert"), keep);
    revertButton->setObjectName(QStringLiteral("revert"));
    revertButton->setToolTip(tr("Back to the saved numbers of the mode in force, discarding what "
                                "the controls were moved to."));
    keepRow->addWidget(saveButton);
    keepRow->addWidget(m_ui->saveTarget, 1);
    keepRow->addWidget(revertButton);
    keepLayout->addLayout(keepRow);
    modeLayout->addWidget(keep);

    modeLayout->addStretch(1);

    // ---- Backlight -------------------------------------------------------
    auto *backlightLayout = addPage(QStringLiteral("backlight"), tr("Backlight"));
    QWidget *const backlightPage = m_ui->pages.constLast();

    auto *backlight = new QGroupBox(tr("Backlight"), backlightPage);
    auto *backlightForm = new QFormLayout(backlight);
    m_ui->autoBrightness =
            new QCheckBox(tr("Adjust to the light in the room"), backlight);
    m_ui->autoBrightness->setObjectName(QStringLiteral("auto_brightness"));
    backlightForm->addRow(QString(), m_ui->autoBrightness);

    m_ui->backlightState = new QLabel(QStringLiteral("—"), backlight);
    m_ui->backlightState->setObjectName(QStringLiteral("backlight_state"));
    m_ui->backlightState->setWordWrap(true);
    backlightForm->addRow(QString(), m_ui->backlightState);

    const auto addBacklightTuner = [&](const QString &caption, const QString &name,
                                       const QString &hint, Tuner **target) {
        auto *tuner = new Tuner(0, 100, 1, QStringLiteral(" %"), backlight);
        tuner->nameParts(name);
        tuner->setToolTip(hint);
        backlightForm->addRow(caption, tuner);
        *target = tuner;
    };
    addBacklightTuner(tr("Brightness:"), QStringLiteral("level"),
                      tr("The backlight as it is right now. Moving it says what you want in this "
                         "light, and the baseline below follows from that."),
                      &m_ui->level);
    addBacklightTuner(tr("Baseline:"), QStringLiteral("base"),
                      tr("The level the automation works around. Moving it sets the baseline "
                         "directly instead of waiting for the right light to set it through the "
                         "brightness above."),
                      &m_ui->base);
    addBacklightTuner(tr("Reaction to light:"), QStringLiteral("response"),
                      tr("How far the room may move the backlight either way from the baseline, "
                         "in percentage points."),
                      &m_ui->response);
    addBacklightTuner(tr("Never darker than:"), QStringLiteral("floor"),
                      tr("Nothing goes below this — not the automation, and not a slip of the "
                         "hand either. There is no ceiling: that is 100 %."),
                      &m_ui->floor);
    m_ui->autoBrightness->setToolTip(tr("Whether the backlight follows the ambient light sensor. "
                                        "Switching it on or off never moves the backlight by "
                                        "itself."));
    backlightLayout->addWidget(backlight);
    backlightLayout->addStretch(1);

    // ---- Settings --------------------------------------------------------
    // The tab for what is about the program rather than about the machine. The
    // reset lives here rather than beside the numbers it discards: it covers
    // every setting there is, it is used about once, and on the mode tab it cost
    // height that the controls above it needed.
    auto *settingsLayout = addPage(QStringLiteral("settings"), tr("Settings"));
    QWidget *const settingsPage = m_ui->pages.constLast();

    auto *trayBox = new QGroupBox(tr("This console"), settingsPage);
    auto *trayForm = new QFormLayout(trayBox);

    m_ui->appearance = new QComboBox(trayBox);
    m_ui->appearance->setObjectName(QStringLiteral("appearance"));
    m_ui->appearance->addItem(tr("Follow the desktop"),
                              theme::nameOf(theme::Preference::System));
    m_ui->appearance->setToolTip(tr("Following the desktop takes its colours as they are, and "
                                    "again whenever it changes them. Light and dark are this "
                                    "window's own and ignore the desktop."));
    m_ui->appearance->addItem(tr("Light"), theme::nameOf(theme::Preference::Light));
    m_ui->appearance->addItem(tr("Dark"), theme::nameOf(theme::Preference::Dark));
    m_ui->appearance->setCurrentIndex(
            m_ui->appearance->findData(theme::nameOf(theme::preference())));
    trayForm->addRow(tr("Appearance:"), m_ui->appearance);

    // Captioned like the row above it. Left in a row of its own with no caption,
    // a checkbox reads as another line of whatever is above — and here that
    // would be the appearance, which it has nothing to do with.
    m_ui->autostart = new QCheckBox(tr("Start with the session"), trayBox);
    m_ui->autostart->setObjectName(QStringLiteral("autostart"));
    m_ui->autostart->setChecked(autostart::enabled());
    m_ui->autostart->setToolTip(tr("Takes effect at the next login: the session reads its "
                                   "autostart entries when it starts."));
    trayForm->addRow(tr("Autostart:"), m_ui->autostart);
    settingsLayout->addWidget(trayBox);

    auto *defaults = new QGroupBox(tr("Back to the built-in values"), settingsPage);
    auto *defaultsLayout = new QVBoxLayout(defaults);
    auto *resetButton = new QPushButton(tr("Reset everything"), defaults);
    resetButton->setObjectName(QStringLiteral("reset_all"));
    resetButton->setToolTip(tr("Every mode definition and every backlight setting, back to what "
                               "is compiled into the daemon. Asks first."));
    defaultsLayout->addWidget(resetButton);
    settingsLayout->addWidget(defaults);
    settingsLayout->addStretch(1);

    // Outside the tabs, like the banner: a command sent from one tab is
    // answered where the eye already is.


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
        if (QMessageBox::question(this, tr("Back to the built-in values"),
                                  tr("Put every mode definition and every backlight "
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
    connect(m_ui->base, &Tuner::changed, this, &TrayWindow::sendBacklightBase);
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
        describeModes();
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
                tr("Changed from the saved %1. Save it into a mode, or revert.")
                        .arg(modeLabel));
        // The mode it was changed from is the likeliest destination, and it is
        // the one the person is thinking about. Only until they pick another:
        // after that the choice is theirs and is not overwritten.
        if (!m_saveTargetChosen && modeIndex >= 0)
            m_ui->saveTarget->setCurrentIndex(modeIndex);
    } else {
        m_ui->modeState->setText(
                tr("These are the saved numbers of %1.").arg(modeLabel));
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
            tr("Applied: PL1 %1 W, PL2 %2 W, ceiling %3 %, turbo %4, EPP %5, "
                           "fan curve %6, iGPU %7 MHz")
                    .arg(status.applied.pl1W)
                    .arg(status.applied.pl2W)
                    .arg(status.applied.maxPerfPct)
                    // The words are translated; the vocabulary beside them is not.
                    // `epp` and the fan curves are the values of the settings
                    // file, and a translated one would name nothing.
                    .arg(status.applied.noTurbo ? tr("off") : tr("on"),
                         eppToString(status.applied.epp), profileToString(status.applied.profile))
                    .arg(status.applied.gtMaxMhz));
    m_ui->powerSource->setText(status.onAc ? tr("On mains") : tr("On battery"));
    m_ui->live->setVisible(status.liveTuning);
    m_ui->guard->setVisible(status.guardEngaged);
    m_ui->mismatch->setVisible(status.writeMismatch);
    m_ui->sensors->setVisible(status.sensorsDegraded);
    m_ui->autoBrightness->setChecked(status.autoBrightness);
    m_ui->autoBrightness->setEnabled(status.brightnessAvailable);
    m_ui->level->setEnabled(status.brightnessAvailable);
    if (status.brightness > TrayClient::kNoReading)
        m_ui->level->setValue(static_cast<int>(status.brightness));
    // The baseline is what the automation works around, so it is a control only
    // while the automation is running. Left enabled it would move a number
    // nothing acts on.
    m_ui->base->setEnabled(status.brightnessAvailable && status.autoBrightness);
    if (status.brightnessBase > TrayClient::kNoReading)
        m_ui->base->setValue(static_cast<int>(status.brightnessBase));
    m_ui->response->setValue(status.brightnessResponsePct);
    m_ui->floor->setValue(status.brightnessFloorPct);
    if (!status.brightnessAvailable) {
        m_ui->backlightState->setText(tr("This machine has no backlight control."));
    } else if (status.autoBrightness) {
        // Per cent, and only per cent: what the sensor reads is the daemon's
        // business and means nothing to anybody looking at a screen.
        m_ui->backlightState->setText(
                tr("Moving between %1 % and %2 %, around the %3 % you set.")
                        .arg(status.brightnessWindowLow)
                        .arg(status.brightnessWindowHigh)
                        .arg(status.brightnessBase > TrayClient::kNoReading
                                     ? QString::number(static_cast<int>(status.brightnessBase))
                                     : QStringLiteral("—")));
    } else {
        m_ui->backlightState->setText(
                tr("Not adjusting. The brightness is yours alone."));
    }

    m_updating = false;

    // Read once, when the daemon first answers: definitions change when
    // somebody changes them, so there is nothing to poll for. Queued because
    // this is a blocking exchange on the socket whose reply is being handled.
    if (status.valid && !m_configuration.valid)
        QTimer::singleShot(0, this, &TrayWindow::reloadConfiguration);
}

void TrayWindow::describeModes()
{
    // A name is not an explanation: somebody opening this for the first time sees
    // three words and has no way to know which of them is the cool one. So every
    // entry carries its own numbers and where it sits on the scale, and those are
    // read from the definitions rather than written here — the names and the
    // numbers are the maintainer's, and a hard-coded description would go stale
    // the first time one of them was tuned.
    const int count = m_ui->mode->count();
    for (int i = 0; i < count; ++i) {
        const QString id = m_ui->mode->itemData(i).toString();
        const TrayClient::ModeDefinition *definition = nullptr;
        for (const TrayClient::ModeDefinition &candidate : m_configuration.modes) {
            if (candidate.id == id)
                definition = &candidate;
        }

        QString place;
        if (count > 1 && i == 0)
            place = tr("the coolest of them");
        else if (count > 1 && i == count - 1)
            place = tr("the widest of them");
        else if (count > 2)
            place = tr("the working one");

        QString description;
        if (definition) {
            description = tr("%1/%2 W, ceiling %3 %, iGPU %4 MHz, %5 fan curve, turbo %6")
                                  .arg(definition->setpoints.pl1W)
                                  .arg(definition->setpoints.pl2W)
                                  .arg(definition->setpoints.maxPerfPct)
                                  .arg(definition->setpoints.gtMaxMhz)
                                  .arg(profileToString(definition->setpoints.profile),
                                       definition->setpoints.noTurbo ? tr("off") : tr("on"));
        }
        if (!place.isEmpty()) {
            description = description.isEmpty()
                    ? place
                    : QStringLiteral("%1 — %2").arg(place, description);
        }
        if (description.isEmpty())
            continue;
        m_ui->mode->setItemData(i, description, Qt::ToolTipRole);
        m_ui->saveTarget->setItemData(i, description, Qt::ToolTipRole);
    }
}

void TrayWindow::reloadConfiguration()
{
    m_configuration = m_client.fetchConfiguration();
    if (!m_configuration.valid)
        return;
    describeModes();
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

    // Only what moved. Sending all nine assignments works and reads badly: the
    // status line then echoes a command whose interesting part is one number in
    // the middle of it, and the daemon is told eight things it already knows.
    const Setpoints &now = m_client.status().requested;
    QStringList changed;
    const auto note = [&changed](const QString &key, const QString &value) {
        changed.append(key + QLatin1Char('=') + value);
    };
    if (m_ui->pl1->value() != now.pl1W)
        note(QStringLiteral("pl1"), QString::number(m_ui->pl1->value()));
    if (m_ui->pl2->value() != now.pl2W)
        note(QStringLiteral("pl2"), QString::number(m_ui->pl2->value()));
    if (m_ui->maxPerfPct->value() != now.maxPerfPct)
        note(QStringLiteral("max_perf_pct"), QString::number(m_ui->maxPerfPct->value()));
    if (m_ui->gtMaxMhz->value() != now.gtMaxMhz)
        note(QStringLiteral("gt_max_mhz"), QString::number(m_ui->gtMaxMhz->value()));
    if (m_ui->turbo->isChecked() == now.noTurbo) {
        note(QStringLiteral("turbo"),
             m_ui->turbo->isChecked() ? QStringLiteral("on") : QStringLiteral("off"));
    }
    if (m_ui->hwpBoost->isChecked() != now.hwpBoost) {
        note(QStringLiteral("hwp_boost"),
             m_ui->hwpBoost->isChecked() ? QStringLiteral("on") : QStringLiteral("off"));
    }
    const Epp epp = static_cast<Epp>(m_ui->epp->currentData().toInt());
    if (epp != now.epp)
        note(QStringLiteral("epp"), eppToString(epp));

    // The fan is one control over two keys, so it is compared as one thing: a
    // curve is only meaningful while the fan is not at full speed.
    const QString fan = fanSetting();
    const QString fanNow = now.fanMode == 0
            ? QStringLiteral("fan=max")
            : QStringLiteral("fan=auto profile=") + profileToString(now.profile);
    if (fan != fanNow)
        changed.append(fan);

    if (changed.isEmpty())
        return;
    sendCommand(QStringLiteral("set ") + changed.join(QLatin1Char(' ')));
}

void TrayWindow::saveToMode()
{
    const int index = m_ui->saveTarget->currentIndex();
    if (index < 0) {
        report(tr("No modes to save into yet."), true);
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

void TrayWindow::sendBacklightBase()
{
    if (m_updating)
        return;
    sendCommand(QStringLiteral("brightness base %1").arg(m_ui->base->value()));
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
        report(on ? tr("Will start with the session from the next login.")
                  : tr("Will not start with the session."),
               false);
        return;
    }
    // The switch goes back, because the file is the state and the file did not
    // change. A checkbox that stayed on after a failed write would be a lie.
    m_updating = true;
    m_ui->autostart->setChecked(!on);
    m_updating = false;
    report(QStringLiteral("autostart → %1").arg(error), true);
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
    m_connected = connected;
    // A refusal outlives a change of connection state: it is the answer to
    // something the person did, and it has not been read yet.
    if (!m_holdingRefusal)
        showConnectionState();
    setCommandsEnabled(connected);
}

QString TrayWindow::connectionText() const
{
    // Short, because this line is read at a glance and shares its room with the
    // daemon's replies. The socket and the group are named in the tooltip: they
    // are what a diagnosis needs and not what a glance does.
    return m_connected ? tr("Connected") : tr("The daemon is not answering");
}

QString TrayWindow::connectionDetail() const
{
    if (m_connected)
        return tr("The daemon is answering on %1.").arg(m_client.socketPath());
    return tr("Nothing is answering on %1. The daemon may not be running, or you may not be in "
              "the %2 group.")
            .arg(m_client.socketPath(), QString::fromLatin1(kControlSocketGroup));
}

void TrayWindow::showConnectionState()
{
    m_holdingRefusal = false;
    m_statusFull = connectionText();
    m_ui->status->setToolTip(connectionDetail());
    paintStatus();
}

QString TrayWindow::statusText() const
{
    return m_statusFull;
}

void TrayWindow::paintStatus()
{
    // Elided against the room the label actually has, which is why this is done
    // here and again on every resize rather than once when the text arrives.
    const QMargins margins = m_ui->status->contentsMargins();
    const int room = m_ui->status->width() - margins.left() - margins.right();
    m_ui->status->setText(room > 0
                                  ? m_ui->status->fontMetrics().elidedText(
                                            m_statusFull, Qt::ElideRight, room)
                                  : m_statusFull);
}

void TrayWindow::setCommandsEnabled(bool enabled)
{
    for (QWidget *group : std::as_const(m_ui->commands))
        group->setEnabled(enabled);
}

void TrayWindow::sendCommand(const QString &command)
{
    const QString reply = m_client.sendAndRefresh(command);
    report(QStringLiteral("%1 → %2")
                   .arg(command, reply.isEmpty() ? tr("no reply") : reply),
           reply.startsWith(QLatin1String("err")) || reply.isEmpty());
}

void TrayWindow::report(const QString &text, bool refused)
{
    m_statusFull = text;
    // The whole of it behind the line, because what is elided from a refusal is
    // usually the part that says which value was wrong.
    m_ui->status->setToolTip(text);
    paintStatus();
    m_holdingRefusal = refused;
    // A refusal stays until the next command: it is worth reading, and five
    // seconds is not long enough to be sure it was. An acceptance is a
    // confirmation and goes away by itself, which is what keeps the line from
    // becoming a log nobody clears.
    if (refused) {
        m_statusTimer->stop();
        return;
    }
    m_statusTimer->start(kReplyLingerMs);
}

#include "tray_window.moc"

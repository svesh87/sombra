// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "tray_icon.h"

#include <QIconEngine>
#include <QPainter>
#include <QPixmap>

#include <algorithm>
#include <utility>

namespace {

// QIcon::fromTheme() may resolve a specific name to a generic alias, and
// QSystemTrayIcon then publishes that alias over StatusNotifierItem — which is
// how a panel ends up drawing one glyph for every state. Keeping the name we
// asked for is the whole point of asking for it.
class NamedIcon final : public QIconEngine {
public:
    NamedIcon(QString name, QIcon resolved)
        : m_name(std::move(name))
        , m_resolved(std::move(resolved))
    {
    }

    QIconEngine *clone() const override { return new NamedIcon(m_name, m_resolved); }
    QString iconName() override { return m_name; }
    void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode,
               QIcon::State state) override
    {
        m_resolved.paint(painter, rect, Qt::AlignCenter, mode, state);
    }
    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override
    {
        return m_resolved.pixmap(size, mode, state);
    }

private:
    QString m_name;
    QIcon m_resolved;
};

// Drawn on a 64-unit grid and scaled by the panel. The body is the identity of
// the thing; the pins are what makes it a chip at 22 pixels rather than a
// rounded square.
constexpr qreal kBody = 14;
constexpr qreal kBodySize = 36;
// Short on purpose. The pins say "chip"; the body is where the level has to be
// legible at 22 pixels, so it gets the room.
constexpr qreal kPinLength = 6;
constexpr qreal kPinWidth = 5;
constexpr qreal kStroke = 5;
// The gap between the outline and the fill. Wide enough that the outline stays
// a line at panel size rather than merging with a full fill into one blob.
constexpr qreal kFillInset = 6;
constexpr qreal kCornerRadius = 5;

// Saturated mid-tones on purpose. A near-black glyph disappears on a dark
// panel and a near-white one on a light panel; these read on both, which is the
// whole reason the colour is not taken from the palette.
//
// The whole picture is drawn in one of them, outline and fill alike. A second
// colour for the fill would have to be light or dark, and either choice is a
// bet on the panel — the same bet the palette was making.
const QColor kRunning(74, 144, 217);   // blue: the daemon is answering
const QColor kGuard(224, 74, 63);      // red: the emergency guard is clamping
const QColor kAbsent(150, 156, 162);   // grey: nothing is answering

// How much of the body the fill occupies, from the coolest mode to the widest.
// A mode that is not known yet leaves the body empty rather than guessing at
// the middle.
qreal fillFraction(int modeIndex, int modeCount)
{
    if (modeCount <= 0 || modeIndex < 0)
        return 0;
    const int position = std::min(modeIndex, modeCount - 1) + 1;
    return static_cast<qreal>(position) / static_cast<qreal>(modeCount);
}

} // namespace

QString trayIconName(bool connected, bool guardEngaged, int modeIndex, int modeCount)
{
    if (!connected)
        return QStringLiteral("action-unavailable-symbolic");
    if (guardEngaged)
        return QStringLiteral("dialog-warning-symbolic");
    if (modeCount <= 0 || modeIndex < 0)
        return {};
    // By position on the scale rather than by name: the modes run from the
    // coolest envelope to the widest, and an id is the maintainer's word rather
    // than a promise about where it sits.
    if (modeIndex == 0)
        return QStringLiteral("power-profile-power-saver-symbolic");
    if (modeIndex >= modeCount - 1)
        return QStringLiteral("power-profile-performance-symbolic");
    return QStringLiteral("power-profile-balanced-symbolic");
}

namespace {
QIcon drawnIcon(bool connected, bool guardEngaged, int modeIndex, int modeCount)
{
    const QColor colour = !connected ? kAbsent : (guardEngaged ? kGuard : kRunning);

    QPixmap pixmap(64, 64);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);

    // The body as an outline, so the level inside it has somewhere to rise from.
    const QRectF body(kBody, kBody, kBodySize, kBodySize);
    painter.setPen(QPen(colour, kStroke, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(body, kCornerRadius, kCornerRadius);

    // Three pins on each side, spread across the body rather than placed at
    // measured offsets, so the geometry follows the constants above.
    painter.setPen(QPen(colour, kPinWidth, Qt::SolidLine, Qt::FlatCap));
    for (const int pin : { 1, 2, 3 }) {
        const qreal along = kBody + kBodySize * pin / 4;
        painter.drawLine(QPointF(along, kBody - kPinLength), QPointF(along, kBody));
        painter.drawLine(QPointF(along, kBody + kBodySize),
                         QPointF(along, kBody + kBodySize + kPinLength));
        painter.drawLine(QPointF(kBody - kPinLength, along), QPointF(kBody, along));
        painter.drawLine(QPointF(kBody + kBodySize, along),
                         QPointF(kBody + kBodySize + kPinLength, along));
    }

    const qreal available = kBodySize - 2 * kFillInset;
    painter.setPen(Qt::NoPen);
    painter.setBrush(colour);

    if (!connected) {
        // A bar across the middle rather than a diagonal over the whole
        // picture: at panel size a diagonal reads as a smudge, and what this
        // says is that the level is unknown rather than zero.
        painter.drawRect(QRectF(kBody + kFillInset, kBody + (kBodySize - kStroke) / 2,
                                available, kStroke));
        return QIcon(pixmap);
    }

    // The level, filled from the bottom: more power is more of it, which is the
    // one thing about a mode that does not need a word.
    const qreal fraction = fillFraction(modeIndex, modeCount);
    if (fraction > 0) {
        const qreal height = available * fraction;
        painter.drawRect(QRectF(kBody + kFillInset, kBody + kBodySize - kFillInset - height,
                                available, height));
    }
    return QIcon(pixmap);
}
} // namespace

QIcon trayIcon(bool connected, bool guardEngaged, int modeIndex, int modeCount)
{
    const QString name = trayIconName(connected, guardEngaged, modeIndex, modeCount);
    // hasThemeIcon() rather than the emptiness of what fromTheme() returns:
    // fromTheme() answers with a usable icon for a name the theme does not have,
    // and publishing that name is what puts a question mark in the panel.
    if (!name.isEmpty() && QIcon::hasThemeIcon(name))
        return QIcon(new NamedIcon(name, QIcon::fromTheme(name)));
    return drawnIcon(connected, guardEngaged, modeIndex, modeCount);
}

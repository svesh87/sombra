// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "monitor_bus.h"

#include <QCryptographicHash>

namespace {

// Everything outside the set becomes one underscore, and a run of them collapses:
// "DELL U3425WE" is one word with a space in it, not two words worth keeping apart.
QString sanitised(const QString &text)
{
    QString out;
    out.reserve(text.size());
    for (const QChar c : text) {
        const bool keep = c.isLetterOrNumber() || c == QLatin1Char('.') || c == QLatin1Char('_')
                || c == QLatin1Char('-');
        if (keep && c.unicode() < 128) {
            out.append(c);
        } else if (!out.endsWith(QLatin1Char('_'))) {
            out.append(QLatin1Char('_'));
        }
    }
    while (out.endsWith(QLatin1Char('_')))
        out.chop(1);
    while (out.startsWith(QLatin1Char('_')))
        out.remove(0, 1);
    return out;
}

} // namespace

QString monitorId(const QString &vendor, const QString &model, const QString &serial,
                  const QString &connector)
{
    const QString tail = serial.trimmed().isEmpty() ? connector : serial;
    QStringList parts;
    for (const QString &part : { vendor, model, tail }) {
        const QString clean = sanitised(part);
        if (!clean.isEmpty())
            parts.append(clean);
    }
    // A monitor that gave nothing readable still has to be addressable, and the
    // connector is the one thing always known about it.
    if (parts.isEmpty())
        return sanitised(connector);
    return parts.join(QLatin1Char('-'));
}

int monitorSlot(const QString &connector, const QStringList &externalConnectors)
{
    const int index = externalConnectors.indexOf(connector);
    return index >= 0 && index < 3 ? index : -1;
}

uint8_t monitorFingerprint(const QString &id)
{
    if (id.isEmpty())
        return 0;
    const QByteArray digest =
            QCryptographicHash::hash(id.toUtf8(), QCryptographicHash::Sha256);
    // Zero is "the slot is empty", so a monitor whose digest starts with a zero
    // byte takes the next value up rather than reading as nothing at all.
    const uint8_t value = static_cast<uint8_t>(digest.at(0));
    return value == 0 ? 1 : value;
}

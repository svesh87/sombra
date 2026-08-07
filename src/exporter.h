// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#pragma once

#include "metrics_ring.h"

#include <QString>
#include <QTextStream>

// Renders the ring as text.
//
// This is the only interface to the stored samples for analysis, which is what
// lets the ring format change without breaking anything that reads it. Nothing
// here averages or downsamples: the peaks are the point.
namespace exporter {

enum class Format {
    Csv,
    Json,
};

// Column order for the CSV form. Every live setpoint appears alongside the
// readings, because mode and setpoint transitions are derived by diffing
// consecutive samples rather than from a separate event log.
QString csvHeader();

// One record. Absent readings are rendered as an empty field, never as zero.
QString csvRow(const Sample &sample);

QString jsonRow(const Sample &sample);

// Writes the samples whose timestamp falls inside [from, to]. A `to` of zero
// means "no upper bound". Slots that were never written are skipped. Returns
// the number of records written.
quint64 write(const MetricsRing &ring, QTextStream &out, Format format, quint32 from, quint32 to);

} // namespace exporter

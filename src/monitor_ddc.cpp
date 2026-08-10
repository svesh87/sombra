// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "monitor_ddc.h"

#include <QFileInfo>
#include <QLoggingCategory>

#include <ddcutil_c_api.h>
#include <ddcutil_status_codes.h>

#include <algorithm>

namespace {

constexpr DDCA_Vcp_Feature_Code kBrightness = 0x10;

// The library's own verification reads the value back at once, and one of the
// monitors measured here is not ready that soon. Off once per process; the reading
// that confirms a write is the next ordinary one.
void ensureVerifyDisabled()
{
    static const bool once = [] {
        ddca_enable_verify(false);
        return true;
    }();
    Q_UNUSED(once);
}

QString trimmedField(const char *text)
{
    return QString::fromLatin1(text).trimmed();
}

int percentOf(int value, int maximum)
{
    if (maximum <= 0)
        return 0;
    return std::clamp((value * 100 + maximum / 2) / maximum, 0, 100);
}

int rawOf(int percent, int maximum)
{
    return std::clamp((std::clamp(percent, 0, 100) * maximum + 50) / 100, 0, maximum);
}

} // namespace

MonitorDdcBus::MonitorDdcBus(Sysfs &sysfs) : m_sysfs(sysfs)
{
    ensureVerifyDisabled();
}

QString MonitorDdcBus::connectorForBus(int busno)
{
    const QString wanted = QStringLiteral("i2c-%1").arg(busno);
    for (const QString &relative :
         m_sysfs.glob(QStringLiteral("sys/class/drm"), QStringLiteral("card*-*"))) {
        const QFileInfo link(m_sysfs.absolute(relative) + QStringLiteral("/ddc"));
        if (!link.exists())
            continue;
        if (QFileInfo(link.symLinkTarget()).fileName() == wanted)
            return relative.section(QLatin1Char('/'), -1);
    }
    return QString();
}

QList<MonitorInfo> MonitorDdcBus::detect()
{
    m_references.clear();
    m_maximum.clear();

    DDCA_Display_Info_List *list = nullptr;
    // Invalid displays excluded: the internal panel is one of them, and it says so
    // itself — a laptop panel does not answer DDC/CI.
    const DDCA_Status status = ddca_get_display_info_list2(false, &list);
    if (status != 0 || !list) {
        qWarning().noquote() << "monitors: cannot list displays:" << ddca_rc_name(status);
        return {};
    }

    QList<MonitorInfo> found;
    for (int i = 0; i < list->ct; ++i) {
        const DDCA_Display_Info &entry = list->info[i];
        MonitorInfo info;
        info.vendor = trimmedField(entry.mfg_id);
        info.model = trimmedField(entry.model_name);
        info.serial = trimmedField(entry.sn);
        info.connector = entry.path.io_mode == DDCA_IO_I2C
                ? connectorForBus(entry.path.path.i2c_busno)
                : QString();
        info.id = monitorId(info.vendor, info.model, info.serial, info.connector);
        // The maximum comes from the monitor, on the first read below. Until then
        // the scale is assumed to be the usual one; every monitor measured here
        // reported 100 and none of them lied about it.
        info.maximum = 100;
        m_references.insert(info.id, entry.dref);
        found.append(info);
    }
    ddca_free_display_info_list(list);
    return found;
}

std::optional<int> MonitorDdcBus::read(const QString &id)
{
    const auto reference = m_references.constFind(id);
    if (reference == m_references.constEnd())
        return std::nullopt;

    DDCA_Display_Handle handle = nullptr;
    if (ddca_open_display2(*reference, false, &handle) != 0 || !handle)
        return std::nullopt;

    DDCA_Non_Table_Vcp_Value value {};
    const DDCA_Status status = ddca_get_non_table_vcp_value(handle, kBrightness, &value);
    ddca_close_display(handle);
    if (status != 0)
        return std::nullopt;

    const int maximum = (value.mh << 8) | value.ml;
    const int current = (value.sh << 8) | value.sl;
    m_maximum.insert(id, maximum > 0 ? maximum : 100);
    return percentOf(current, m_maximum.value(id));
}

bool MonitorDdcBus::write(const QString &id, int percent)
{
    const auto reference = m_references.constFind(id);
    if (reference == m_references.constEnd())
        return false;

    DDCA_Display_Handle handle = nullptr;
    if (ddca_open_display2(*reference, false, &handle) != 0 || !handle)
        return false;

    const int maximum = m_maximum.value(id, 100);
    const int raw = rawOf(percent, maximum);
    const DDCA_Status status = ddca_set_non_table_vcp_value(
            handle, kBrightness, static_cast<uint8_t>(raw >> 8), static_cast<uint8_t>(raw & 0xff));
    ddca_close_display(handle);

    // The status is not the answer and is not logged as a failure. A monitor
    // measured here reports DDCRC_RETRIES and applies the value anyway; what
    // decides is the read that follows. Anything the library says here would fill
    // a log with failures that did not happen.
    return status == 0 || status == DDCRC_RETRIES;
}

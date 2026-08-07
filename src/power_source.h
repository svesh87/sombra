// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#pragma once

#include "sysfs.h"

#include <QString>

#include <optional>

// AC/battery state and the charge threshold.
//
// Both are responsibilities inherited along with sole ownership of the power
// controls. The vendor daemon that reacts to a mains transition and holds the
// charge limit is masked, so nothing else does either of them.
class PowerSource {
public:
    explicit PowerSource(Sysfs &sysfs);

    void resolve();

    // False when no mains supply node exists, which is also the safe answer:
    // it is the state with the smaller thermal budget.
    bool onAc() const;

    std::optional<int> chargeLimit() const;
    bool setChargeLimit(int percent);

    bool hasChargeControl() const { return !m_chargeLimitPath.isEmpty(); }

private:
    Sysfs &m_sysfs;
    QString m_onlinePath;
    QString m_chargeLimitPath;
};

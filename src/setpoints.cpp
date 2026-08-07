// Copyright (c) 2026 Sergei Sveshnikov. MIT licensed, see LICENSE.
#include "setpoints.h"

#include <algorithm>

bool Setpoints::operator==(const Setpoints &other) const
{
    return profile == other.profile && pl1W == other.pl1W && pl2W == other.pl2W
            && maxPerfPct == other.maxPerfPct && noTurbo == other.noTurbo && epp == other.epp
            && hwpBoost == other.hwpBoost && gtMaxMhz == other.gtMaxMhz
            && fanMode == other.fanMode;
}

Setpoints safeExitSetpoints()
{
    Setpoints exit;
    exit.profile = Profile::Balanced;
    exit.pl1W = kMaxConfirmedPl1W;
    exit.pl2W = kMaxConfirmedPl2W;
    exit.maxPerfPct = 100;
    exit.noTurbo = false;
    exit.epp = Epp::BalancePerformance;
    exit.hwpBoost = true;
    exit.gtMaxMhz = kMaxGtMhz;
    exit.fanMode = 2;
    return exit;
}

Setpoints applyGuard(const Sample &sample, const Setpoints &requested, GuardState &state)
{
    Setpoints effective = requested;
    const bool haveTemp = sample.pkgTemp != kAbsentTemp;

    if (haveTemp) {
        if (sample.pkgTemp >= kGuardClampTempDeci) {
            state.engaged = true;
            state.coolTicks = 0;
        }
        if (sample.pkgTemp >= kGuardFanMaxTempDeci)
            state.fanForced = true;

        if (state.engaged) {
            if (sample.pkgTemp <= kGuardReleaseTempDeci) {
                if (++state.coolTicks >= kGuardReleaseTicks) {
                    state.engaged = false;
                    state.fanForced = false;
                    state.coolTicks = 0;
                }
            } else {
                state.coolTicks = 0;
            }
        }
    }
    // Without a package temperature the guard neither escalates nor releases:
    // releasing on a reading that does not exist would be the one failure the
    // interlock must not have. The daemon separately falls back to the most
    // conservative mode when sensors are missing.

    if (state.engaged) {
        effective.pl1W = std::min(effective.pl1W, kGuardPl1W);
        effective.pl2W = std::min(effective.pl2W, kGuardPl2W);
        effective.maxPerfPct = std::min(effective.maxPerfPct, kMinPerfPct);
        effective.noTurbo = true;
        effective.epp = Epp::Power;
        effective.hwpBoost = false;
        effective.gtMaxMhz = std::min(effective.gtMaxMhz, kMinGtMhz);
    }
    if (state.fanForced)
        effective.fanMode = 0;

    return effective;
}

QString eppToString(Epp epp)
{
    switch (epp) {
    case Epp::Default:
        return QStringLiteral("default");
    case Epp::Performance:
        return QStringLiteral("performance");
    case Epp::BalancePerformance:
        return QStringLiteral("balance_performance");
    case Epp::BalancePower:
        return QStringLiteral("balance_power");
    case Epp::Power:
        return QStringLiteral("power");
    case Epp::Unknown:
        break;
    }
    return QStringLiteral("unknown");
}

Epp eppFromString(const QString &text)
{
    const QString value = text.trimmed().toLower();
    if (value == QLatin1String("default"))
        return Epp::Default;
    if (value == QLatin1String("performance"))
        return Epp::Performance;
    if (value == QLatin1String("balance_performance"))
        return Epp::BalancePerformance;
    if (value == QLatin1String("balance_power"))
        return Epp::BalancePower;
    if (value == QLatin1String("power"))
        return Epp::Power;
    return Epp::Unknown;
}

QString profileToString(Profile profile)
{
    switch (profile) {
    case Profile::Quiet:
        return QStringLiteral("quiet");
    case Profile::Balanced:
        return QStringLiteral("balanced");
    case Profile::Performance:
        return QStringLiteral("performance");
    case Profile::Unknown:
        break;
    }
    return QStringLiteral("unknown");
}

Profile profileFromString(const QString &text)
{
    const QString value = text.trimmed().toLower();
    if (value == QLatin1String("quiet"))
        return Profile::Quiet;
    if (value == QLatin1String("balanced"))
        return Profile::Balanced;
    if (value == QLatin1String("performance"))
        return Profile::Performance;
    return Profile::Unknown;
}

QString markToString(Mark mark)
{
    switch (mark) {
    case Mark::Good:
        return QStringLiteral("good");
    case Mark::TooLoud:
        return QStringLiteral("too-loud");
    case Mark::TooSlow:
        return QStringLiteral("too-slow");
    case Mark::None:
        break;
    }
    return QStringLiteral("none");
}

Mark markFromString(const QString &text)
{
    const QString value = text.trimmed().toLower();
    if (value == QLatin1String("good"))
        return Mark::Good;
    if (value == QLatin1String("too-loud"))
        return Mark::TooLoud;
    if (value == QLatin1String("too-slow"))
        return Mark::TooSlow;
    return Mark::None;
}

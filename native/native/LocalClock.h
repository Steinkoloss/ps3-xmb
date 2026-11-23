// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QDateTime>

// Local hour and month for per-frame use. QDateTime's local-time conversion goes through
// the C library's time-zone code on every call (noticeable at 60 Hz on several threads),
// so the UTC offset and month are looked up at most once a minute per thread; the hour
// comes from the epoch clock plus that offset.
class LocalClock {
public:
    static double hour() { return state().hour(); }
    static unsigned month() { state().hour(); return state().month; }
private:
    struct State {
        qint64 refreshAt = 0, offsetMs = 0;
        unsigned month = 1;
        double hour() {
            const auto now = QDateTime::currentMSecsSinceEpoch();
            if (now >= refreshAt) {
                const auto local = QDateTime::fromMSecsSinceEpoch(now);
                offsetMs = qint64(local.offsetFromUtc()) * 1000;
                month = unsigned(local.date().month());
                refreshAt = now + 60'000;
            }
            constexpr qint64 day = 86'400'000;
            return double(((now + offsetMs) % day + day) % day) / 3.6e6;
        }
    };
    static State& state() { static thread_local State s; return s; }
};

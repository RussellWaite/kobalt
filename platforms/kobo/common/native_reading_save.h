#pragma once

#include "support/qt_compat.h"
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QElapsedTimer>
#include <QVariantMap>
#include <deque>
#include <functional>
#include <utility>
#include <vector>

namespace kobalt::nickel {
class NativeReadingTime {
public:
    template <class Commit, class Verify> bool save(Commit commit, Verify verify) {
        if (uncertain)
            return false;
        if (!committed) {
            if (!commit(expected))
                return false;
            committed = true;
        }
        return verify(expected);
    }
    bool committed = false;
    // A failed rollback leaves the transaction outcome unknown. Never replay
    // the increment merely because a later retry can obtain the database.
    bool uncertain = false;

private:
    QVariantMap expected;
};
// GUI-thread queue. An interval is removed only after both operations succeed.
// The writer retains its own commit state, so retrying verification cannot add
// the interval twice. Owners must finish the queue before releasing storage.
// Both operations report through their completion, so on a device neither
// has to run on the GUI thread: a tap can arrive while they write.
class NativeReadingSave final : public QObject {
public:
    using Completion = std::function<void(bool)>;
    using Flush = std::function<void(Completion)>;
    using Write = std::function<void(Completion)>;
    explicit NativeReadingSave(int retryMilliseconds = 100, int timeoutMilliseconds = 2000)
        : retryMilliseconds(retryMilliseconds), timeoutMilliseconds(timeoutMilliseconds) {
        elapsed.invalidate();
        timer.setSingleShot(true);
        connect(&timer, &QTimer::timeout, this, [this] { advance(); });
    }
    void append(Flush flush, Write write) {
        intervals.push_back({std::move(flush), std::move(write), false});
        if (!elapsed.isValid())
            elapsed.start();
        if (!active && !timer.isActive())
            timer.start(0);
    }
    bool pending() const {
        return !intervals.empty();
    }
    bool inFlight() const {
        return active;
    }
    void finish(Completion done) {
        if (!pending()) {
            done(true);
            return;
        }
        completions.push_back(std::move(done));
        if (!active) {
            elapsed.start();
            timer.start(0);
        }
    }

private:
    void retry() {
        if (elapsed.elapsed() < timeoutMilliseconds) {
            timer.start(retryMilliseconds);
            return;
        }
        elapsed.start();
        // Sleep has no Retry button. Keep retrying retained data at a slower
        // rate so a transient failure cannot prevent suspend forever after
        // storage recovers. Close callers still receive the failed attempt.
        timer.start(1000);
        const auto ready = std::exchange(completions, {});
        // Keep the interval and its commit phase for the next attempt.
        const QPointer<NativeReadingSave> guard(this);
        for (const auto &done : ready) {
            if (!guard)
                break;
            done(false);
        }
    }
    struct Interval {
        Flush flush;
        Write write;
        bool flushed;
    };
    void advance() {
        if (active || intervals.empty())
            return;
        if (!intervals.front().flushed) {
            active = true;
            const QPointer<NativeReadingSave> guard(this);
            intervals.front().flush([guard](bool saved) {
                if (!guard)
                    return;
                guard->active = false;
                if (saved)
                    guard->intervals.front().flushed = true;
                if (saved)
                    guard->timer.start(0);
                else
                    guard->retry();
            });
            return;
        }
        active = true;
        const QPointer<NativeReadingSave> guard(this);
        intervals.front().write([guard](bool saved) {
            if (!guard)
                return;
            guard->active = false;
            if (saved)
                guard->written();
            else
                guard->retry();
        });
    }
    void written() {
        intervals.pop_front();
        if (!intervals.empty()) {
            timer.start(0);
            return;
        }
        const auto ready = std::exchange(completions, {});
        elapsed.invalidate();
        // A completion can destroy this queue. Do not access members afterward.
        const QPointer<NativeReadingSave> guard(this);
        for (const auto &done : ready) {
            if (!guard)
                break;
            done(true);
        }
    }
    const int retryMilliseconds;
    const int timeoutMilliseconds;
    QTimer timer;
    QElapsedTimer elapsed;
    std::deque<Interval> intervals;
    std::vector<Completion> completions;
    bool active = false;
};
}

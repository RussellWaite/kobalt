#include "platforms/kobo/common/native_reading_save.h"
#include "check.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <cstdio>
#include <memory>

using kobalt::nickel::NativeReadingSave;
using kobalt::nickel::NativeReadingTime;
namespace {
void waitFor(const std::function<bool()> &ready) {
    QElapsedTimer timer;
    timer.start();
    while (!ready() && timer.elapsed() < 3000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    require(ready(), "Timed out waiting for native save");
}
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    {
        NativeReadingTime time;
        int attempts = 0, stored = 29, verifies = 0;
        const auto commit = [&](QVariantMap &expected) {
            if (++attempts == 1)
                return false;
            stored += 15;
            expected.insert("time", stored);
            return true;
        };
        const auto verify = [&](const QVariantMap &expected) {
            require(expected.value("time").toInt() == 44, "Verification lost its original committed target");
            return ++verifies > 1;
        };
        require(!time.save(commit, verify) && !time.committed && stored == 29,
                "Busy transaction changed or consumed the delta");
        require(!time.save(commit, verify) && time.committed && stored == 44,
                "Failed readback lost the successful commit");
        require(time.save(commit, verify) && attempts == 2 && stored == 44,
                "Readback retry added the interval twice");
    }
    {
        NativeReadingTime time;
        int attempts = 0;
        auto commit = [&](QVariantMap &) { ++attempts; time.uncertain = true; return false; };
        require(!time.save(commit, [](const QVariantMap &) { return true; }), "Uncertain commit succeeded");
        require(!time.save(commit, [](const QVariantMap &) { return true; }) && attempts == 1,
                "Unknown rollback outcome replayed an increment");
    }
    {
        NativeReadingSave saves(1, 20);
        NativeReadingSave::Completion flush;
        int writes = 0, completions = 0;
        saves.append([&](auto done) { flush = std::move(done); }, [&] { ++writes; return true; });
        saves.finish([&](bool saved) { require(saved, "Held flush failed"); ++completions; });
        saves.finish([&](bool saved) { require(saved, "Repeated finish failed"); ++completions; });
        waitFor([&] { return bool(flush); });
        bool heartbeat = false;
        kobalt::qt::invoke(&app, [&] { heartbeat = true; }, Qt::QueuedConnection);
        waitFor([&] { return heartbeat; });
        require(saves.pending() && writes == 0 && completions == 0, "Finish passed an unfinished event flush");
        saves.append([](auto done) { done(true); }, [&] { ++writes; return true; });
        flush(true);
        waitFor([&] { return completions == 2; });
        require(!saves.pending() && writes == 2, "Queued intervals were lost or duplicated");
    }
    {
        NativeReadingSave saves(1, 10);
        bool writable = false, completed = false, result = true;
        int flushes = 0, writes = 0;
        saves.append([&](auto done) { ++flushes; done(true); }, [&] { ++writes; return writable; });
        saves.finish([&](bool saved) { result = saved; completed = true; });
        waitFor([&] { return completed; });
        require(!result && saves.pending() && flushes == 1, "Timeout discarded the pending interval");
        writable = true;
        completed = false;
        saves.finish([&](bool saved) { result = saved; completed = true; });
        waitFor([&] { return completed; });
        require(result && !saves.pending() && flushes == 1 && writes > 1,
                "Explicit retry repeated the event flush or lost the delta");
    }
    {
        NativeReadingSave saves(1, 10);
        bool writable = false;
        int attempts = 0;
        saves.append([](auto done) { done(true); }, [&] { ++attempts; return writable; });
        waitFor([&] { return attempts > 1; });
        QElapsedTimer timeout;
        timeout.start();
        waitFor([&] { return timeout.elapsed() > 30; });
        require(saves.pending(), "Failed sleeping interval disappeared");
        writable = true;
        waitFor([&] { return !saves.pending(); });
    }
    {
        auto saves = std::make_unique<NativeReadingSave>(1, 10);
        NativeReadingSave::Completion flush;
        bool wrote = false, completed = false;
        saves->append([&](auto done) { flush = std::move(done); }, [&] { wrote = true; return true; });
        saves->finish([&](bool) { completed = true; });
        waitFor([&] { return bool(flush); });
        saves.reset();
        flush(true);
        QCoreApplication::processEvents();
        require(!wrote && !completed, "Destroyed queue performed late work or called its owner");
    }
    std::puts("PASS: native interval retry, commit verification, flush ordering, responsiveness and cancellation");
}

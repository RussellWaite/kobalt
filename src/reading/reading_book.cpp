#include "support/strings.h"
#include "reading_book.h"
#include "appearance_settings.h"
#include "reader_session.h"
#include "storage/reader_storage.h"
#include "support/diagnostics.h"
#include "support/qt_compat.h"
#include <QCoreApplication>

namespace kobalt {
ReadingBook::ReadingBook(std::unique_ptr<ReaderSession> session, QString statePath, storage::Paths paths,
                         QString identity, QByteArray digest, std::unique_ptr<ReadingBridge> bridge,
                         ReadingProgress baseline, ReadingProgress diskBaseline)
    : model(std::move(session)), statePath(std::move(statePath)), identity_(std::move(identity)),
      paths_(std::move(paths)), digest(std::move(digest)), bridge(std::move(bridge)), baseline(std::move(baseline)),
      reconciliation(std::move(diskBaseline)) {
    const auto saved = storage::readBookState(paths_, identity_, this->statePath).object;
    const auto preferences = appearance::readerPreferences(
        storage::readPreferences(paths_.preferences()).object, saved.value("book").toString() == identity_ ? saved : QJsonObject());
    for (const auto *key : appearance::layoutKeys)
        if (preferences.contains(key))
            layoutPreferences_[key] = preferences.value(key);
    if (saved.value("book").toString() == identity_)
        dictionaryChoice_ = saved.value("dictionary_id").toString();
    // Qt 5.2 leaves a default-constructed QElapsedTimer uninitialized, so
    // isValid() is not reliable until the timer is invalidated or started.
    progressPoll.invalidate();
    activeReading.invalidate();
    speedSaved.invalidate();
    if (!this->bridge) {
        speed = std::make_unique<ReadingSpeed>();
        speed->restore(storage::readRecord(paths_.directory, "reading-speed", paths_.readingSpeed()).object);
    }
}
ReadingBook::~ReadingBook() {
    saveSpeed(true);
    // Finish the last queued position before the profile can be released.
    writer.reset();
}
bool ReadingBook::savesPending() const {
    return writer && !writer->idle();
}
bool ReadingBook::deviceTransition() const {
    return bridge && bridge->deviceTransition();
}
bool ReadingBook::canWriteNative() const {
    return bridge && bridge->writable();
}
void ReadingBook::releaseNative() {
    stopReading();
    bridge.reset();
    released = true;
}
void ReadingBook::observeReading(bool learnPace) {
    if (released || (bridge && !bridge->writable())) {
        stopReading();
        return;
    }
    if (model->renderProgress().active) {
        pauseLocalPace();
    }
    if (model->busy())
        return;
    if (model->image().isNull() || !model->error().isEmpty()) {
        stopReading();
        return;
    }
    if (!activeReading.isValid())
        activeReading.start();
    if (speed) {
        if (learnPace) {
            speed->observe(readingActivity(), readingMilliseconds());
        } else {
            speed->pause();
        }
    }
    if (bridge)
        bridge->observeReading(readingActivity());
}
ReadingActivity ReadingBook::readingActivity() const {
    return {model->state().value("position").toString(),
            100.0 * model->page() / qMax(1, model->pages()),
            model->page(),
            model->pages(),
            model->turnDirection(),
            model->totalWords(),
            model->chapterGraph(),
            model->graphSelection()};
}
ReadingEstimates ReadingBook::estimates() const {
    if (speed) {
        return speed->estimates(readingActivity());
    }
    if (bridge && !released && bridge->writable()) {
        ReadingEstimates result;
        if (bridge->estimates(readingActivity(), &result))
            return result;
    }
    const auto reading = readingMilliseconds();
    const int chapterLeft = model->chapterPages() > 0 ? model->chapterPages() - model->chapterPage() + 1 : -1;
    return {pace_.estimate(chapterLeft, reading), pace_.estimate(model->pages() - model->page(), reading),
            pace_.estimate(0, reading) < 0};
}
void ReadingBook::stopReading() {
    if (activeReading.isValid()) {
        completedReadingMilliseconds += activeReading.elapsed();
        activeReading.invalidate();
    }
    if (bridge)
        bridge->stopReading();
    if (speed) {
        speed->pause();
        saveSpeed(true);
    }
}
void ReadingBook::saveSpeed(bool force) {
    if (!speed || !speed->dirty() || (!force && speedSaved.isValid() && speedSaved.elapsed() < 60000)) {
        return;
    }
    speedSaved.start();
    if (storage::writeRecord(paths_.directory, "reading-speed", paths_.readingSpeed(), speed->state())) {
        speed->saved();
    } else {
        diagnostic("reading-speed-unsaved", {}, false);
    }
}
qint64 ReadingBook::readingMilliseconds() const {
    return completedReadingMilliseconds + (activeReading.isValid() ? activeReading.elapsed() : 0);
}
bool ReadingBook::offerFinish() const {
    if (released || (bridge && !bridge->writable()) || model->busy() || model->pages() <= 0 ||
        !model->error().isEmpty())
        return false;
    const auto anchor = model->state().value("position").toString();
    if (anchor.isEmpty() || anchor == acceptedFinishedAnchor)
        return false;
    // Nickel asks on exit at 90%. Include the final page of very short books,
    // and the last page of the text before trailing non-linear sections.
    return model->page() * 10 >= model->pages() * 9 || model->nextPage() == model->page();
}
void ReadingBook::markFinished() {
    if (offerFinish())
        acceptedFinishedAnchor = model->state().value("position").toString();
}
ReadingProgress ReadingBook::readProgress() const {
    return bridge ? bridge->read() : ReadingProgress{};
}
void ReadingBook::observeProgress() {
    if (bridge)
        reconciliation.observe(bridge->read());
}
void ReadingBook::pollProgress() {
    if (!bridge)
        return;
    // Nickel's saved position changes after sync, account updates, USB and
    // wake. Check then, and otherwise once a minute.
    if (!bridge->takeChange() && progressPoll.isValid() && progressPoll.elapsed() < 60000)
        return;
    observeProgress();
    progressPoll.restart();
    if (!reconciliation.pending() && reconciliation.baseline().valid)
        baseline = reconciliation.baseline();
}
void ReadingBook::acknowledge(const ReadingProgress &shown, bool moved) {
    reconciliation.acknowledge(shown);
    baseline = reconciliation.baseline();
    acceptedFinishedAnchor = moved && shown.status == 2 ? model->state().value("position").toString() : QString();
}
bool ReadingBook::writeNative(const QString &location, const QString &mappingError) {
    if (!bridge && !released) {
        nativeWriteback = "local-only";
        nativeWritebackError.clear();
        return true;
    }
    // Some plain EPUB files have positions Nickel cannot store. Record the
    // time, status and percentage; Nickel keeps its own position. Kobo's cloud
    // does not sync sideloaded EPUB files, so no other device sees the old
    // position with a new time. Other books save only locally when their
    // position cannot be mapped.
    const bool timeOnly = !mappingError.isEmpty();
    QString error = timeOnly && !plainEpub() ? mappingError : QString();
    nativeWriteback = "local-only";
    if (error.isEmpty() && canWriteNative()) {
        const auto current = bridge->read(&error);
        reconciliation.observe(current);
        if (reconciliation.pending())
            return false;
        if (reconciliation.mayWrite(current)) {
            ReadingProgress verified;
            const double percent = 100.0 * model->page() / qMax(1, model->pages());
            const bool preserveFinished = !acceptedFinishedAnchor.isEmpty() &&
                                          acceptedFinishedAnchor == model->state().value("position").toString();
            diagnostic("native-save-start");
            if (bridge->write(current, timeOnly ? QString() : location, percent, preserveFinished, readingActivity(),
                              &verified, &error)) {
                baseline = verified;
                reconciliation = ProgressReconciler(verified);
                nativeWriteback = timeOnly ? "time-only" : "verified";
            } else {
                // A commit can lose a race after the earlier read. Ask about
                // that update before letting the window finish its exit.
                observeProgress();
                if (reconciliation.pending() && canWriteNative())
                    return false;
            }
        } else if (error.isEmpty())
            error = strings::get("error.nickel_progress_changed_before_exit");
    } else if (error.isEmpty())
        error = strings::get("error.nickel_save_deferred_while_progress_or_device_state_is_unresolved");
    nativeWritebackError = error;
    diagnostic("native-save",
               {{"result", nativeWriteback}, {"mapping_failed", timeOnly}, {"failed", !error.isEmpty()}});
    return true;
}
bool ReadingBook::saveDictionaryChoice(const QString &id) {
    const auto previous = dictionaryChoice_;
    dictionaryChoice_ = id;
    QJsonObject snapshot;
    if (checkpoint(&snapshot))
        return true;
    dictionaryChoice_ = previous;
    return false;
}
bool ReadingBook::checkpoint(QJsonObject *snapshot, bool background) {
    saveSpeed();
    auto state = model->state();
    state["book"] = identity_;
    state["fingerprint"] = QString::fromLatin1(digest);
    for (auto it = layoutPreferences_.begin(); it != layoutPreferences_.end(); ++it)
        state[it.key()] = it.value();
    if (!dictionaryChoice_.isEmpty())
        state["dictionary_id"] = dictionaryChoice_;
    state["schema"] = 1;
    state["nickel_progress"] = baseline.json();
    state["finished_position"] = acceptedFinishedAnchor;
    // For the Android and desktop library, which shows progress without
    // opening the book.
    if (model->pages() > 0)
        state["percent"] = qBound(0, (model->page() + 1) * 100 / model->pages(), 100);
    state["native_writeback"] = nativeWriteback;
    state["sync_pending"] = reconciliation.pending();
    state["native_writeback_error"] = nativeWritebackError;
    *snapshot = state;
    auto checkpoint = state;
    if (model->temporaryTypography())
        appearance::copy(checkpoint, model->persistentState());
    if (checkpoint == savedCheckpoint || (background && checkpoint == pendingCheckpoint))
        return true;
    auto persisted = checkpoint;
    for (const auto *key : appearance::layoutKeys)
        persisted.remove(key);
    for (const auto *key : appearance::marginKeys)
        persisted.remove(key);
    // The first write of an opening stays synchronous: it can import legacy
    // files. A layout or margin change also stays synchronous, because it
    // updates the global layout memory below. Other checkpoints only change
    // the position and progress.
    const auto layoutChanged = [&] {
        for (const auto *key : appearance::layoutKeys)
            if (checkpoint.value(key) != savedCheckpoint.value(key))
                return true;
        for (const auto *key : appearance::marginKeys)
            if (checkpoint.value(key) != savedCheckpoint.value(key))
                return true;
        return false;
    };
    if (background && !savedCheckpoint.isEmpty() && !layoutChanged()) {
        pendingCheckpoint = checkpoint;
        if (!writer)
            writer = std::make_unique<storage::RecordWriter>();
        const std::weak_ptr<ReadingBook *> guard = alive;
        writer->post(
            [paths = paths_, identity = identity_, persisted, path = statePath] {
                return storage::writeBookState(paths, identity, persisted, path);
            },
            [guard, checkpoint](bool saved) {
                kobalt::qt::invoke(
                    QCoreApplication::instance(),
                    [guard, checkpoint, saved] {
                        if (const auto book = guard.lock())
                            (*book)->finishBackgroundCheckpoint(checkpoint, saved);
                    },
                    Qt::QueuedConnection);
            });
        return true;
    }
    if (writer)
        writer->cancel();
    pendingCheckpoint = {};
    const bool saved = storage::writeBookState(paths_, identity_, persisted, statePath);
    if (saved) {
        if (!model->temporaryTypography()) {
            const auto settings = storage::readPreferences(paths_.preferences());
            if (settings.status == storage::ReadStatus::Ok || settings.status == storage::ReadStatus::Missing) {
                auto next = settings.object;
                appearance::rememberTextLayout(next, state);
                appearance::rememberPageMargins(next, state);
                if (next != settings.object && !kobalt::storage::writePreferences(paths_.preferences(), next)) {
                    diagnostic("appearance-settings-unsaved", {}, false);
                    return false;
                }
            }
        }
        savedCheckpoint = checkpoint;
    } else {
        diagnostic("local-save-failed");
    }
    return saved;
}
void ReadingBook::finishBackgroundCheckpoint(const QJsonObject &checkpoint, bool saved) {
    // A synchronous checkpoint replaced this one while it ran.
    if (checkpoint != pendingCheckpoint)
        return;
    pendingCheckpoint = {};
    if (saved) {
        savedCheckpoint = checkpoint;
        return;
    }
    diagnostic("local-save-failed");
    if (checkpointFailed)
        checkpointFailed();
}
}

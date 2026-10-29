#include "support/strings.h"
#include "reading_book.h"
#include "appearance_settings.h"
#include "session_timeout.h"
#include "reader_session.h"
#include "storage/profile_database.h"
#include "storage/reader_storage.h"
#include "storage/annotation_database.h"
#include "support/diagnostics.h"
#include "support/qt_compat.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QTimer>

namespace kobalt {
ReadingBook::ReadingBook(std::unique_ptr<ReaderSession> session, QString statePath, storage::Paths paths,
                         QString identity, QByteArray digest, std::unique_ptr<ReadingBridge> bridge,
                         ReadingProgress baseline, ReadingProgress diskBaseline, QString sourceKey, ReadingHistory::UtcClock utc)
    : model(std::move(session)), statePath(std::move(statePath)), identity_(std::move(identity)),
      paths_(std::move(paths)), digest(std::move(digest)), bridge(std::move(bridge)),
      nativeCloudSync(this->bridge && !identity_.startsWith("file:///")), baseline(std::move(baseline)),
      reconciliation(std::move(diskBaseline)) {
    this->utc = utc ? std::move(utc) : ReadingHistory::UtcClock([] { return QDateTime::currentMSecsSinceEpoch(); });
    auto saved = storage::readBookState(paths_, identity_, this->statePath).object;
    // The native opener resets its snapshot before the database record is
    // replaced. Apply the same reset before retaining fields or preferences.
    if (this->bridge)
        resetNativeUnread(saved, reconciliation.baseline());
    preservedState = saved;
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
    if (annotationDatabaseEnabled())
        history = std::make_unique<ReadingHistory>(paths_.directory, identity_, this->digest, std::move(sourceKey),
                                                   model->title(), model->author(), this->utc);
    if (!this->bridge) {
        speed = std::make_unique<ReadingSpeed>();
        speed->restore(storage::readRecord(paths_.directory, "reading-speed", paths_.readingSpeed()).object);
    }
}
ReadingBook::~ReadingBook() {
    sleepingReading = false;
    if (history)
        stopReading();
    else
        saveSpeed(true);
    writer.reset();
    speedWriter.reset();
}
bool ReadingBook::savesPending() const {
    return closeCheckpointPending || (writer && !writer->idle()) || (history && history->saving()) ||
           (speedWriter && !speedWriter->idle());
}
bool ReadingBook::deviceTransition() const {
    return bridge && bridge->deviceTransition();
}
bool ReadingBook::canWriteNative() const {
    return bridge && bridge->writable();
}
void ReadingBook::releaseNative(bool save) {
    sleepingReading = false;
    stopReading(true, save);
    ++nativeFinishGeneration;
    const auto callbacks = std::exchange(nativeFinishCallbacks, {});
    bridge.reset();
    released = true;
    // Destroying a native session cancels its callbacks. Release callers that
    // were waiting so their local disposal barrier can retry without it.
    for (const auto &callback : callbacks)
        callback(false);
}
void ReadingBook::finishReading(std::function<void(bool)> done) {
    sleepingReading = false;
    stopReading(true);
    if (!bridge) {
        done(true);
        return;
    }
    nativeFinishCallbacks.append(std::move(done));
    if (nativeFinishCallbacks.size() > 1)
        return;
    const auto generation = ++nativeFinishGeneration;
    const std::weak_ptr<ReadingBook *> guard = alive;
    bridge->finishReading([guard, generation](bool saved) {
        if (const auto book = guard.lock())
            (*book)->finishNativeReading(generation, saved);
    });
}
void ReadingBook::finishNativeReading(quint64 generation, bool saved) {
    if (generation != nativeFinishGeneration)
        return;
    const auto callbacks = std::exchange(nativeFinishCallbacks, {});
    for (const auto &callback : callbacks)
        callback(saved);
}
bool ReadingBook::readingSavePending() const {
    return bridge && bridge->readingSavePending();
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
    if (!activeReading.isValid()) {
        if (sleepingReading) {
            const auto now = utc();
            if (sleepingTimeout > 0 && now >= sleepingAt && now - sleepingAt >= sleepingTimeout) {
                if (history)
                    history->stop(completedReadingMilliseconds, model->state().value("position").toString());
                sessionBaseline = completedReadingMilliseconds;
            }
        }
        sleepingReading = false;
        activeReading.start();
        if (history) {
            history->start(completedReadingMilliseconds, model->state().value("position").toString());
            checkpointReadingHistory(true);
        }
    }
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
void ReadingBook::sleepReading(const QJsonObject &preferences) {
    if (!sleepingReading) {
        sleepingReading = true;
        sleepingAt = utc();
        sleepingTimeout = sessionTimeout::milliseconds(preferences);
    }
    stopReading();
}
void ReadingBook::stopReading(bool background, bool nativeAllowed) {
    if (activeReading.isValid()) {
        const auto elapsed = activeReading.elapsed();
        completedReadingMilliseconds += elapsed;
        activeReading.invalidate();
    }
    // Freeze the duration before history work so both use the same boundary.
    if (history) {
        const auto position = model->state().value("position").toString();
        if (sleepingReading)
            history->pause(completedReadingMilliseconds, position);
        else
            history->stop(completedReadingMilliseconds, position);
    }
    // End Nickel's interval before local storage work, so its clock does not
    // include that I/O and its asynchronous flush can run alongside our save.
    if (bridge && nativeAllowed)
        bridge->stopReading();
    checkpointReadingHistory(background);
    if (speed) {
        speed->pause();
        if (!background)
            saveSpeed(true);
    }
}
bool ReadingBook::checkpointReadingHistory(bool background) {
    if (!history)
        return true;
    if (activeReading.isValid())
        history->update(readingMilliseconds(), model->state().value("position").toString());
    return history->save(background);
}
void ReadingBook::saveSpeed(bool force) {
    if (!speed || !speed->dirty() || (!force && speedSaved.isValid() && speedSaved.elapsed() < 60000)) {
        return;
    }
    speedSaved.start();
    if (!speedWriter)
        speedWriter = std::make_unique<storage::RecordWriter>();
    const bool saved = speedWriter->writeNow([directory = paths_.directory, path = paths_.readingSpeed(),
                                         state = speed->state()] {
        // Rewritten every minute while reading; a checkpoint syncs it.
        storage::RoutineWrites routine;
        return storage::writeRecord(directory, "reading-speed", path, state);
    });
    if (saved) {
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
    if (!bridge) {
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
    // Unsupported mappings intentionally stay local. Storage failures do not.
    nativeWriteback = error.isEmpty() ? "failed" : "local-only";
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
ReadingBook::NativeSave ReadingBook::nativeSaveResult() const {
    if (reconciliation.pending())
        return NativeSave::Conflict;
    if (nativeWriteback == "verified" || nativeWriteback == "time-only")
        return NativeSave::Saved;
    if (nativeWriteback == "local-only")
        return NativeSave::LocalOnly;
    return NativeSave::Failed;
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
QJsonObject ReadingBook::checkpointState() const {
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
        state["percent"] = pageProgressPercent(model->page(), model->pages());
    state["native_writeback"] = nativeWriteback;
    state["sync_pending"] = reconciliation.pending();
    state["native_writeback_error"] = nativeWritebackError;
    return state;
}
QJsonObject ReadingBook::persistedCheckpoint(const QJsonObject &state) const {
    auto checkpoint = preservedState;
    for (auto it = state.begin(); it != state.end(); ++it)
        checkpoint[it.key()] = it.value();
    if (dictionaryChoice_.isEmpty())
        checkpoint.remove("dictionary_id");
    if (model->temporaryTypography())
        appearance::copy(checkpoint, model->persistentState());
    // These describe the displayed frame, not a saved reading position. Drop
    // them from imported records too, before comparing checkpoints or applying
    // the record's byte limit. Diagnostic snapshots above keep the full frame.
    for (const auto *key : {"chapter_title", "chapter_page", "chapter_pages", "page", "pages", "next_page",
                            "text_font_face", "embedded_font_count", "cached_pages"})
        checkpoint.remove(key);
    return checkpoint;
}
bool ReadingBook::checkpoint(QJsonObject *snapshot, bool background) {
    saveSpeed();
    const auto state = checkpointState();
    *snapshot = state;
    const auto checkpoint = persistedCheckpoint(state);
    if (closeCheckpointPending)
        return false;
    if (checkpoint == savedCheckpoint || (background && checkpoint == pendingCheckpoint))
        return true;
    // Unchanged checkpoints need no disk write. The interval timer and pause
    // boundaries save time even when the position has not changed. A history
    // failure must not block an independently saved position or dictionary.
    if (!background) checkpointReadingHistory();
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
                // Reading checkpoints follow each other closely. The next synced
                // commit, sleep, standby or close puts this one on storage.
                storage::RoutineWrites routine;
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
    // The profile database has one writer thread. A synchronous checkpoint
    // replaces a queued one there and waits for its result.
    if (!writer)
        writer = std::make_unique<storage::RecordWriter>();
    pendingCheckpoint = {};
    const bool saved = writer->writeNow([paths = paths_, identity = identity_, persisted, path = statePath] {
        return storage::writeBookState(paths, identity, persisted, path);
    });
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
void ReadingBook::checkpointForClose(std::function<void(bool)> done) {
    if (sleepingReading) {
        sleepingReading = false;
        stopReading(true);
    }
    Q_ASSERT(!closeCheckpointPending);
    closeCheckpointPending = true;
    const auto state = checkpointState();
    const auto checkpoint = persistedCheckpoint(state);
    auto persisted = checkpoint;
    for (const auto *key : appearance::layoutKeys)
        persisted.remove(key);
    for (const auto *key : appearance::marginKeys)
        persisted.remove(key);
    const bool rememberAppearance = !model->temporaryTypography();
    const bool writeSpeed = speed && speed->dirty();
    const auto speedState = writeSpeed ? speed->state() : QJsonObject();
    checkpointReadingHistory(true);
    pendingCheckpoint = {};
    if (!writer)
        writer = std::make_unique<storage::RecordWriter>();
    const std::weak_ptr<ReadingBook *> guard = alive;
    writer->post(
        [paths = paths_, identity = identity_, persisted, path = statePath, state, rememberAppearance,
         writeSpeed, speedState] {
            bool ok = storage::writeBookState(paths, identity, persisted, path);
            if (writeSpeed)
                ok = storage::writeRecord(paths.directory, "reading-speed", paths.readingSpeed(), speedState) && ok;
            if (rememberAppearance) {
                const auto settings = storage::readPreferences(paths.preferences());
                if (settings.status == storage::ReadStatus::Ok || settings.status == storage::ReadStatus::Missing) {
                    auto next = settings.object;
                    appearance::rememberTextLayout(next, state);
                    appearance::rememberPageMargins(next, state);
                    if (next != settings.object)
                        ok = storage::writePreferences(paths.preferences(), next) && ok;
                }
            }
            // An unchanged book record can leave an earlier routine save in
            // WAL. Complete its sync before declaring the close saved.
            if (storage::profileHasUnsyncedWrites())
                storage::checkpointProfile();
            return ok && !storage::profileHasUnsyncedWrites();
        },
        [guard, checkpoint, done = std::move(done)](bool saved) {
            kobalt::qt::invoke(QCoreApplication::instance(), [guard, checkpoint, saved, done] {
                if (const auto book = guard.lock())
                    (*book)->finishCloseCheckpoint(checkpoint, saved, done);
            }, Qt::QueuedConnection);
        });
}
void ReadingBook::finishCloseCheckpoint(const QJsonObject &checkpoint, bool saved, std::function<void(bool)> done) {
    if (history && history->saving()) {
        const std::weak_ptr<ReadingBook *> guard = alive;
        QTimer::singleShot(10, QCoreApplication::instance(), [guard, checkpoint, saved, done] {
            if (const auto book = guard.lock())
                (*book)->finishCloseCheckpoint(checkpoint, saved, done);
        });
        return;
    }
    saved = (!history || history->savesSucceeded()) && saved;
    closeCheckpointPending = false;
    if (saved) {
        savedCheckpoint = checkpoint;
        if (speed)
            speed->saved();
    } else {
        diagnostic("local-close-save-failed");
    }
    done(saved);
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

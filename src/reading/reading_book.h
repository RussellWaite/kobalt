#pragma once
#include "reading_pace.h"
#include "reading_speed.h"
#include "reading_progress.h"
#include "storage/reader_storage.h"
#include "storage/record_writer.h"
#include "reading_bridge.h"
#include "reading_timer.h"
#include <QElapsedTimer>
#include <QJsonObject>
#include <memory>
#include <utility>

namespace kobalt {
class ReaderSession;

// Owns the open engine facade and its local/native progress. The window decides
// when to present a conflict; this object decides which progress may be saved.
// Like Nickel, a book opens as reading: it is saved as finished only after it
// is finished again or a finished position from sync is accepted.
class ReadingBook {
public:
    ReadingBook(std::unique_ptr<ReaderSession> session, QString statePath, storage::Paths paths, QString identity,
                QByteArray digest, std::unique_ptr<ReadingBridge> bridge, ReadingProgress baseline,
                ReadingProgress diskBaseline);
    ~ReadingBook();
    ReaderSession *session() const {
        return model.get();
    }
    const QString &identity() const {
        return identity_;
    }
    const storage::Paths &paths() const {
        return paths_;
    }
    const QByteArray &fingerprint() const {
        return digest;
    }
    bool deviceTransition() const;
    bool canWriteNative() const;
    // A sideloaded EPUB file that is not a kepub.
    bool plainEpub() const {
        return identity_.startsWith("file://") && identity_.endsWith(".epub", Qt::CaseInsensitive) &&
               !identity_.endsWith(".kepub.epub", Qt::CaseInsensitive);
    }
    void releaseNative();
    void observeReading(bool learnPace = true);
    void stopReading();
    bool usesLocalPace() const {
        return bool(speed);
    }
    void pauseLocalPace() {
        if (speed) {
            speed->pause();
        }
    }
    // Active time for this opening, excluding sleep and hidden reader time.
    qint64 readingMilliseconds() const;
    bool readingActive() const {
        return activeReading.isValid();
    }
    ReadingEstimates estimates() const;
    void onEstimatesChanged(std::function<void()> changed) {
        if (bridge) {
            bridge->estimatesChanged = std::move(changed);
        }
    }
    // Session fallback for native bridges without a reading estimate.
    ReadingPace &pace() {
        return pace_;
    }
    bool offerFinish() const;
    void markFinished();
    void pollProgress();
    void invalidateProgress() {
        progressPoll.invalidate();
    }
    ReadingProgress readProgress() const;
    void observeProgress();
    bool pendingProgress() const {
        return reconciliation.pending();
    }
    ReadingProgress candidate() const {
        return reconciliation.candidate();
    }
    void acknowledge(const ReadingProgress &shown, bool moved);
    // False leaves a sync conflict pending for the window to resolve.
    bool writeNative(const QString &location, const QString &mappingError);
    // A background checkpoint is queued on the book's writer thread and
    // returns at once; a failure is reported through onCheckpointFailed. A
    // synchronous checkpoint first drops a queued one and waits for a running
    // one, so an older position cannot replace it.
    bool checkpoint(QJsonObject *snapshot, bool background = false);
    bool savesPending() const;
    void onCheckpointFailed(std::function<void()> failed) {
        checkpointFailed = std::move(failed);
    }
    const QJsonObject &layoutPreferences() const {
        return layoutPreferences_;
    }
    const QString &dictionaryChoice() const {
        return dictionaryChoice_;
    }
    bool saveDictionaryChoice(const QString &id);

private:
    ReadingActivity readingActivity() const;
    void saveSpeed(bool force = false);
    std::unique_ptr<ReaderSession> model;
    QString statePath, identity_;
    storage::Paths paths_;
    QByteArray digest;
    std::unique_ptr<ReadingBridge> bridge;
    bool released = false;
    ReadingProgress baseline;
    ProgressReconciler reconciliation;
    QElapsedTimer progressPoll;
    ReadingTimer activeReading;
    qint64 completedReadingMilliseconds = 0;
    ReadingPace pace_;
    std::unique_ptr<ReadingSpeed> speed;
    QElapsedTimer speedSaved;
    QString nativeWriteback = "not-attempted", nativeWritebackError, acceptedFinishedAnchor;
    // The last checkpoint that reached storage. An identical one is not written again.
    QJsonObject savedCheckpoint, layoutPreferences_;
    // The background checkpoint that has not reported back yet.
    QJsonObject pendingCheckpoint;
    std::function<void()> checkpointFailed;
    void finishBackgroundCheckpoint(const QJsonObject &checkpoint, bool saved);
    // A writer holds one waiting write, so each record has its own: a speed
    // record must not replace a waiting position. The destructor resets both
    // after their last writes have finished.
    std::unique_ptr<storage::RecordWriter> writer, speedWriter;
    std::shared_ptr<ReadingBook *> alive = std::make_shared<ReadingBook *>(this);
    QString dictionaryChoice_;
};
}

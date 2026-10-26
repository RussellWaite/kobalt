#pragma once
#include <QString>
#include <QByteArray>
#include <QVector>
#include <functional>
#include <memory>
#include "record_writer.h"

namespace kobalt {
// Durable intervals use the reader's cumulative active clock for duration and
// UTC only for chronology. A clock adjustment must not change reading time.
class ReadingHistory {
public:
    using UtcClock = std::function<qint64()>;
    ReadingHistory(QString directory, QString identity, QByteArray fingerprint, QString sourceKey,
                   QString title, QString author, UtcClock utc = {});
    ~ReadingHistory();
    void start(qint64 activeMilliseconds, const QString &position);
    void update(qint64 activeMilliseconds, const QString &position);
    void stop(qint64 activeMilliseconds, const QString &position);
    // Failed writes retain every pending interval for the next attempt.
    // Periodic checkpoints queue an immutable snapshot. Lifecycle boundaries
    // wait for the latest snapshot; replacing a queued one preserves intervals.
    bool save(bool background = false);
    bool saving() const;
    class Profile;
    // Keep this handle for the app's profile lifetime. Controllers in the same
    // process share it; another process cannot recover or start a reader here.
    // Failed acquisition returns a diagnostic code through error.
    static std::shared_ptr<Profile> openProfile(const QString &directory, QString *error = nullptr);


private:
    static bool recover(const QString &directory);
    void acknowledge();
    struct Interval {
        QString id, startPosition, endPosition, status;
        qint64 started = 0, observed = 0, milliseconds = 0;
    };
    QString directory, identity, book, sourceKey, title, author;
    QByteArray fingerprint;
    UtcClock utc;
    QVector<Interval> pending;
    qint64 baseline = 0;
    bool dirty = false;
    quint64 generation = 0, posted = 0, acknowledged = 0;
    struct Saved;
    std::shared_ptr<Saved> saved;
    // Destroy first: writes own snapshots and results until this has drained.
    std::unique_ptr<storage::RecordWriter> writer;
};
}
